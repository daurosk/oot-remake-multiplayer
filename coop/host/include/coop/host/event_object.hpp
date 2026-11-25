// Event objects (sync plan D4 / S6; G1: issues 251, 243, 200, 144, 86; row 228).
//
// One owned, epoch-numbered object per running event, so "does this event bind MY Link / my actors" is one lookup
// (EventFollower::HoldsLink, Members) and an event happens once, for everyone, in one order, whoever moves away,
// pauses, dies, leaves or joins in the middle. event_owner.hpp answers "whose event runs in OUR game" from the
// game's own facts of one frame; this file is the shared half: who owns the event, which phase it is in, who takes
// it over and when everybody is let go.
//
// The rules (each one checked by the model in tests/host/event_object_test.cpp):
// - One authority, EventTable (the server; pure, no sockets). The games send asks (Start, Phase, Reward, End) and
//   what happens to a player (Enter, Leave, Away). The table answers with the WHOLE state of the object at every
//   change (EventState). The highest `version` of an id wins, so a state that comes late, twice or out of order
//   changes nothing, and one that never comes is healed by the next one or by Snapshot (sent to a player that
//   enters, rejoins or reconnects: an event it still holds and the snapshot does not list is over for it).
// - A second Start of a key that runs joins that run: never two runs, never two rewards.
// - Only the owner's asks with the object's current epoch move it. The epoch goes up at every change of owner, so an
//   old owner's late ask is refused.
// - The owner goes away (leaves, loads, pauses, dies) or stalls (a phase longer than its max_ms): the lowest-numbered
//   member that can act takes the event over at the phase its resume rule names. Nobody can (or kMaxEventHandovers
//   are used up; a stalled owner then just keeps it): the give-up rule ends it: with its result when the reward was given (a reward is never undone)
//   or the kind has none and the phase says complete, else cancelled (as never played: no reward, may run again).
// - The reward is granted by the table, once per run, at the first Reward ask of any player that owned the run (the
//   old owner's ask may come after the hand-over: it gave it, so the new owner's run skips past the reward phase).
//   End is refused until the reward is granted.
// - A player that goes away is no member from that moment (its own game lets go at once, EventFollower::LetGo, and
//   the table drops it): no event ever holds a player that is not in it.
// No clock but the caller's `now_us` (server time, sync_clock.hpp), no event names, no distances.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::host {

// The owner changed in this phase: where the new owner goes on.
enum class EventResume : std::uint8_t {
  restart_phase,  // the phase again from its start (a talk page, a cutscene cut)
  next_phase,     // the phase cannot be played twice: the next one
  from_start,     // the whole event again
  finish,         // nothing left worth playing: the event ends
};
// Nobody can take the event over in this phase.
enum class EventGiveUp : std::uint8_t { cancel, complete };
enum class EventStatus : std::uint8_t { running, ended, cancelled };

struct EventPhase {
  std::string_view name;
  EventResume resume = EventResume::restart_phase;
  EventGiveUp give_up = EventGiveUp::cancel;
  bool holds_members = true;   // the members' Links are held in this phase (the owner's always is)
  std::uint32_t max_ms = 0;    // longer without the owner's next ask: the owner stalls; 0: no limit
};

inline constexpr std::size_t kNoReward = static_cast<std::size_t>(-1);
inline constexpr std::uint32_t kMaxEventHandovers = 4;  // per run; then the give-up rule
inline constexpr std::uint8_t kMaxEventPlayers = 16;

struct EventKind {
  std::string_view name;
  std::span<const EventPhase> phases;       // at least one, at most 255
  std::size_t reward_phase = kNoReward;     // the phase in which the game gives the event's reward
  bool shared = false;  // binds every player in the scene, also one that enters while it runs (a story scene)
  bool once = true;     // ended once, it never runs again (cancelled may)
};

inline bool valid(const EventKind& k) {
  return !k.phases.empty() && k.phases.size() <= 255 && (k.reward_phase == kNoReward || k.reward_phase < k.phases.size());
}

