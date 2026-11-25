// SPDX-License-Identifier: GPL-3.0-or-later
//
// The room object (sync plan S4 + S7): one state machine for a room's transient state, so every player in a room
// sees the same room, a player who comes later (or rejoins, dies, reloads, lost the connection) gets exactly the
// kept state, and no press is lost or undone. Pure: no network, no game, no clock of its own
// (tests/protocol/room_object_model_test.cpp runs both halves against each other over many seeds).
//
// Who decides what (three kinds of key, one order):
// - hold: a plug that is on while somebody stands on it or holds it. One bit per player (bit N = player N, bit 0
//   the room itself: a block or an enemy on the switch, from the owner). Only player N's own game writes bit N, the
//   server clears it when that player leaves the room. On = any bit. Two players on one switch (row 244) need no
//   rule: each has its own bit.
// - set: a latched value (a torch lit, a hit switch, a block seat, a clock's start time). Any player's own action
//   asks for it; the server takes asks in the order they arrive.
// - derived: what a room's own logic did with no player's press (a timer ran out). Only the owner's game proposes
//   it, with the newest revision it had applied (seen); the server refuses a proposal for a key that changed after
//   that revision, so an owner that had not yet seen a press can never undo it.
// The server is the one place where these meet: it gives every accepted change the next revision and tells every
// member in that order (reliable, in order). So the kept state is the truth, a game's view is that state up to the
// revision it has applied, and the owner matters only for derived values.
//
// Owner (Room::Choose): the lowest-numbered member that can own (a current module, not frozen), sticky: it stays
// until it leaves, freezes or disconnects, never moved by distance or number. Every change of owner, also to
// nobody, is a new epoch; the server takes proposals only from the owner of the current epoch. No hand-over ask:
// every member already holds the kept state, so a frozen or gone owner never blocks the room.
//
// A room with no member forgets its state (every game unloads it; results are flags, sync plan D3). Revisions and
// epochs keep rising across that, so nothing old can be taken for new.
#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::protocol::room {

using Player = std::uint8_t;  // 1 to kMaxPlayers; 0: the room itself (hold bit 0), or nobody (owner)

struct Key {
  std::uint64_t owner = 0;  // a placement hash, or 0 for the room itself (KeyedValue::owner)
  std::uint32_t name = 0;   // Murmur3 of the game's own name for it (KeyedValue::name)
  auto operator<=>(const Key&) const = default;
};

constexpr std::uint32_t Bit(Player p) { return std::uint32_t{1} << p; }

// What is kept of one key. A hold key keeps its bits (value is not used), any other key its value.
struct Cell {
  bool hold = false;
  std::uint32_t holders = 0;
  KeyedData value = std::int32_t{0};
  std::uint32_t rev = 0;  // the revision of its last change
  bool On() const { return holders != 0; }
  bool Same(const Cell& o) const { return hold == o.hold && (hold ? holders == o.holders : value == o.value); }
};
using Cells = std::map<Key, Cell>;

// Server to members, reliable and in order.
struct Merge {  // one accepted change; gone: the key is no longer kept
  std::uint32_t rev = 0;
  Key key;
  bool gone = false;
  Cell cell;
};
struct Owner {  // a new epoch
  std::uint16_t epoch = 0;
  Player owner = 0;
};
struct Snapshot {  // to a player who enters: everything kept
  std::uint16_t epoch = 0;
  Player owner = 0;
  std::uint32_t rev = 0;
  Cells cells;
};
struct ToAll {
  std::variant<Merge, Owner> what;
};
struct ToOne {
  Player to = 0;
  Snapshot snapshot;
};
using Effect = std::variant<ToAll, ToOne>;
using Effects = std::vector<Effect>;

// A game's ask (reliable): its own player's action.
struct Ask {
  Key key;
  bool hold = false;
  bool on = false;                    // hold: this player's bit
  KeyedData value = std::int32_t{0};  // set: the value
};
// The owner's proposal for one key (rides the unreliable sample). remove: the key is over.
struct Proposal {
  Key key;
  bool hold = false;                  // a hold key: the room's own bit (bit 0), on
  bool on = false;
  bool remove = false;                // a set key: it is over
  KeyedData value = std::int32_t{0};  // a set key: the value
};

