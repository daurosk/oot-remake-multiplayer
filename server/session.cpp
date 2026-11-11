#include "session.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <utility>

namespace coop::server {

using namespace coop::protocol;

namespace {

char ascii_lower(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

// Nicknames are compared without ASCII case, so "Link" and "link" clash.
bool same_name(const std::string& a, const std::string& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

// The codec already refuses empty names and control characters; a name of
// nothing but spaces is refused here.
bool blank_name(const std::string& name) {
  return std::all_of(name.begin(), name.end(), [](char ch) { return ch == ' '; });
}


// For the server's window when a game of another protocol version is refused:
// which side is out of date, and what to do about it.
std::string version_note(std::uint16_t theirs) {
  const std::string other = std::to_string(theirs);
  const std::string mine = std::to_string(kProtocolVersion);
  if (theirs < kProtocolVersion) {
    return "the game speaks protocol " + other + ", older than this server's " + mine +
           ": that player runs Update.bat in their kit";
  }
  return "the game speaks protocol " + other + ", newer than this server's " + mine +
         ": run Update.bat in this kit (the host's)";
}

}  // namespace

std::uint64_t Stats::dropped() const {
  std::uint64_t total = before_welcome + repeated_hello + after_refused + rate_limited +
                        join_dropped + flag_refused + object_refused + damage_dropped +
                        group_refused;
  for (std::uint64_t n : by_error) total += n;
  return total;
}

Session::Session(SessionConfig config) : config_(std::move(config)) {}

bool Session::connected(ConnectionId id, std::uint64_t now_us,
                        std::optional<std::uint32_t> address) {
  if (address && config_.max_half_open_per_address != 0 &&
      half_open_count(*address) >= config_.max_half_open_per_address) {
    ++stats_.half_open_limited;
    return false;
  }
  Connection c;
  c.connected_at = now_us;
  c.last_heard = now_us;
  c.address = address;
  connections_[id] = std::move(c);
  return true;
}

std::size_t Session::half_open_count(std::uint32_t address) const {
  return static_cast<std::size_t>(
      std::count_if(connections_.begin(), connections_.end(), [&](const auto& entry) {
        return entry.second.state == State::awaiting_hello && entry.second.address == address;
      }));
}

void Session::received(ConnectionId id, std::uint8_t channel,
                       std::span<const std::uint8_t> bytes, std::uint64_t now_us) {
  auto it = connections_.find(id);
  if (it == connections_.end()) return;  // the transport's mistake, not the peer's
  Connection& c = it->second;
  c.last_heard = now_us;

  if (c.state == State::closing) {
    ++stats_.after_refused;
    return;
  }

  // A HELLO of another protocol version may not decode at all; its first
  // two body bytes are the version in every version, so refuse it properly.
  if (c.state == State::awaiting_hello) {
    if (auto version = hello_protocol_version(bytes);
        version && *version != kProtocolVersion) {
      refuse(id, c, RefuseReason::wrong_protocol_version, now_us, version_note(*version));
      return;
    }
  }

  Message message;
  Error error = decode(bytes, message);
  if (error == Error::none) error = check_received(message, Sender::client, channel);
  if (error != Error::none) {
    ++stats_.by_error[static_cast<std::size_t>(error)];
    return;
  }

  if (c.state == State::awaiting_hello) {
    if (const auto* hello = std::get_if<Hello>(&message)) {
      on_hello(id, c, *hello, now_us);
    } else {
      ++stats_.before_welcome;
    }
    return;
  }
  on_player_message(id, c, channel, message, now_us);
}

void Session::on_hello(ConnectionId id, Connection& c, const Hello& hello,
                       std::uint64_t now_us) {
  std::string expected = config_.game_build_id;
  if (expected.empty()) {
    expected = player_count() == 0 ? hello.game_build_id : session_game_build_;
  }
  if (auto reason = check_hello(hello, expected)) {
    refuse(id, c, *reason, now_us,
           *reason == RefuseReason::wrong_protocol_version ? version_note(hello.protocol_version)
                                                           : std::string());
    return;
  }
  if (blank_name(hello.nickname)) {
    refuse(id, c, RefuseReason::bad_name, now_us);
    return;
  }
  // A crashed build restarted on the same machine finds its old self still
  // in until the silence timeout. It takes that place when the old one is
  // quiet and from the same address; a live player's name, or one from
  // another address, stays refused, so nobody can push a player out.
  std::uint8_t preferred = 0;
  if (auto holder = holder_of(hello.nickname)) {
    const Connection& old = connections_.at(*holder);
    const bool stale = config_.rejoin_stale_us != 0 && c.address && old.address == c.address &&
                       now_us - old.last_heard >= config_.rejoin_stale_us;
    if (!stale) {
      refuse(id, c, RefuseReason::name_taken, now_us);
      return;
    }
    preferred = old.number;
    ++stats_.rejoined;
    log("player " + std::to_string(old.number) + " rejoins: " + old.nickname);
    actions_.push_back(DisconnectAction{*holder});
    remove_player(*holder, LeaveReason::timed_out, now_us);
  }
  if (player_count() >= kMaxPlayers) {
    refuse(id, c, RefuseReason::server_full, now_us);
    return;
  }

  Welcome welcome;
  welcome.player_number = free_number(preferred);
  welcome.server_time_us = now_us;
  welcome.tick_rate = config_.tick_rate;
  for (const auto& entry : connections_) {
    const Connection& other = entry.second;
    if (other.state == State::joined) welcome.players.push_back({other.number, other.nickname});
  }
  std::sort(welcome.players.begin(), welcome.players.end(),
            [](const PlayerInfo& a, const PlayerInfo& b) { return a.number < b.number; });

  c.state = State::joined;
  c.number = welcome.player_number;
  c.nickname = hello.nickname;
  c.last_ping = now_us;
  c.reliable_budget = 2.0 * config_.reliable_per_second;
  c.unreliable_budget = 2.0 * config_.unreliable_per_second;
  c.budget_time = now_us;
  if (player_count() == 1) session_game_build_ = hello.game_build_id;

  send(id, kChannelSession, welcome);
  send_to_others(id, kChannelSession, PlayerJoined{c.number, c.nickname});
  // The first player in hosts. The joiner learns who hosts, then gets the
  // whole flag table, both on the progress channel and so before any
  // FLAG_STATE relayed after this.
  if (!host_) host_ = id;
  send(id, kChannelProgress, Host{connections_.at(*host_).number});
  send_snapshot(id);
  log("player " + std::to_string(c.number) + " joined: " + c.nickname);
}

void Session::on_player_message(ConnectionId id, Connection& c, std::uint8_t channel,
                                Message& message, std::uint64_t now_us) {
  if (std::holds_alternative<Pong>(message)) return;  // our PING answered
  if (!within_rate(c, channel, now_us)) {
    ++stats_.rate_limited;
    return;
  }
  if (const auto* ping = std::get_if<Ping>(&message)) {
    send(id, kChannelSession, Pong{ping->sender_time_us, now_us});
    return;
  }
  if (std::holds_alternative<Hello>(message)) {
    ++stats_.repeated_hello;
    return;
  }
  // PLAYER_STATUS, CHAT and PLAYER_SAMPLE carry the sender: the server writes
  // it from the connection, never trusting the message. TEST_PAYLOAD has no
  // player field and goes on as it came. check_received has already refused
  // every server-only type, so nothing else reaches here.
  if (auto* j = std::get_if<JoinPartner>(&message)) {
    on_join_partner(id, c, *j);
    return;
  }
  if (std::holds_alternative<FlagSet>(message)) {
    on_flag_set(c, message);
    return;
  }
  if (std::holds_alternative<FlagState>(message) || std::holds_alternative<FlagReset>(message)) {
    on_flag_state(id, c, message);
    return;
  }
  if (const auto* part = std::get_if<FlagSnapshot>(&message)) {
    on_flag_snapshot(id, c, *part);
    return;
  }
  if (const auto* present = std::get_if<ObjectPresent>(&message)) {
    on_object_present(id, c, *present, now_us);
    return;
  }
  if (auto* d = std::get_if<Damage>(&message)) {
    on_damage(id, c, *d, now_us);
    return;
  }
  if (const auto* group = std::get_if<ObjectGroup>(&message)) {
    on_object_group(id, c, *group);
    return;
  }
  if (auto* cut = std::get_if<GrassCut>(&message)) {
    on_grass_cut(id, c, *cut, now_us);
    return;
  }
  if (std::holds_alternative<WorldBreak>(message) || std::holds_alternative<WorldMove>(message) ||
      std::holds_alternative<WorldPlace>(message) || std::holds_alternative<WorldHit>(message) ||
      std::holds_alternative<SignCut>(message)) {
    on_world(id, c, channel, message, now_us);
    return;
  }
  if (std::holds_alternative<ObjectSpawn>(message) ||
      std::holds_alternative<ObjectSample>(message) ||
      std::holds_alternative<ObjectRemove>(message) ||
      std::holds_alternative<HandoverState>(message)) {
    on_object_message(id, c, channel, message, now_us);
    return;
  }
  // Where the player is and whether its game runs, for the ownership rules.
  // The place of a sample from a scene it has just left is not its place.
  if (const auto* sample = std::get_if<PlayerSample>(&message)) {
    const bool here = !sample->scene || (!stale_scene(c.sample_scene, c, *sample->scene) &&
                                         set_scene(id, c, *sample->scene, now_us));
    if (here && sample->position) {
      c.position = *sample->position;
      c.position_at = now_us;
    }
  } else if (const auto* st = std::get_if<PlayerStatus>(&message)) {
    on_status(c, st->flags, now_us);
  }
  if (sender_of(message)) set_sender(message, c.number);
  send_to_others(id, channel, message);
  ++stats_.relayed;
}

// To the named player only. The codec has made sure the player field is 0
// and the other player is 1 to 4.
void Session::on_join_partner(ConnectionId id, Connection& c, JoinPartner& j) {
  if (j.other_player == c.number) {  // set_sender refuses it too
    ++stats_.join_dropped;
    return;
  }
  auto target = connection_of(j.other_player);
  if (!target) {
    if (j.kind != JoinKind::request) {  // an answer to someone who left
      ++stats_.join_dropped;
      return;
    }
    JoinPartner answer;
    answer.player_number = j.other_player;
    answer.other_player = c.number;
    answer.request_id = j.request_id;
    answer.kind = JoinKind::refuse;
    answer.reason = JoinRefuseReason::not_there;
    send(id, kChannelSession, answer);
    ++stats_.join_not_there;
    return;
  }
  Message m = std::move(j);
  set_sender(m, c.number);
  send(*target, kChannelSession, m);
  ++stats_.join_relayed;
  ++stats_.relayed;
}

// To the host only, the host itself included: a host's own FLAG_SET comes
// back to it, so its build answers every FLAG_SET the same way.
void Session::on_flag_set(Connection& c, Message& message) {
  set_sender(message, c.number);
  send(*host_, kChannelProgress, message);
  ++stats_.flag_relayed;
  ++stats_.relayed;
}

void Session::on_flag_state(ConnectionId id, Connection& c, Message& message) {
  if (id != host_) {
    ++stats_.flag_refused;
    return;
  }
  // FLAG_RESET (the host's save load) is kept the same way: each value
  // replaces the table's, 0s included.
  const auto& flags = std::holds_alternative<FlagReset>(message) ? std::get<FlagReset>(message).flags
                                                                  : std::get<FlagState>(message).flags;
  for (const FlagEntry& e : flags) keep_flag(e);
  if (std::holds_alternative<FlagReset>(message)) on_host_reset();
  set_sender(message, c.number);
  send_to_others(id, kChannelProgress, message);
  ++stats_.flag_relayed;
  ++stats_.relayed;
}

void Session::on_host_reset() {
  ++stats_.host_resets;
  // Owners of session-wide kept state add their clear here (docs/flags.md, "Saves").
  world_.forget_kept();  // locked doors' kept breaks (docs/dungeon.md, "Locked doors")
  dead_.clear();         // kills-late: the save's enemies are alive again
}

// A host's table, merged in part by part. A flag the server lacks is kept and
// sent to everyone else as FLAG_STATE. On a clash the server's value wins (it
// is what every other player already has, and it may hold progress made
// while this player was away): the host is sent it as FLAG_STATE, which it
// applies like any other.
void Session::on_flag_snapshot(ConnectionId id, Connection& c, const FlagSnapshot& part) {
  if (id != host_) {
    ++stats_.flag_refused;
    return;
  }
  ++stats_.snapshot_parts;
  FlagState added{c.number, {}};
  FlagState kept{c.number, {}};
  for (const FlagEntry& e : part.flags) {
    auto it = flags_.find(e.key);
    if (it == flags_.end()) {
      // Sent on even past max_flags, as a FLAG_STATE past it would be.
      keep_flag(e);
      added.flags.push_back(e);
    } else if (!(it->second == e)) {
      ++stats_.flag_clashes;
      kept.flags.push_back(it->second);
    }
  }
  if (!added.flags.empty()) send_to_others(id, kChannelProgress, added);
  if (!kept.flags.empty()) send(id, kChannelProgress, kept);
}

bool Session::keep_flag(const FlagEntry& e) {
  const FlagKey key = e.key;
  auto it = flags_.find(key);
  if (it != flags_.end()) {
    it->second = e;
    return true;
  }
  // Never more than a snapshot can carry (65535 parts), so every joiner
  // gets the whole table whatever max_flags says.
  constexpr std::size_t kSnapshotLimit = 65535 * kMaxFlagEntries;
  if (flags_.size() >= std::min(config_.max_flags, kSnapshotLimit)) {
    ++stats_.flag_table_full;
    return false;
  }
  flags_.emplace(key, e);
  return true;
}

void Session::send_snapshot(ConnectionId to) {
  std::vector<FlagEntry> table;
  table.reserve(flags_.size());
  for (const auto& entry : flags_) table.push_back(entry.second);
  // Cannot fail: keep_flag holds the table to 65535 parts.
  auto parts = split_snapshot(table);
  if (!parts) return;
  for (const FlagSnapshot& p : *parts) send(to, kChannelProgress, p);
}

// ---------------------------------------------------------------- objects
// docs/objects.md "Enemies" and docs/protocol.md "Objects". The first game to
// list a placed object owns it; only the owner of the current epoch is
// relayed, to the players in the object's scene; DAMAGE goes to the owner
// alone. An object changes owner (epoch + 1, the state first, then the news)
// when its owner lets go of it, cannot play, or another player is the one
// near it; a fight keeps it where it is. Projectiles stay with their maker
// and go when it lets go of them.

namespace {

// Bits of PLAYER_STATUS that keep a player from being given an object.
constexpr std::uint8_t kBusy = status::loading | status::paused | status::dead;

constexpr std::uint8_t kFighting = object_presence::in_fight | object_presence::targeting;

double metres(const Vec3& a, const Vec3& b) {
  double sum = 0;
  for (std::size_t i = 0; i < 3; ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

// A time that is never 0, for fields where 0 means "not set".
std::uint64_t stamp(std::uint64_t now_us) { return std::max<std::uint64_t>(now_us, 1); }

}  // namespace

void Session::on_status(Connection& c, std::uint8_t flags, std::uint64_t now_us) {
  auto since = [&](std::uint8_t bit, std::uint64_t& when) {
    if (!(flags & bit)) {
      when = 0;
    } else if (when == 0) {
      when = stamp(now_us);
    }
  };
  since(status::paused, c.paused_since);
  since(status::dead, c.dead_since);
  c.status = flags;
}

// PLAYER_SAMPLE and OBJECT_PRESENT each say the scene, from different threads
// of the game, and cross at a scene change. A stream that changes its scene
// is believed at once; one that only repeats what it said before, while the
// other has moved on, is late and is not.
bool Session::stale_scene(std::string& said, const Connection& c, const std::string& scene) {
  const bool repeat = scene == said;
  said = scene;
  if (!repeat || scene == c.scene) return false;
  ++stats_.stale_scenes;
  return true;
}

bool Session::set_scene(ConnectionId id, Connection& c, const std::string& scene,
                        std::uint64_t now_us) {
  if (c.scene == scene) return true;
  const std::set<ObjectId> present = std::exchange(c.present, {});
  const std::string left = std::exchange(c.scene, scene);
  c.position.reset();  // the old scene's place means nothing here
  c.replay_due = true;
  c.owners_told_at = now_us;
  drop_all_objects(id, c.number, present, now_us);
  // Its game unloads the scene: none of its dying copies is listed there
  // any more, so coming back with the enemy on its list is a respawn.
  if (auto ds = dead_.find(left); ds != dead_.end()) {
    for (auto& [oid, d] : ds->second) {
      if (auto w = d.witnesses.find(id); w != d.witnesses.end()) w->second = false;
    }
  }
  forget_empty_grass(left);
  forget_empty_world(left);
  forget_empty_dead(left);
  // What was broken and moved here before this player came (docs/world.md).
  for (const Message& m : world_.replay(scene)) {
    send(id, kChannelSession, m);
    ++stats_.world_replayed;
  }
  // The grass the others cut here before this player came.
  if (auto it = grass_.find(scene); it != grass_.end()) {
    for (const GrassCut& cut : it->second) {
      send(id, kChannelSession, cut);
      ++stats_.grass_replayed;
    }
  }
  return true;
}

// A made enemy is announced once, when it is made, and a placed one that
// other games may lack once, by its owner. A player who enters the scene
// after that gets them here, at their last relayed state.
void Session::replay_spawns(ConnectionId id, const Connection& c) {
  for (const auto& [oid, o] : objects_) {
    if (o.actor.empty() || o.removed || !o.owner || *o.owner == id || o.scene != c.scene) {
      continue;
    }
    auto owner = connections_.find(*o.owner);
    if (owner == connections_.end()) continue;
    const std::uint8_t number = owner->second.number;
    send(id, kChannelOwnership,
         ObjectSpawn{number, oid, o.actor, o.spawn_flags, o.last_state.value_or(ObjectState{})});
    // A spawn implies the maker (or the sender) at epoch 0; say so when it
    // is otherwise.
    if (o.epoch != 0 || (o.made && number != oid.maker)) {
      send(id, kChannelOwnership, owner_message(oid, o));
    }
    ++stats_.spawns_replayed;
  }
}

void Session::on_object_present(ConnectionId id, Connection& c, const ObjectPresent& p,
                                std::uint64_t now_us) {
  if (stale_scene(c.present_scene, c, p.scene) || !set_scene(id, c, p.scene, now_us)) return;
  if (c.replay_due) {
    c.replay_due = false;
    replay_spawns(id, c);
  }
  const bool refresh = config_.owner_refresh_us != 0 &&
                       now_us - c.owners_told_at >= config_.owner_refresh_us;
  if (refresh) {
    c.owners_told_at = now_us;
    ++stats_.owners_refreshed;
  }

  std::set<ObjectId> listed;
  for (const ObjectPresence& e : p.objects) listed.insert(e.id);
  const std::set<ObjectId> before = c.present;
  for (const ObjectId& oid : before) {
    if (listed.count(oid)) continue;
    c.present.erase(oid);
    if (auto ds = dead_.find(p.scene); ds != dead_.end()) {
      if (auto d = ds->second.find(oid); d != ds->second.end()) {
        if (auto w = d->second.witnesses.find(id); w != d->second.witnesses.end()) {
          w->second = false;  // its dying (or told) copy is gone from this game
        }
      }
    }
    auto it = objects_.find(oid);
    if (it != objects_.end() && it->second.made && it->second.owner == id) {
      // Its OBJECT_REMOVE is probably right behind: run_ownership lets go
      // of it when none comes.
      it->second.present.erase(id);
      if (it->second.orphan_since == 0) it->second.orphan_since = stamp(now_us);
      continue;
    }
    drop_presence(id, c.number, oid, now_us);
  }

  for (const ObjectPresence& e : p.objects) {
    const ObjectId& oid = e.id;
    if (oid.kind == ObjectKind::placed && dead_for(id, c, p.scene, oid)) {
      c.present.insert(oid);  // so its leaving the list is seen
      continue;
    }
    auto it = objects_.find(oid);
    if (it == objects_.end()) {
      if (oid.kind != ObjectKind::placed) continue;  // made ones come by OBJECT_SPAWN
      if (objects_.size() >= config_.max_objects) {
        ++stats_.object_table_full;
        continue;
      }
      Object& o = objects_[oid];
      o.scene = p.scene;
      o.serial = next_serial_++;
      o.present.insert(id);
      o.owner = id;
      c.present.insert(oid);
      if (e.flags & kFighting) mark_fight(o, now_us);
      announce_owner(oid, o);
      continue;
    }
    Object& o = it->second;
    // The same hash in another scene is not ours; a projectile has no copy
    // that could take it.
    if (o.scene != p.scene || o.projectile) continue;
    c.present.insert(oid);
    const bool fresh = o.present.insert(id).second;
    if (o.owner == id) {
      o.orphan_since = 0;
      if (e.flags & kFighting) mark_fight(o, now_us);
    } else if (e.flags & object_presence::targeting) {
      o.chased_by.try_emplace(id, now_us);  // its copy chases this player
    } else {
      o.chased_by.erase(id);
    }
    // Tell a newcomer who owns it and the epoch to send with; its own made
    // enemy it knows.
    if (refresh || (fresh && !(o.made && o.owner == id))) {
      send(id, kChannelOwnership, owner_message(oid, o));
    }
  }
}

void Session::on_object_message(ConnectionId id, Connection& c, std::uint8_t channel,
                                Message& message, std::uint64_t now_us) {
  // The owner's answer to HANDOVER_ASK. Not relayed as it came: the new
  // owner gets it from finish_handover with the new epoch.
  if (const auto* handed = std::get_if<HandoverState>(&message)) {
    auto it = objects_.find(handed->id);
    if (it == objects_.end() || it->second.owner != id || !it->second.ask ||
        it->second.epoch != handed->epoch) {
      ++stats_.object_refused;
      return;
    }
    finish_handover(handed->id, it->second, &handed->state, now_us);
    return;
  }

  ObjectId oid;
  if (const auto* spawn = std::get_if<ObjectSpawn>(&message);
      spawn && spawn->id.kind == ObjectKind::placed) {
    // The owner of a placed object names its actor, for games that do not
    // have it loaded (a Gold Skulltula only one game shook out of a tree).
    // Relayed to the scene, and kept for players who enter later.
    oid = spawn->id;
    auto it = objects_.find(oid);
    if (it == objects_.end() || it->second.owner != id || it->second.scene != c.scene) {
      ++stats_.object_refused;
      return;
    }
    Object& o = it->second;
    o.actor = spawn->actor;
    o.spawn_flags = spawn->flags;
    if (!o.last_state) o.last_state = spawn->state;
  } else if (spawn) {
    oid = spawn->id;
    if (spawn->id.maker != c.number || objects_.count(oid)) {
      ++stats_.object_refused;
      return;
    }
    if (objects_.size() >= config_.max_objects) {
      ++stats_.object_table_full;
      return;
    }
    Object& o = objects_[oid];
    o.scene = c.scene;
    o.serial = next_serial_++;
    o.owner = id;
    o.made = true;
    o.projectile = (spawn->flags & object_spawn::projectile) != 0;
    o.actor = spawn->actor;
    o.spawn_flags = spawn->flags;
    o.last_state = spawn->state;
  } else if (const auto* sample = std::get_if<ObjectSample>(&message)) {
    oid = sample->id;
    auto it = objects_.find(oid);
    if (it == objects_.end() || it->second.owner != id || it->second.epoch != sample->epoch) {
      ++stats_.object_refused;
      return;
    }
    Object& o = it->second;
    const bool life_changed = sample->state.life && o.last_state && o.last_state->life &&
                              sample->state.life->life != o.last_state->life->life;
    const bool says_fight =
        sample->state.state && (*sample->state.state & object_state::in_fight) != 0;
    if (life_changed || says_fight) mark_fight(o, now_us);
    // The owner's own hit: its life fell with no DAMAGE from another player
    // just before (issue 150, boss_owner_hit_hold_us).
    if (life_changed && sample->state.life->life < o.last_state->life->life &&
        (o.hit_at == 0 || now_us - o.hit_at > config_.hit_window_us)) {
      o.owner_hit_at = stamp(now_us);
    }
    o.last_state = sample->state;
  } else {
    auto& remove = std::get<ObjectRemove>(message);
    oid = remove.id;
    auto it = objects_.find(oid);
    if (it == objects_.end()) {
      ++stats_.object_refused;
      return;
    }
    Object& o = it->second;
    const bool owner_now = o.owner == id && o.epoch == remove.epoch;
    // A kill that crossed a hand-over: the old owner's enemy died before it
    // heard it was no longer the owner. It dies for everyone.
    const bool late = !owner_now && o.owner && o.previous_owner == id &&
                      remove.reason == RemoveReason::dead &&
                      static_cast<std::uint16_t>(remove.epoch + 1) == o.epoch &&
                      now_us - o.changed_at <= config_.late_remove_us;
    if (!owner_now && !late) {
      ++stats_.object_refused;
      return;
    }
    if (late) {
      ++stats_.late_removes;
      remove.epoch = o.epoch;  // receivers drop older epochs
    }
    const std::string scene = o.scene;
    if (o.made) {
      forget(it);
    } else {
      if (remove.reason == RemoveReason::dead) {
        // Kept for the games that come later (dead_for), up to a cap a scene.
        auto& kills = dead_[scene];
        if (kills.size() >= kMaxDeadPerScene && !kills.count(oid)) {
          ++stats_.dead_unkept;
        } else {
          Dead& d = kills[oid];
          d.witnesses.clear();
          for (const ConnectionId w : o.present) d.witnesses[w] = true;
          d.witnesses[id] = false;  // its game dropped it from its walk
          d.position = remove.position;
          d.by = c.number;
        }
      }
      // Kept, owned by nobody, until no game has it loaded, so a copy still
      // listed cannot bring it back under a new owner.
      o.removed = true;
      o.owner.reset();
      o.last_state.reset();
      o.ask.reset();
      o.candidate.reset();
    }
    set_sender(message, c.number);
    send_to_scene(scene, id, channel, message);
    ++stats_.relayed;
    ++stats_.object_relayed;
    return;
  }
  set_sender(message, c.number);
  send_to_scene(objects_.at(oid).scene, id, channel, message);
  ++stats_.relayed;
  ++stats_.object_relayed;
}

// To the owner alone, whatever epoch the hitter knew: the hit was real. A
// hitter who is the one near the enemy, with its owner well away, then takes
// it: the enemy fights whoever hits it, and only its owner's game moves it.
// Two players in the same fight are never that far apart, so their hits do
// not move it.
void Session::on_damage(ConnectionId id, Connection& c, Damage& d, std::uint64_t now_us) {
  auto it = objects_.find(d.id);
  if (it == objects_.end() || !it->second.owner || *it->second.owner == id) {
    ++stats_.damage_dropped;
    return;
  }
  Object& o = it->second;
  d.player_number = c.number;
  send(*o.owner, kChannelOwnership, d);
  ++stats_.relayed;
  ++stats_.damage_relayed;
  mark_fight(o, now_us);
  if (o.hit_at == 0 || now_us - o.hit_at > config_.hit_window_us) {
    o.hit_base.reset();
    if (o.last_state && o.last_state->life) o.hit_base = o.last_state->life->life;
    o.hit_sum = 0;
  }
  o.hit_sum += d.amount;
  o.hit_at = stamp(now_us);

  // A boss goes to the player who hurt it (issue 150), the whole group with
  // its root: the hit's effect is in that game, the others follow its pose.
  ObjectId root_id = d.id;
  Object* root = &o;
  if (o.root) {
    if (auto r = objects_.find(*o.root); r != objects_.end()) {
      root_id = r->first;
      root = &r->second;
    }
  }
  if ((o.boss || root->boss) && config_.boss_hitter_cooldown_us != 0) {
    if (root->owner && *root->owner != id && !root->ask && !(c.status & kBusy) &&
        has_loaded(id, *root) &&
        (root->changed_at == 0 || now_us - root->changed_at >= config_.boss_hitter_cooldown_us) &&
        !owner_hit_lately(*root, now_us) && !owner_hit_lately(o, now_us)) {
      ask_handover(root_id, *root, id, now_us);
      ++stats_.boss_to_hitter;
    }
    return;  // a boss is not moved by the distance rule below
  }

  if (o.ask || o.root || o.projectile || (c.status & kBusy) || !has_loaded(id, o)) return;
  if (o.changed_at != 0 && now_us - o.changed_at < config_.handover_cooldown_us) return;
  auto owner = connections_.find(*o.owner);
  if (owner == connections_.end()) return;
  auto mine = distance_to(c, o, now_us);
  // Where the hit landed is where the hitter's copy stands. A copy that
  // chased the hitter stands far from the owner's enemy (issue 101), and
  // the hitter is the one near the enemy that fights it.
  const bool at_known = d.position != Vec3{} && std::isfinite(d.position[0]) &&
                        std::isfinite(d.position[1]) && std::isfinite(d.position[2]);
  if (at_known && c.position &&
      !(now_us > c.position_at && now_us - c.position_at > config_.position_stale_us)) {
    const double at = metres(*c.position, d.position);
    if (!mine || at < *mine) mine = at;
  }
  const auto theirs = distance_to(owner->second, o, now_us);
  if (mine && theirs && *mine <= config_.handover_near_m &&
      *mine + config_.handover_margin_m <= *theirs) {
    ask_handover(d.id, o, id, now_us);
  }
}

// Field grass (docs/world.md "Field grass"): kept per scene and relayed to
// the players in it. A cut the scene already has (the same place and size,
// bit for bit) is not passed on, so a cut can never go round between games.
void Session::on_grass_cut(ConnectionId id, Connection& c, GrassCut& cut,
                           std::uint64_t /*now_us*/) {
  // A cut never moves the player: only its samples and OBJECT_PRESENT do. A
  // late one from the scene it has left is still a cut there, kept and passed
  // on. One for a scene nobody is in is dropped: that grass has grown back.
  if (!scene_has_player(cut.scene)) {
    ++stats_.world_unkept;
    return;
  }
  cut.player_number = c.number;
  auto& cuts = grass_[cut.scene];
  const auto same = [&cut](const GrassCut& o) {
    return std::memcmp(o.position.data(), cut.position.data(), sizeof(float) * 3) == 0 &&
           std::memcmp(&o.radius, &cut.radius, sizeof(float)) == 0;
  };
  if (std::any_of(cuts.begin(), cuts.end(), same)) {
    ++stats_.grass_repeated;
    return;
  }
  if (cuts.size() >= kMaxGrassCuts) cuts.pop_front();
  cuts.push_back(cut);
  send_to_scene(cut.scene, id, kChannelSession, cut);
  ++stats_.relayed;
  ++stats_.grass_relayed;
}

// Shared world objects (docs/world.md): breaks and rest places are kept per
// scene in world_ and replayed by set_scene; moves are only passed on. All
// three go to the players in the sender's scene.
void Session::on_world(ConnectionId id, Connection& c, std::uint8_t channel, Message& message,
                       std::uint64_t /*now_us*/) {
  set_sender(message, c.number);
  // As for grass cuts: these never move the player, and one for a scene
  // nobody is in is not kept (the games there have reset it).
  if (auto* b = std::get_if<WorldBreak>(&message)) {
    if (!scene_has_player(b->scene)) {
      ++stats_.world_unkept;
      return;
    }
    if (!world_.on_break(*b)) {
      ++stats_.world_repeated;
      // A lock a second player opened too: the others must hear of it, so
      // the one who spent the second key gives it back (docs/dungeon.md).
      if ((b->flags & world_break::kept) == 0) return;
    }
    send_to_scene(b->scene, id, channel, message);
  } else if (auto* p = std::get_if<WorldPlace>(&message)) {
    if (!scene_has_player(p->scene)) {
      ++stats_.world_unkept;
      return;
    }
    world_.on_place(*p);
    send_to_scene(p->scene, id, channel, message);
  } else if (auto* h = std::get_if<WorldHit>(&message)) {
    if (!scene_has_player(h->scene)) {
      ++stats_.world_unkept;
      return;
    }
    world_.on_hit(*h);
    send_to_scene(h->scene, id, channel, message);
  } else if (auto* k = std::get_if<SignCut>(&message)) {
    if (!scene_has_player(k->scene)) {
      ++stats_.world_unkept;
      return;
    }
    world_.on_sign_cut(*k);
    send_to_scene(k->scene, id, channel, message);
  } else if (auto* v = std::get_if<WorldMove>(&message)) {
    if (v->scene != c.scene) return;  // late, from the scene it left
    send_to_scene(v->scene, id, channel, message);
  }
  ++stats_.relayed;
  ++stats_.world_relayed;
}

bool Session::scene_has_player(const std::string& scene) const {
  if (scene.empty()) return false;
  for (const auto& [id, c] : connections_) {
    if (c.state == State::joined && c.scene == scene) return true;
  }
  return false;
}

void Session::forget_empty_world(const std::string& scene) {
  if (!scene_has_player(scene)) world_.forget(scene);
}

void Session::forget_empty_grass(const std::string& scene) {
  if (!scene_has_player(scene)) grass_.erase(scene);
}

void Session::forget_empty_dead(const std::string& scene) {
  if (!scene_has_player(scene)) dead_.erase(scene);  // the games load it anew
}

// Issue 133: a kill that a game entering later must get. A game that did not
// see the enemy die is told now, once; one whose dying copy is still listed
// is left alone; a witness that lists it again after it left its walk has
// respawned it (its room loaded again), so it is alive again for everyone.
bool Session::dead_for(ConnectionId id, Connection& c, const std::string& scene,
                       const ObjectId& oid) {
  auto ds = dead_.find(scene);
  if (ds == dead_.end()) return false;
  auto d = ds->second.find(oid);
  if (d == ds->second.end()) return false;
  auto w = d->second.witnesses.find(id);
  if (w == d->second.witnesses.end()) {
    d->second.witnesses.emplace(id, true);
    send(id, kChannelOwnership,
         ObjectRemove{d->second.by, oid, 0, RemoveReason::dead, d->second.position});
    ++stats_.dead_told;
    log("told player " + std::to_string(c.number) + " a kill from before it came");
    return true;
  }
  if (w->second) return true;
  ds->second.erase(d);
  ++stats_.dead_respawned;
  return false;
}

// Not relayed. A member follows its root's owner (run_ownership), shares its
// fight and is never moved alone.
void Session::on_object_group(ConnectionId id, Connection& c, const ObjectGroup& g) {
  for (const ObjectGroupEntry& e : g.entries) {
    auto m = objects_.find(e.member);
    auto r = objects_.find(e.root);
    // Both known, in the sender's scene, and the member loaded by the sender.
    if (m == objects_.end() || r == objects_.end() || m->second.scene != c.scene ||
        r->second.scene != c.scene ||
        !(m->second.owner == id || m->second.present.count(id) != 0)) {
      ++stats_.group_refused;
      continue;
    }
    Object& member = m->second;
    // A root that follows another: its own root is the group's.
    const ObjectId top = (e.member != e.root && r->second.root) ? *r->second.root : e.root;
    if (e.member != e.root && top == e.member) {  // a loop
      ++stats_.group_refused;
      continue;
    }
    ++stats_.group_links;
    member.boss = (e.flags & object_group::boss) != 0;
    const std::uint32_t before = group_of(member);
    if (top == e.member) {
      unlink(e.member, member);
    } else if (member.root != top) {
      unlink(e.member, member);
      Object& root = objects_.at(top);
      const std::uint32_t root_before = group_of(root);
      // What followed the member follows its new root.
      for (const ObjectId& child : std::exchange(member.members, {})) {
        auto ci = objects_.find(child);
        if (ci == objects_.end()) continue;
        ci->second.root = top;
        root.members.insert(child);
        announce_owner(child, ci->second);
      }
      member.root = top;
      root.members.insert(e.member);
      root.fight_until = std::max(root.fight_until, member.fight_until);
      if (group_of(root) != root_before) announce_owner(top, root);
    }
    if (group_of(member) != before) announce_owner(e.member, member);
  }
}

void Session::drop_all_objects(ConnectionId id, std::uint8_t number,
                               const std::set<ObjectId>& present, std::uint64_t now_us) {
  for (const ObjectId& oid : present) drop_presence(id, number, oid, now_us);
  std::vector<ObjectId> made;
  for (const auto& [oid, o] : objects_) {
    if (o.made && o.owner == id) made.push_back(oid);
  }
  for (const ObjectId& oid : made) drop_presence(id, number, oid, now_us);
}

void Session::drop_presence(ConnectionId id, std::uint8_t number, const ObjectId& oid,
                            std::uint64_t now_us) {
  auto it = objects_.find(oid);
  if (it == objects_.end()) return;
  Object& o = it->second;
  o.present.erase(id);
  o.chased_by.erase(id);
  if (o.candidate == id) o.candidate.reset();
  if (o.made) {
    if (o.owner != id) return;
    // A made enemy goes on with a player who has its copy. A projectile, or
    // an enemy nobody else has, is gone on every screen.
    std::optional<ConnectionId> next;
    if (!o.projectile) next = best_other(o, id, false, now_us);
    if (next) {
      change_owner(oid, o, next, number, now_us);
      return;
    }
    ObjectRemove r{number, oid, o.epoch, RemoveReason::gone, {}};
    if (o.last_state && o.last_state->position) r.position = *o.last_state->position;
    const std::string scene = o.scene;
    forget(it);
    send_to_scene(scene, id, kChannelOwnership, r);
    return;
  }
  if (o.present.empty()) {
    forget(it);  // nobody has it loaded: forgotten
    return;
  }
  if (o.owner == id) change_owner(oid, o, best_other(o, id, false, now_us), number, now_us);
}

bool Session::has_loaded(ConnectionId id, const Object& o) const {
  auto it = connections_.find(id);
  return it != connections_.end() && it->second.state == State::joined &&
         it->second.scene == o.scene && o.present.count(id) != 0;
}

std::optional<double> Session::distance_to(const Connection& c, const Object& o,
                                           std::uint64_t now_us) const {
  if (!c.position || !o.last_state || !o.last_state->position) return std::nullopt;
  if (now_us > c.position_at && now_us - c.position_at > config_.position_stale_us) {
    return std::nullopt;
  }
  return metres(*c.position, *o.last_state->position);
}

bool Session::unfit_owner(const Connection& c, std::uint64_t now_us) const {
  if (c.status & status::loading) return true;
  if (c.paused_since != 0 && now_us - c.paused_since >= config_.owner_paused_us) return true;
  return c.dead_since != 0 && now_us - c.dead_since >= config_.owner_dead_us;
}

std::optional<ConnectionId> Session::best_other(const Object& o,
                                                std::optional<ConnectionId> except,
                                                bool fit_only, std::uint64_t now_us) const {
  std::optional<ConnectionId> best;
  std::optional<double> best_distance;
  std::uint8_t best_number = 0;
  for (ConnectionId other : o.present) {
    if (other == except || !has_loaded(other, o)) continue;
    const Connection& oc = connections_.at(other);
    if (fit_only && (oc.status & kBusy)) continue;
    const auto d = distance_to(oc, o, now_us);
    bool better = !best;
    if (best) {
      if (d.has_value() != best_distance.has_value()) {
        better = d.has_value();  // a known place before an unknown one
      } else if (d && *d != *best_distance) {
        better = *d < *best_distance;
      } else {
        better = oc.number < best_number;
      }
    }
    if (better) {
      best = other;
      best_distance = d;
      best_number = oc.number;
    }
  }
  return best;
}

bool Session::owner_hit_lately(const Object& o, std::uint64_t now_us) const {
  return o.owner_hit_at != 0 && now_us - o.owner_hit_at < config_.boss_owner_hit_hold_us;
}

void Session::mark_fight(Object& o, std::uint64_t now_us) {
  Object* target = &o;
  if (o.root) {
    if (auto r = objects_.find(*o.root); r != objects_.end()) target = &r->second;
  }
  const bool boss = o.boss || target->boss;
  const std::uint64_t until =
      now_us + (boss ? config_.boss_fight_lock_us : config_.fight_lock_us);
  target->fight_until = std::max(target->fight_until, until);
}

bool Session::in_fight(const Object& o, std::uint64_t now_us) const {
  const Object* target = &o;
  if (o.root) {
    if (auto r = objects_.find(*o.root); r != objects_.end()) target = &r->second;
  }
  return target->fight_until > now_us;
}

void Session::ask_handover(const ObjectId& oid, Object& o, ConnectionId to,
                           std::uint64_t now_us) {
  auto target = connections_.find(to);
  if (!o.owner || target == connections_.end()) return;
  o.ask = Object::Ask{to, now_us};
  o.candidate.reset();
  send(*o.owner, kChannelOwnership, HandoverAsk{oid, o.epoch, target->second.number});
  ++stats_.handover_asked;
}

void Session::finish_handover(const ObjectId& oid, Object& o, const ObjectState* answer,
                              std::uint64_t now_us) {
  if (!o.ask || !o.owner) return;
  const ConnectionId to = o.ask->to;
  o.ask.reset();
  // The player it was for left, changed scene or unloaded it meanwhile: the
  // owner keeps it.
  if (!has_loaded(to, o)) return;
  if (answer) {
    // What the answer lacks (an owner that no longer has the actor sends
    // only the time) stays as last relayed.
    ObjectState s = *answer;
    if (o.last_state) {
      const ObjectState& old = *o.last_state;
      if (!s.position) s.position = old.position;
      if (!s.velocity) s.velocity = old.velocity;
      if (!s.rotation) s.rotation = old.rotation;
      if (!s.animation) s.animation = old.animation;
      if (!s.life) s.life = old.life;
    }
    o.last_state = std::move(s);
  }
  // A hit passed on to the old owner just before: its answer may have been
  // read before the game took the life off. The new owner must not get the
  // life back.
  if (o.last_state && o.last_state->life && o.hit_base && o.hit_at != 0 &&
      now_us - o.hit_at <= config_.hit_window_us) {
    const std::int64_t cap = std::max<std::int64_t>(0, std::int64_t{*o.hit_base} - o.hit_sum);
    if (o.last_state->life->life > cap) o.last_state->life->life = static_cast<std::int32_t>(cap);
  }
  std::uint8_t old_owner = 0;
  if (auto it = connections_.find(*o.owner); it != connections_.end()) {
    old_owner = it->second.number;
  }
  change_owner(oid, o, to, old_owner, now_us);
}

void Session::change_owner(const ObjectId& oid, Object& o, std::optional<ConnectionId> next,
                           std::uint8_t old_owner, std::uint64_t now_us) {
  o.previous_owner = o.owner;
  o.owner = next;
  o.last_owner = old_owner;
  ++o.epoch;
  o.changed_at = stamp(now_us);
  o.owner_hit_at = 0;  // the new owner has not hit it yet
  o.candidate.reset();
  o.ask.reset();
  o.orphan_since = 0;
  if (next) o.chased_by.erase(*next);
  ++stats_.owner_changes;
  // The state first, then the news: the new owner has it before it starts.
  if (next && o.last_state) {
    send(*next, kChannelOwnership, HandoverState{old_owner, oid, o.epoch, *o.last_state});
  }
  announce_owner(oid, o);
}

void Session::run_ownership(std::uint64_t now_us) {
  std::vector<ObjectId> ids;
  ids.reserve(objects_.size());
  for (const auto& entry : objects_) ids.push_back(entry.first);
  for (const ObjectId& oid : ids) {
    auto it = objects_.find(oid);
    if (it == objects_.end()) continue;
    Object& o = it->second;
    if (o.removed || !o.owner) continue;
    const ConnectionId owner = *o.owner;
    auto owner_it = connections_.find(owner);
    if (owner_it == connections_.end()) continue;
    const Connection& oc = owner_it->second;

    if (o.orphan_since != 0) {
      if (now_us - o.orphan_since >= config_.orphan_us) {
        o.orphan_since = 0;
        drop_presence(owner, oc.number, oid, now_us);  // may forget it
      }
      continue;
    }
    if (o.ask) {
      if (now_us - o.ask->at >= config_.handover_answer_us) {
        ++stats_.handover_unanswered;
        finish_handover(oid, o, nullptr, now_us);
      }
      continue;
    }
    if (o.projectile) continue;

    // A group member goes where its root is, fight or not.
    if (o.root) {
      auto r = objects_.find(*o.root);
      if (r != objects_.end() && r->second.owner && r->second.owner != o.owner &&
          !r->second.ask && has_loaded(*r->second.owner, o)) {
        ask_handover(oid, o, *r->second.owner, now_us);
      }
      continue;
    }

    // An owner whose game stands still gives it up, fight or not.
    if (unfit_owner(oc, now_us)) {
      if (auto next = best_other(o, owner, true, now_us)) ask_handover(oid, o, *next, now_us);
      continue;
    }

    // A boss in a fight: the player clearly nearer it than its owner (issue
    // 150). Its AI chases only its owner's Link, so the near one gets it.
    if (o.boss && config_.boss_near_hold_us != 0 && in_fight(o, now_us)) {
      std::optional<ConnectionId> want;
      const bool may = (o.changed_at == 0 ||
                        now_us - o.changed_at >= config_.boss_hitter_cooldown_us) &&
                       !owner_hit_lately(o, now_us);
      const auto theirs = may ? distance_to(oc, o, now_us) : std::nullopt;
      if (theirs) {
        double best = 0;
        for (ConnectionId other : o.present) {
          if (other == owner || !has_loaded(other, o)) continue;
          const Connection& pc = connections_.at(other);
          if (pc.status & kBusy) continue;
          const auto mine = distance_to(pc, o, now_us);
          if (!mine || *mine > config_.boss_near_m ||
              *mine + config_.boss_near_margin_m > *theirs) {
            continue;
          }
          if (!want || *mine < best) {
            want = other;
            best = *mine;
          }
        }
      }
      if (!want) {
        o.candidate.reset();
      } else {
        if (o.candidate != want) {
          o.candidate = want;
          o.candidate_since = now_us;
        }
        if (now_us - o.candidate_since >= config_.boss_near_hold_us) {
          ++stats_.boss_to_near;
          ask_handover(oid, o, *want, now_us);
        }
      }
      continue;
    }

    const bool settled =
        o.changed_at == 0 || now_us - o.changed_at >= config_.handover_cooldown_us;
    if (!settled || in_fight(o, now_us)) {
      o.candidate.reset();
      continue;
    }

    // Out of a fight: a game whose copy chases its own Link (issue 101). The
    // enemy's AI chases only its own game's Link, so that game is the only
    // one whose enemy goes where it should. A group's root takes its
    // members' chases as its own.
    if (auto chaser = chased_by(o, owner, now_us)) {
      ++stats_.handover_chased;
      ask_handover(oid, o, *chaser, now_us);
      continue;
    }

    // Out of a fight: the player who is the one near it, held for a while.
    std::optional<ConnectionId> want;
    const auto theirs = distance_to(oc, o, now_us);
    if (theirs) {
      double best = 0;
      for (ConnectionId other : o.present) {
        if (other == owner || !has_loaded(other, o)) continue;
        const Connection& pc = connections_.at(other);
        if (pc.status & kBusy) continue;
        const auto mine = distance_to(pc, o, now_us);
        if (!mine || *mine > config_.handover_near_m ||
            *mine + config_.handover_margin_m > *theirs) {
          continue;
        }
        if (!(*theirs > config_.handover_owner_far_m || *mine <= config_.handover_chase_m)) {
          continue;
        }
        if (!want || *mine < best) {
          want = other;
          best = *mine;
        }
      }
    }
    if (!want) {
      o.candidate.reset();
      continue;
    }
    if (o.candidate != want) {
      o.candidate = want;
      o.candidate_since = now_us;
    }
    if (now_us - o.candidate_since >= config_.handover_hold_us) {
      ask_handover(oid, o, *want, now_us);
    }
  }
}

std::optional<ConnectionId> Session::chased_by(const Object& o, ConnectionId owner,
                                               std::uint64_t now_us) const {
  std::optional<ConnectionId> best;
  std::uint64_t best_since = 0;
  std::uint8_t best_number = 0;
  auto consider = [&](const Object& from) {
    for (const auto& [other, since] : from.chased_by) {
      if (other == owner || !has_loaded(other, o)) continue;
      if (now_us < since || now_us - since < config_.handover_chased_us) continue;
      const Connection& pc = connections_.at(other);
      if (pc.status & kBusy) continue;
      // The longest chase first, then the lower player number.
      if (!best || since < best_since || (since == best_since && pc.number < best_number)) {
        best = other;
        best_since = since;
        best_number = pc.number;
      }
    }
  };
  consider(o);
  for (const ObjectId& m : o.members) {
    if (auto it = objects_.find(m); it != objects_.end()) consider(it->second);
  }
  return best;
}

std::uint32_t Session::group_of(const Object& o) const {
  if (o.root) {
    auto r = objects_.find(*o.root);
    return r == objects_.end() ? 0 : r->second.serial;
  }
  return o.members.empty() ? 0 : o.serial;
}

ObjectOwner Session::owner_message(const ObjectId& oid, const Object& o) const {
  std::uint8_t owner = 0;
  if (o.owner) {
    auto it = connections_.find(*o.owner);
    if (it != connections_.end()) owner = it->second.number;
  }
  return ObjectOwner{oid, owner, o.epoch, group_of(o)};
}

void Session::unlink(const ObjectId& oid, Object& o) {
  if (!o.root) return;
  auto r = objects_.find(*o.root);
  if (r != objects_.end()) r->second.members.erase(oid);
  o.root.reset();
}

void Session::forget(Objects::iterator it) {
  const ObjectId oid = it->first;
  unlink(oid, it->second);
  const std::set<ObjectId> members = std::move(it->second.members);
  objects_.erase(it);
  // What followed it is on its own again.
  for (const ObjectId& m : members) {
    auto mi = objects_.find(m);
    if (mi == objects_.end()) continue;
    mi->second.root.reset();
    announce_owner(m, mi->second);
  }
}

void Session::announce_owner(const ObjectId& oid, const Object& o) {
  send_to_scene(o.scene, std::nullopt, kChannelOwnership, owner_message(oid, o));
}

void Session::send_to_scene(const std::string& scene, std::optional<ConnectionId> except,
                            std::uint8_t channel, const Message& m) {
  std::vector<std::uint8_t> bytes;
  if (encode(m, bytes) != Error::none) return;
  for (const auto& [id, c] : connections_) {
    if (id == except || c.state != State::joined) continue;
    if (!scene.empty() && c.scene != scene) continue;
    actions_.push_back(SendAction{id, channel, bytes});
  }
}

std::optional<std::pair<std::uint8_t, std::uint16_t>> Session::object_owner(
    const ObjectId& id) const {
  auto it = objects_.find(id);
  if (it == objects_.end()) return std::nullopt;
  return std::make_pair(owner_message(id, it->second).owner, it->second.epoch);
}

std::optional<std::uint32_t> Session::object_group(const ObjectId& id) const {
  auto it = objects_.find(id);
  if (it == objects_.end()) return std::nullopt;
  return group_of(it->second);
}

bool Session::object_in_fight(const ObjectId& id, std::uint64_t now_us) const {
  auto it = objects_.find(id);
  return it != objects_.end() && in_fight(it->second, now_us);
}

std::optional<ConnectionId> Session::connection_of(std::uint8_t number) const {
  for (const auto& [id, c] : connections_) {
    if (c.state == State::joined && c.number == number) return id;
  }
  return std::nullopt;
}

bool Session::within_rate(Connection& c, std::uint8_t channel, std::uint64_t now_us) {
  double seconds = static_cast<double>(now_us - c.budget_time) / 1e6;
  c.budget_time = now_us;
  auto refill = [seconds](double& budget, std::uint32_t per_second) {
    budget = std::min(budget + seconds * per_second, 2.0 * per_second);
  };
  refill(c.reliable_budget, config_.reliable_per_second);
  refill(c.unreliable_budget, config_.unreliable_per_second);
  double& budget = channel <= kChannelOwnership ? c.reliable_budget : c.unreliable_budget;
  if (budget < 1.0) return false;
  budget -= 1.0;
  return true;
}

void Session::refuse(ConnectionId id, Connection& c, RefuseReason reason,
                     std::uint64_t now_us, const std::string& why) {
  send(id, kChannelSession, Refused{reason});
  actions_.push_back(DisconnectAction{id});
  c.state = State::closing;
  c.refused_at = now_us;
  log("refused a connection, reason " + std::to_string(static_cast<int>(reason)) +
      (why.empty() ? std::string() : ": " + why));
}

void Session::disconnected(ConnectionId id, std::uint64_t now_us) {
  auto it = connections_.find(id);
  if (it == connections_.end()) return;
  bool silent = now_us - it->second.last_heard >= config_.silence_timeout_us / 2;
  remove_player(id, silent ? LeaveReason::timed_out : LeaveReason::left, now_us);
}

void Session::remove_player(ConnectionId id, LeaveReason reason, std::uint64_t now_us) {
  auto it = connections_.find(id);
  if (it == connections_.end()) return;
  Connection c = std::move(it->second);
  connections_.erase(it);
  if (c.state != State::joined) return;
  send_to_others(id, kChannelSession, PlayerLeft{c.number, reason});
  // After PLAYER_LEFT, so the others learn the owner left before the new
  // owner is announced; the leaver is out of connections_ by now.
  drop_all_objects(id, c.number, c.present, now_us);
  forget_empty_grass(c.scene);
  forget_empty_world(c.scene);
  forget_empty_dead(c.scene);
  log("player " + std::to_string(c.number) + " left: " + c.nickname +
      (reason == LeaveReason::timed_out ? " (timed out)" : ""));
  if (player_count() == 0) {
    session_game_build_.clear();
    host_.reset();
    flags_.clear();
    objects_.clear();
    grass_.clear();
    world_.clear();
    dead_.clear();
  } else if (host_ == id) {
    // The remaining player with the lowest number takes over. Everyone is
    // told on the progress channel, so the new host learns it hosts before
    // any FLAG_SET relayed to it.
    std::optional<ConnectionId> next;
    for (const auto& [other, oc] : connections_) {
      if (oc.state == State::joined && (!next || oc.number < connections_.at(*next).number)) {
        next = other;
      }
    }
    host_ = next;
    const std::uint8_t number = connections_.at(*host_).number;
    send_to_others(id, kChannelProgress, Host{number});
    log("player " + std::to_string(number) + " hosts now");
  }
}

void Session::tick(std::uint64_t now_us) {
  std::vector<ConnectionId> late_hello;  // not a player: dropped without PLAYER_LEFT
  std::vector<ConnectionId> silent;
  for (auto& [id, c] : connections_) {
    if (c.state == State::awaiting_hello &&
        now_us - c.connected_at >= config_.hello_timeout_us) {
      late_hello.push_back(id);
    } else if (c.state == State::closing &&
               now_us - c.refused_at >= config_.refused_grace_us) {
      late_hello.push_back(id);  // refused, but the transport never closed it
    } else if (c.state == State::joined) {
      if (now_us - c.last_heard >= config_.silence_timeout_us) {
        silent.push_back(id);
      } else if (now_us - c.last_ping >= config_.ping_interval_us) {
        c.last_ping = now_us;
        send(id, kChannelSession, Ping{now_us});
      }
    }
  }
  for (ConnectionId id : late_hello) {
    actions_.push_back(DisconnectAction{id});
    remove_player(id, LeaveReason::timed_out, now_us);
  }
  for (ConnectionId id : silent) {
    actions_.push_back(DisconnectAction{id});
    remove_player(id, LeaveReason::timed_out, now_us);
  }
  if (now_us - ownership_checked_at_ >= config_.ownership_check_us) {
    ownership_checked_at_ = now_us;
    run_ownership(now_us);
  }
}

std::vector<Action> Session::take_actions() { return std::exchange(actions_, {}); }

std::size_t Session::player_count() const {
  return static_cast<std::size_t>(std::count_if(
      connections_.begin(), connections_.end(),
      [](const auto& entry) { return entry.second.state == State::joined; }));
}

std::optional<std::uint8_t> Session::host() const {
  if (!host_) return std::nullopt;
  return connections_.at(*host_).number;
}

std::optional<std::uint8_t> Session::player_number(ConnectionId id) const {
  auto it = connections_.find(id);
  if (it == connections_.end() || it->second.state != State::joined) return std::nullopt;
  return it->second.number;
}

std::optional<ConnectionId> Session::holder_of(const std::string& nickname) const {
  for (const auto& [id, c] : connections_) {
    if (c.state == State::joined && same_name(c.nickname, nickname)) return id;
  }
  return std::nullopt;
}

// `preferred` (a rejoiner's old number) when it is free, else the lowest.
std::uint8_t Session::free_number(std::uint8_t preferred) const {
  auto used = [&](std::uint8_t n) {
    return std::any_of(connections_.begin(), connections_.end(), [&](const auto& entry) {
      return entry.second.state == State::joined && entry.second.number == n;
    });
  };
  if (preferred >= 1 && preferred <= kMaxPlayers && !used(preferred)) return preferred;
  for (std::uint8_t n = 1; n <= kMaxPlayers; ++n) {
    if (!used(n)) return n;
  }
  return 0;  // unreachable: on_hello refuses a full session first
}

void Session::send(ConnectionId to, std::uint8_t channel, const Message& m) {
  SendAction action{to, channel, {}};
  if (encode(m, action.bytes) != Error::none) return;  // our own message; cannot happen
  actions_.push_back(std::move(action));
}

void Session::send_to_others(ConnectionId except, std::uint8_t channel, const Message& m) {
  std::vector<std::uint8_t> bytes;
  if (encode(m, bytes) != Error::none) return;
  for (const auto& [id, c] : connections_) {
    if (id != except && c.state == State::joined) {
      actions_.push_back(SendAction{id, channel, bytes});
    }
  }
}

void Session::log(const std::string& line) const {
  if (config_.log) config_.log(line);
}

}  // namespace coop::server