// What runs where: the event's name in its scene, and the actor that asked for it (its placement hash; 0 none).
struct EventKey {
  std::string scene;
  std::string name;
  std::uint64_t subject = 0;
  auto operator<=>(const EventKey&) const = default;
  bool operator==(const EventKey&) const = default;
};

inline constexpr std::uint32_t event_bit(std::uint8_t player) {
  return player >= 1 && player <= kMaxEventPlayers ? 1u << (player - 1) : 0u;
}

// The whole object, as the table sends it at every change.
struct EventState {
  std::uint64_t id = 0;        // the table's counter: one per run
  EventKey key;
  std::uint32_t epoch = 0;     // up at every change of owner
  std::uint32_t version = 0;   // up at every change of anything: the highest wins
  std::uint8_t owner = 0;      // player number, 1 to 16
  std::uint8_t phase = 0;
  EventStatus status = EventStatus::running;
  bool rewarded = false;
  bool holds = false;          // this phase holds the members' Links
  std::uint32_t members = 0;   // event_bit of each player whose Link the event binds
  std::uint64_t phase_start_us = 0;  // server time: every game plays the phase from the same moment
  bool member(std::uint8_t player) const { return (members & event_bit(player)) != 0; }
  bool operator==(const EventState&) const = default;
};

class EventTable {
 public:
  using Out = std::vector<EventState>;  // states to send to the event's scene, in order

  // The player is in `scene` and can act (after a join, a load, a rejoin). Send it Snapshot(scene) too.
  void Enter(std::uint8_t p, std::string_view scene, std::uint64_t now_us, Out& out) {
    Drop(p, now_us, out);
    players_[p] = Player{std::string(scene), false};
    Join(p, now_us, out);
  }
  void Leave(std::uint8_t p, std::uint64_t now_us, Out& out) {
    Drop(p, now_us, out);
    players_.erase(p);
  }
  // Paused, down, loading, a menu of its own in front: it cannot own or be held; back with away = false.
  void Away(std::uint8_t p, bool away, std::uint64_t now_us, Out& out) {
    const auto it = players_.find(p);
    if (it == players_.end() || it->second.away == away) return;
    it->second.away = away;
    if (away) Drop(p, now_us, out);
    else Join(p, now_us, out);
  }

  // The player's game started the event. Returns the id of the run it is in now (a run of that key that runs
  // already: joined, not started twice), 0 when refused (not in the scene, away, played once already).
  std::uint64_t Start(std::uint8_t p, const EventKind& kind, const EventKey& key, std::uint64_t now_us, Out& out) {
    if (!valid(kind) || !Able(p, key.scene)) return 0;
    for (auto& [id, e] : events_) {
      if (e.s.key != key) continue;
      if (!e.s.member(p)) {
        e.s.members |= event_bit(p);
        Emit(e, out);
      } else {
        out.push_back(e.s);  // its own start crossed the other's: tell it which run it is in
      }
      return id;
    }
    if (kind.once && played_.count(key)) return 0;
    Run e;
    e.kind = &kind;
    e.s.id = ++next_id_;
    e.s.key = key;
    e.s.epoch = 1;
    e.s.owner = p;
    e.s.members = event_bit(p);
    e.owners = event_bit(p);
    if (kind.shared) {
      for (const auto& [n, pl] : players_) {
        if (Able(n, key.scene)) e.s.members |= event_bit(n);
      }
    }
    SetPhase(e, 0, now_us);
    auto& kept = events_[e.s.id] = e;
    Emit(kept, out);
    return kept.s.id;
  }

  // The owner's game reached `phase` (one step at a time, never back, never past an ungiven reward).
  bool Phase(std::uint8_t p, std::uint64_t id, std::uint32_t epoch, std::uint8_t phase, std::uint64_t now_us, Out& out) {
    Run* e = Owned(p, id, epoch);
    if (!e || phase != e->s.phase + 1 || phase >= e->kind->phases.size()) return false;
    if (e->kind->reward_phase != kNoReward && phase > e->kind->reward_phase && !e->s.rewarded) return false;
    SetPhase(*e, phase, now_us);
    Emit(*e, out);
    return true;
  }