struct RoomStats {
  std::uint64_t merges = 0;
  std::uint64_t asks_refused = 0;       // a hold from a player who is not in the room, or anything for an empty room
  std::uint64_t proposals_refused = 0;  // not the owner, or an old epoch
  std::uint64_t proposals_stale = 0;    // the key changed after the revision the owner had seen
  std::uint64_t kind_clash = 0;         // a hold ask for a set key or the other way round
};

// The server's half: one per scene that has (or had) a member.
class Room {
 public:
  struct Member {
    bool can_own = true;  // false: an older module, it only asks
    bool frozen = false;  // paused or loading for kFreezeUs: it cannot own, its holds stay
  };

  std::uint16_t epoch() const { return epoch_; }
  Player owner() const { return owner_; }
  std::uint32_t rev() const { return rev_; }
  const Cells& cells() const { return cells_; }
  const std::map<Player, Member>& members() const { return members_; }
  const RoomStats& stats() const { return stats_; }

  // p has the room loaded now (also again: a reload is Leave then Enter). It gets everything kept.
  Effects Enter(Player p, bool can_own = true) {
    Effects out;
    if (members_.count(p)) Drop(p, out);  // a second Enter with no Leave between: a fresh room in that game
    members_[p] = Member{can_own, false};
    Choose(out);
    out.push_back(ToOne{p, Snapshot{epoch_, owner_, rev_, cells_}});
    return out;
  }
  // p left the scene, unloaded it or lost the connection.
  Effects Leave(Player p) {
    Effects out;
    if (!members_.count(p)) return out;
    Drop(p, out);
    Choose(out);
    if (members_.empty()) {  // every game unloaded it: the next one in loads it fresh
      cells_.clear();
      floor_ = ++rev_;
    }
    return out;
  }
  Effects Freeze(Player p, bool frozen) {
    Effects out;
    auto it = members_.find(p);
    if (it == members_.end() || it->second.frozen == frozen) return out;
    it->second.frozen = frozen;
    Choose(out);
    return out;
  }
  Effects OnAsk(Player p, const Ask& a) {
    Effects out;
    // A hold is a player's body in the room. A set from a player who has just left still counts while somebody is
    // there to see it.
    if (members_.empty() || (a.hold && !members_.count(p)) || p == 0 || p > kMaxPlayers) {
      ++stats_.asks_refused;
      return out;
    }
    auto it = cells_.find(a.key);
    if (it != cells_.end() && it->second.hold != a.hold) {
      ++stats_.kind_clash;
      return out;
    }
    if (a.hold) {
      const std::uint32_t before = it == cells_.end() ? 0 : it->second.holders;
      SetHolders(a.key, a.on ? (before | Bit(p)) : (before & ~Bit(p)), out);
    } else {
      SetValue(a.key, a.value, out);
    }
    return out;
  }
  // The owner's sample: its epoch, the newest revision it had applied, its proposals.
  Effects OnSample(Player p, std::uint16_t epoch, std::uint32_t seen, const std::vector<Proposal>& proposals) {
    Effects out;
    if (p == 0 || p != owner_ || epoch != epoch_) {
      ++stats_.proposals_refused;
      return out;
    }
    for (const Proposal& pr : proposals) {
      auto it = cells_.find(pr.key);
      // A key we keep: the owner must have seen its last change. One we do not keep: it must have seen the last
      // time anything was taken away (it may be proposing what it has not yet seen removed).
      if (seen < (it == cells_.end() ? floor_ : it->second.rev)) {
        ++stats_.proposals_stale;
        continue;
      }
      if (it != cells_.end() && it->second.hold != pr.hold) {
        ++stats_.kind_clash;
        continue;
      }
      if (pr.hold) {
        const std::uint32_t before = it == cells_.end() ? 0 : it->second.holders;
        SetHolders(pr.key, pr.on ? (before | Bit(0)) : (before & ~Bit(0)), out);
      } else if (pr.remove) {
        if (it != cells_.end()) Erase(pr.key, out);
      } else {
        SetValue(pr.key, pr.value, out);
      }
    }
    return out;
  }

 private:
  void Tell(const Key& key, bool gone, const Cell& cell, Effects& out) {
    ++stats_.merges;
    out.push_back(ToAll{Merge{rev_, key, gone, cell}});
  }
  void Erase(const Key& key, Effects& out) {
    cells_.erase(key);
    floor_ = ++rev_;
    Tell(key, true, Cell{}, out);
  }
  void SetHolders(const Key& key, std::uint32_t holders, Effects& out) {
    auto it = cells_.find(key);
    if ((it == cells_.end() ? 0 : it->second.holders) == holders) return;  // said twice: nothing new
    if (holders == 0) {
      Erase(key, out);
      return;
    }
    Cell& c = cells_[key];
    c.hold = true;
    c.holders = holders;
    c.rev = ++rev_;
    Tell(key, false, c, out);
  }
  void SetValue(const Key& key, const KeyedData& value, Effects& out) {
    auto it = cells_.find(key);
    if (it != cells_.end() && it->second.value == value) return;
    Cell& c = cells_[key];
    c.hold = false;
    c.value = value;
    c.rev = ++rev_;
    Tell(key, false, c, out);
  }
  void Drop(Player p, Effects& out) {
    members_.erase(p);
    std::vector<std::pair<Key, std::uint32_t>> held;
    for (const auto& [key, c] : cells_) {
      if (c.hold && (c.holders & Bit(p))) held.emplace_back(key, c.holders & ~Bit(p));
    }
    for (const auto& [key, holders] : held) SetHolders(key, holders, out);
  }
  void Choose(Effects& out) {
    auto fit = [&](Player p) {
      auto it = members_.find(p);
      return it != members_.end() && it->second.can_own && !it->second.frozen;
    };
    if (owner_ != 0 && fit(owner_)) return;  // sticky
    Player next = 0;
    for (const auto& [p, m] : members_) {
      if (fit(p)) {
        next = p;  // the map is by number: the first is the lowest
        break;
      }
    }
    if (next == owner_) return;  // nobody before, nobody now
    owner_ = next;
    ++epoch_;
    // The room's own bit was the old owner's game's word: the new owner says its own.
    std::vector<std::pair<Key, std::uint32_t>> own;
    for (const auto& [key, c] : cells_) {
      if (c.hold && (c.holders & Bit(0))) own.emplace_back(key, c.holders & ~Bit(0));
    }
    out.push_back(ToAll{Owner{epoch_, owner_}});
    for (const auto& [key, holders] : own) SetHolders(key, holders, out);
  }

  std::map<Player, Member> members_;
  Cells cells_;
  Player owner_ = 0;
  std::uint16_t epoch_ = 0;
  std::uint32_t rev_ = 0;
  std::uint32_t floor_ = 0;  // the revision of the last time a key was taken away (or all of them)
  RoomStats stats_;
};

// A game's half: its view of the room it has loaded, what its own player does, and what it sends.
class View {
 public:
  explicit View(Player me) : me_(me) {}

  struct Out {
    std::vector<Ask> asks;  // reliable, in order
  };

  bool in() const { return in_; }        // our game has the room loaded
  bool live() const { return live_; }    // and the snapshot of this visit came
  bool is_owner() const { return live_ && owner_ == me_; }
  std::uint16_t epoch() const { return epoch_; }
  Player owner() const { return owner_; }
  std::uint32_t seen() const { return seen_; }
  const Cells& cells() const { return cells_; }
  const std::map<Key, Proposal>& proposals() const { return proposals_; }
  const std::map<Key, bool>& pressing() const { return pressing_; }
  std::uint64_t out_of_order() const { return out_of_order_; }

  // Our game loaded the room (fresh: nothing pressed, nothing proposed) / unloaded it. The caller tells the server.
  void Enter() {
    in_ = true;
    live_ = false;
    cells_.clear();
    pressing_.clear();
    proposals_.clear();
    waiting_.clear();
  }
  void Leave() {
    Enter();
    in_ = false;
  }