  // The player's game gave the event's reward. True: this ask is THE grant (the caller shares the reward now);
  // false: granted before, or not this player's to give.
  bool Reward(std::uint8_t p, std::uint64_t id, std::uint64_t now_us, Out& out) {
    const auto it = events_.find(id);
    if (it == events_.end()) return false;
    Run& e = it->second;
    const std::size_t at = e.kind->reward_phase;
    if (at == kNoReward || e.s.rewarded || !(e.owners & event_bit(p))) return false;
    if (p == e.s.owner && e.s.phase != at) return false;
    e.s.rewarded = true;
    ++grants_[id];
    if (p != e.s.owner && e.s.phase <= at) {  // the old owner gave it: the new owner's run goes on after it
      if (at + 1 >= e.kind->phases.size()) {
        Finish(e, EventStatus::ended, out);
        return true;
      }
      SetPhase(e, static_cast<std::uint8_t>(at + 1), now_us);
    }
    Emit(e, out);
    return true;
  }

  // The owner's game ended the event in its last phase, the reward (if any) granted.
  bool End(std::uint8_t p, std::uint64_t id, std::uint32_t epoch, Out& out) {
    Run* e = Owned(p, id, epoch);
    if (!e || e->s.phase + 1u != e->kind->phases.size()) return false;
    if (e->kind->reward_phase != kNoReward && !e->s.rewarded) return false;
    Finish(*e, EventStatus::ended, out);
    return true;
  }

  // Once in a while: an owner that sits in a phase past its max_ms stalls and the event is handed on.
  void Tick(std::uint64_t now_us, Out& out) {
    for (const std::uint64_t id : Ids()) {
      const auto it = events_.find(id);
      if (it == events_.end()) continue;
      Run& e = it->second;
      const std::uint64_t max_us = std::uint64_t{e.kind->phases[e.s.phase].max_ms} * 1000;
      if (max_us != 0 && now_us > e.s.phase_start_us && now_us - e.s.phase_start_us > max_us) Unown(e, now_us, out, true);
    }
  }

  // The running events of a scene: for a player that enters it, rejoins or reconnects.
  std::vector<EventState> Snapshot(std::string_view scene) const {
    std::vector<EventState> v;
    for (const auto& [id, e] : events_) {
      if (e.s.key.scene == scene) v.push_back(e.s);
    }
    return v;
  }

  const EventState* Find(std::uint64_t id) const {
    const auto it = events_.find(id);
    return it == events_.end() ? nullptr : &it->second.s;
  }
  std::size_t Running() const { return events_.size(); }
  // For the tests and the digest: rewards granted and ends (ended or cancelled) of a run.
  std::uint32_t Grants(std::uint64_t id) const { return Count(grants_, id); }
  std::uint32_t Ends(std::uint64_t id) const { return Count(ends_, id); }
  std::uint64_t LastId() const { return next_id_; }
  bool Played(const EventKey& key) const { return played_.count(key) != 0; }

 private:
  struct Player {
    std::string scene;
    bool away = false;
  };
  struct Run {
    const EventKind* kind = nullptr;
    EventState s;
    std::uint32_t owners = 0;  // every player that owned this run
    std::uint32_t handovers = 0;
  };

  static std::uint32_t Count(const std::map<std::uint64_t, std::uint32_t>& m, std::uint64_t id) {
    const auto it = m.find(id);
    return it == m.end() ? 0 : it->second;
  }
  bool Able(std::uint8_t p, std::string_view scene) const {
    const auto it = players_.find(p);
    return it != players_.end() && !it->second.away && it->second.scene == scene;
  }
  std::vector<std::uint64_t> Ids() const {
    std::vector<std::uint64_t> ids;
    for (const auto& [id, e] : events_) ids.push_back(id);
    return ids;
  }
  Run* Owned(std::uint8_t p, std::uint64_t id, std::uint32_t epoch) {
    const auto it = events_.find(id);
    if (it == events_.end() || it->second.s.owner != p || it->second.s.epoch != epoch) return nullptr;
    return &it->second;
  }
  void SetPhase(Run& e, std::uint8_t phase, std::uint64_t now_us) {
    e.s.phase = phase;
    e.s.holds = e.kind->phases[phase].holds_members;
    e.s.phase_start_us = now_us;
  }
  void Emit(Run& e, Out& out) {
    ++e.s.version;
    out.push_back(e.s);
  }
  void Finish(Run& e, EventStatus status, Out& out) {
    e.s.status = status;
    e.s.holds = false;
    e.s.members = 0;
    Emit(e, out);
    ++ends_[e.s.id];
    if (status == EventStatus::ended && e.kind->once) played_.insert(e.s.key);
    events_.erase(e.s.id);  // `e` is gone after this line
  }
  void GiveUp(Run& e, Out& out) {
    const bool has_reward = e.kind->reward_phase != kNoReward;
    const bool complete = e.s.rewarded || (!has_reward && e.kind->phases[e.s.phase].give_up == EventGiveUp::complete);
    Finish(e, complete ? EventStatus::ended : EventStatus::cancelled, out);
  }
  // The owner cannot go on: hand over by the phase's resume rule, or give up.
  // `stalled`: the owner is still there but silent: it keeps the event when nobody else can take it (a player
  // alone reads a text for as long as it likes).
  void Unown(Run& e, std::uint64_t now_us, Out& out, bool stalled = false) {
    std::uint8_t next_owner = 0;
    for (std::uint8_t n = 1; n <= kMaxEventPlayers && !next_owner; ++n) {
      if (n != e.s.owner && e.s.member(n) && Able(n, e.s.key.scene)) next_owner = n;
    }
    if (!next_owner || e.handovers >= kMaxEventHandovers) {
      if (stalled) return;
      GiveUp(e, out);
      return;
    }
    const std::size_t size = e.kind->phases.size();
    std::size_t next = e.s.phase;
    switch (e.kind->phases[e.s.phase].resume) {
      case EventResume::restart_phase: break;
      case EventResume::next_phase: next = e.s.phase + 1u; break;
      case EventResume::from_start: next = 0; break;
      case EventResume::finish: next = size; break;
    }
    const std::size_t at = e.kind->reward_phase;
    if (at != kNoReward) {
      if (e.s.rewarded && next <= at) next = at + 1;      // never given twice
      if (!e.s.rewarded && next > at) next = at;          // never skipped
    }
    if (next >= size) {
      Finish(e, EventStatus::ended, out);
      return;
    }
    e.s.owner = next_owner;
    e.owners |= event_bit(next_owner);
    ++e.s.epoch;
    ++e.handovers;
    SetPhase(e, static_cast<std::uint8_t>(next), now_us);
    Emit(e, out);
  }
  // The player can no longer act: out of every event; its own are handed on.
  void Drop(std::uint8_t p, std::uint64_t now_us, Out& out) {
    for (const std::uint64_t id : Ids()) {
      const auto it = events_.find(id);
      if (it == events_.end() || !it->second.s.member(p)) continue;
      Run& e = it->second;
      e.s.members &= ~event_bit(p);
      if (e.s.owner == p) Unown(e, now_us, out);
      else Emit(e, out);
    }
  }
  // The player can act (again): a shared event of its scene binds it.
  void Join(std::uint8_t p, std::uint64_t, Out& out) {
    const auto pl = players_.find(p);
    if (pl == players_.end() || pl->second.away) return;
    for (auto& [id, e] : events_) {
      if (!e.kind->shared || e.s.key.scene != pl->second.scene || e.s.member(p)) continue;
      e.s.members |= event_bit(p);
      Emit(e, out);
    }
  }

  std::map<std::uint8_t, Player> players_;
  std::map<std::uint64_t, Run> events_;
  std::set<EventKey> played_;
  std::map<std::uint64_t, std::uint32_t> grants_;
  std::map<std::uint64_t, std::uint32_t> ends_;
  std::uint64_t next_id_ = 0;
};