  // Our own player's body went on or off a hold plug.
  void Press(const Key& key, bool on, Out& out) {
    if (!in_) return;
    if (on) {
      pressing_[key] = true;
    } else {
      pressing_.erase(key);
    }
    Say(Ask{key, true, on, std::int32_t{0}}, out);
  }
  // Our own player's action latched a value.
  void Set(const Key& key, const KeyedData& value, Out& out) {
    if (!in_) return;
    Say(Ask{key, false, false, value}, out);
  }
  // Our game's own logic changed a key with no player's press. Kept as a proposal only while we own the room; it
  // is state for nobody (us included) until the server's merge comes.
  void Derive(const Key& key, bool remove, const KeyedData& value) {
    if (!is_owner()) return;
    proposals_[key] = Proposal{key, false, false, remove, value};
  }
  // The same for a hold plug something that is no player holds down (a block, an enemy): the room's own bit.
  void DeriveHold(const Key& key, bool on) {
    if (!is_owner()) return;
    proposals_[key] = Proposal{key, true, on, false, std::int32_t{0}};
  }
  // What the owner's next sample carries (every beat until merged or beaten by another change of the key).
  // A proposal the kept state already says is done: the server would tell nobody, so it ends here.
  std::vector<Proposal> Sample() {
    std::vector<Proposal> out;
    if (!is_owner()) return out;
    for (auto it = proposals_.begin(); it != proposals_.end();) {
      const Proposal& p = it->second;
      auto kept = cells_.find(p.key);
      const bool have = kept != cells_.end();
      const bool done = p.hold ? ((have && kept->second.hold && (kept->second.holders & Bit(0)) != 0) == p.on)
                               : (p.remove ? !have : (have && !kept->second.hold && kept->second.value == p.value));
      if (done) {
        it = proposals_.erase(it);
      } else {
        out.push_back(p);
        ++it;
      }
    }
    return out;
  }

  void On(const Snapshot& s, Out& out) {
    if (!in_) return;  // for a visit that is over
    live_ = true;
    epoch_ = s.epoch;
    owner_ = s.owner;
    seen_ = s.rev;
    cells_ = s.cells;
    proposals_.clear();
    // What our player did while we waited goes out now, then our holds as they are (the server dropped our bits
    // when we left; a visit's snapshot can also be an older visit's, the next one puts it right again).
    std::vector<Ask> waiting = std::move(waiting_);
    waiting_.clear();
    for (const Ask& a : waiting) {
      if (!a.hold) out.asks.push_back(a);
    }
    Reconcile(out);
  }
  void On(const Owner& o) {
    if (!live_) return;
    epoch_ = o.epoch;
    owner_ = o.owner;
    proposals_.clear();  // a proposal belongs to the epoch it was made in
  }
  void On(const Merge& m, Out& out) {
    if (!live_) return;
    if (m.rev <= seen_) {  // reliable and in order: only a snapshot of a newer visit can be ahead of a merge
      ++out_of_order_;
      return;
    }
    seen_ = m.rev;
    if (m.gone) {
      cells_.erase(m.key);
    } else {
      cells_[m.key] = m.cell;
    }
    proposals_.erase(m.key);  // merged, or another change won: our logic derives again from the new value
    if (m.gone || m.cell.hold) ReconcileKey(m.key, out);
  }

  // Whether the plug is on in our game: the kept bits, and our own body at once (before its merge comes back).
  bool HoldOn(const Key& key) const {
    auto it = cells_.find(key);
    return pressing_.count(key) != 0 || (it != cells_.end() && it->second.hold && it->second.On());
  }

 private:
  void Say(const Ask& a, Out& out) {
    if (live_) {
      out.asks.push_back(a);
    } else {
      waiting_.push_back(a);  // the server does not know us in this room yet
    }
  }
  bool KeptBit(const Key& key) const {
    auto it = cells_.find(key);
    return it != cells_.end() && it->second.hold && (it->second.holders & Bit(me_)) != 0;
  }
  // The kept bit of ours must say what our body does; an ask is a statement, so saying it twice is harmless.
  void ReconcileKey(const Key& key, Out& out) {
    const bool want = pressing_.count(key) != 0;
    if (KeptBit(key) != want) out.asks.push_back(Ask{key, true, want, std::int32_t{0}});
  }
  void Reconcile(Out& out) {
    std::vector<Key> keys;
    for (const auto& [key, on] : pressing_) keys.push_back(key);
    for (const auto& [key, c] : cells_) {
      if (c.hold && (c.holders & Bit(me_)) && !pressing_.count(key)) keys.push_back(key);
    }
    for (const Key& key : keys) ReconcileKey(key, out);
  }

  Player me_;
  bool in_ = false;
  bool live_ = false;
  std::uint16_t epoch_ = 0;
  Player owner_ = 0;
  std::uint32_t seen_ = 0;
  Cells cells_;
  std::map<Key, bool> pressing_;
  std::map<Key, Proposal> proposals_;
  std::vector<Ask> waiting_;
  std::uint64_t out_of_order_ = 0;
};

}  // namespace coop::protocol::room