// One game's view of the table. Feed it every EventState that comes (any order, twice, late) and every Snapshot;
// it says what our game has to do, each thing once.
class EventFollower {
 public:
  enum class What : std::uint8_t {
    play,       // an event that binds our Link started (or we became a member): play its phase from phase_start_us
    own,        // we are its owner now (a hand-over): go on at its phase and send the asks with its epoch
    phase,      // its phase or owner changed
    let_go,     // we are no member any more, the event goes on without us
    ended,      // over, with its result: let go; a game that did not play it sets the result only
    cancelled,  // over as never played: let go, nothing else
  };
  struct Act {
    What what;
    EventState state;
  };

  explicit EventFollower(std::uint8_t me) : me_(me) {}

  std::vector<Act> Apply(const EventState& s) {
    std::vector<Act> acts;
    if (done_.count(s.id)) return acts;  // over here already: nothing of it counts any more
    const auto it = known_.find(s.id);
    if (it != known_.end() && s.version <= it->second.version) return acts;  // late or twice
    if (s.status != EventStatus::running) {
      if (it != known_.end()) known_.erase(it);
      done_.insert(s.id);
      acts.push_back({s.status == EventStatus::ended ? What::ended : What::cancelled, s});
      return acts;
    }
    const bool was_known = it != known_.end();
    const EventState before = was_known ? it->second : EventState{};
    known_[s.id] = s;
    const bool in_before = was_known && (before.member(me_) || before.owner == me_);
    const bool in_now = s.member(me_) || s.owner == me_;
    if (in_before && !in_now) {
      acts.push_back({What::let_go, s});
    } else if (in_now) {
      if (s.owner == me_ && (!was_known || before.owner != me_ || before.epoch != s.epoch)) {
        acts.push_back({What::own, s});
      } else if (!in_before) {
        acts.push_back({What::play, s});
      } else if (before.phase != s.phase || before.owner != s.owner || before.epoch != s.epoch) {
        acts.push_back({What::phase, s});
      }
    }
    return acts;
  }

  // All the running events of our scene, from the table. One we hold that is not listed is over (its end never
  // reached us): cancelled here. A played event's result comes with the flags, as for any late entrant.
  std::vector<Act> Snapshot(std::span<const EventState> live) {
    std::vector<Act> acts;
    std::set<std::uint64_t> listed;
    for (const EventState& s : live) listed.insert(s.id);
    for (auto it = known_.begin(); it != known_.end();) {
      if (listed.count(it->first)) {
        ++it;
        continue;
      }
      done_.insert(it->first);
      acts.push_back({What::cancelled, it->second});
      it = known_.erase(it);
    }
    for (const EventState& s : live) {
      // LetGo changed our copy without a version: the table's word of the same version stands.
      const auto same = known_.find(s.id);
      if (same != known_.end() && same->second.version == s.version && same->second != s) known_.erase(same);
      for (Act& a : Apply(s)) acts.push_back(std::move(a));
    }
    return acts;
  }

  // Our own game left the scene, loads, pauses or our Link is down: every event lets us go now, without waiting
  // for the table. (Send Away / Enter / Leave too; what comes back is applied as usual.)
  std::vector<Act> LetGo() {
    std::vector<Act> acts;
    for (auto& [id, s] : known_) {
      if (s.member(me_) || s.owner == me_) acts.push_back({What::let_go, s});
      s.members &= ~event_bit(me_);
      if (s.owner == me_) s.owner = 0;
    }
    return acts;
  }
  // A new scene: nothing of the old one is known any more (ids are never used twice).
  void NewScene() {
    known_.clear();
    done_.clear();
  }

  // The lookups of every hold / hide / freeze site.
  bool HoldsLink() const {
    for (const auto& [id, s] : known_) {
      if (s.owner == me_ || (s.holds && s.member(me_))) return true;
    }
    return false;
  }
  bool Owns(std::uint64_t id) const {
    const auto it = known_.find(id);
    return it != known_.end() && it->second.owner == me_;
  }
  const EventState* Find(std::uint64_t id) const {
    const auto it = known_.find(id);
    return it == known_.end() ? nullptr : &it->second;
  }
  const std::map<std::uint64_t, EventState>& Known() const { return known_; }

 private:
  std::uint8_t me_;
  std::map<std::uint64_t, EventState> known_;
  std::set<std::uint64_t> done_;
};

}  // namespace coop::host
