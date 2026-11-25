// SPDX-License-Identifier: GPL-3.0-or-later
//
// The room object's state machine (coop/protocol/room_object.hpp), both halves against each other: a server with
// two rooms, up to 16 games, and a network that delays and keeps in order what is reliable, and loses, doubles and
// reorders the owner's samples. Random players enter, leave, reload, press, set, pause, lose the connection and
// come back, in any order, over many seeds. Checked: one owner per epoch, no merge applied out of order, no
// accepted proposal from an owner that had not seen the key's last change, and, once the network is quiet, every
// game in a room holds exactly the kept state, every hold bit says what that player's body does, and an empty room
// keeps nothing.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include "coop/protocol/room_object.hpp"

namespace {

using namespace coop::protocol;
using namespace coop::protocol::room;

constexpr int kRooms = 2;
constexpr std::uint64_t kFreezeTicks = 6;

Key HoldKey(int i) { return Key{0x1000u + static_cast<std::uint64_t>(i), 0xaaaa0000u + static_cast<std::uint32_t>(i)}; }
Key SetKey(int i) { return Key{0x2000u + static_cast<std::uint64_t>(i), 0xbbbb0000u + static_cast<std::uint32_t>(i)}; }

struct Down {  // server to a game
  std::uint64_t due = 0;
  int room = 0;
  std::variant<Merge, Owner, Snapshot> what;
};
enum class PresenceKind { enter, leave, freeze, thaw };
struct Presence {
  std::uint64_t due = 0;
  int room = 0;
  PresenceKind kind = PresenceKind::enter;
};
struct Up {  // a game's ask
  std::uint64_t due = 0;
  int room = 0;
  Ask ask;
};
struct Flying {  // an owner's sample: may be lost, doubled, late
  std::uint64_t due = 0;
  Player from = 0;
  int connection = 0;
  int room = 0;
  std::uint16_t epoch = 0;
  std::uint32_t seen = 0;
  std::vector<Proposal> proposals;
  std::vector<std::optional<Cell>> basis;  // the owner's view of each proposal's key when it sent
};

struct Game {
  explicit Game(Player p) : view(p) {}
  bool connected = false;
  int connection = 0;
  bool can_own = true;
  bool frozen = false;
  int room = -1;  // the room its game has loaded
  View view;
  std::deque<Down> down;
  std::deque<Presence> presence;
  std::deque<Up> up;
};

struct Sim {
  Sim(unsigned seed, int players, bool legacy) : rng(seed) {
    for (int p = 1; p <= players; ++p) games.emplace(static_cast<Player>(p), Game(static_cast<Player>(p)));
    if (legacy) games.at(static_cast<Player>(players)).can_own = false;
  }

  std::mt19937 rng;
  std::uint64_t now = 0;
  bool lossy = true;
  Room rooms[kRooms];
  std::map<Player, Game> games;
  std::map<Player, std::set<int>> server_in;  // the rooms the server has each player in
  std::vector<Flying> flying;
  std::map<std::pair<int, std::uint16_t>, Player> owner_of;  // (room, epoch): who acted as its owner
  std::uint64_t accepted = 0;

  int Roll(int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); }
  bool Chance(int percent) { return Roll(100) < percent; }
  std::uint64_t Delay() { return now + 1 + static_cast<std::uint64_t>(Roll(5)); }
  template <class Q>
  static std::uint64_t After(const Q& q, std::uint64_t due) {
    return q.empty() ? due : std::max(due, q.back().due);  // reliable: in order
  }

  void ActsAsOwner(int room, std::uint16_t epoch, Player p) {
    const auto [it, fresh] = owner_of.try_emplace(std::pair{room, epoch}, p);
    CHECK(it->second == p);  // one owner per epoch
  }

  void Send(int room, const Effects& effects) {
    for (const Effect& e : effects) {
      if (const auto* one = std::get_if<ToOne>(&e)) {
        Game& g = games.at(one->to);
        if (g.connected) g.down.push_back(Down{After(g.down, Delay()), room, one->snapshot});
        continue;
      }
      const ToAll& all = std::get<ToAll>(e);
      for (const auto& [p, m] : rooms[room].members()) {
        Game& g = games.at(p);
        if (!g.connected) continue;
        std::visit([&](const auto& w) { g.down.push_back(Down{After(g.down, Delay()), room, w}); }, all.what);
      }
    }
  }
  void SendAsks(Game& g, const View::Out& out) {
    for (const Ask& a : out.asks) g.up.push_back(Up{After(g.up, Delay()), g.room, a});
  }
  void SayPresence(Game& g, int room, PresenceKind kind, std::uint64_t due) {
    g.presence.push_back(Presence{After(g.presence, due), room, kind});
  }

  // What a game's player and its own room logic do.
  void GameEnter(Game& g, int room) {
    g.room = room;
    g.view.Enter();
    SayPresence(g, room, PresenceKind::enter, Delay());
  }
  void GameLeave(Game& g) {
    if (g.room < 0) return;
    SayPresence(g, g.room, PresenceKind::leave, Delay());
    g.view.Leave();
    g.room = -1;
  }
  void Disconnect(Player p, Game& g) {
    g.connected = false;
    ++g.connection;
    g.frozen = false;
    g.room = -1;
    g.view = View(p);
    g.down.clear();
    g.presence.clear();
    g.up.clear();
    for (int room : std::set<int>(server_in[p])) Send(room, rooms[room].Leave(p));
    server_in[p].clear();
  }
  void Act(Player p, Game& g) {
    if (!g.connected) {
      if (Chance(10)) g.connected = true;
      return;
    }
    if (Chance(1)) {
      Disconnect(p, g);
      return;
    }
    if (g.frozen) {
      if (Chance(8)) {
        g.frozen = false;
        if (g.room >= 0) SayPresence(g, g.room, PresenceKind::thaw, Delay());
      }
      return;
    }
    if (g.room < 0) {
      if (Chance(30)) GameEnter(g, Roll(kRooms));
      return;
    }
    const int what = Roll(100);
    View::Out out;
    if (what < 3) {
      GameLeave(g);
    } else if (what < 5) {  // died or loaded a save: the same room, fresh
      const int room = g.room;
      GameLeave(g);
      GameEnter(g, room);
    } else if (what < 7) {
      const int room = 1 - g.room;
      GameLeave(g);
      GameEnter(g, room);
    } else if (what < 9) {  // the server notices only after a while
      g.frozen = true;
      SayPresence(g, g.room, PresenceKind::freeze, now + kFreezeTicks);
    } else if (what < 30) {
      const Key key = HoldKey(Roll(3));
      g.view.Press(key, g.view.pressing().count(key) == 0, out);
    } else if (what < 45) {
      g.view.Set(SetKey(Roll(3)), std::int32_t{Roll(4)}, out);
    } else if (what < 60) {
      if (Chance(30)) {
        g.view.DeriveHold(HoldKey(Roll(3)), Chance(50));
      } else {
        g.view.Derive(SetKey(Roll(3)), Chance(20), std::int32_t{Roll(4)});
      }
    }
    SendAsks(g, out);
  }
  void Beat(Player p, Game& g) {
    if (!g.connected || g.frozen || !g.view.is_owner()) return;
    Flying f{Delay() + static_cast<std::uint64_t>(lossy ? Roll(8) : 0), p, g.connection, g.room, g.view.epoch(),
             g.view.seen(), g.view.Sample(), {}};
    if (f.proposals.empty()) return;
    for (const Proposal& pr : f.proposals) {
      auto it = g.view.cells().find(pr.key);
      f.basis.push_back(it == g.view.cells().end() ? std::nullopt : std::optional<Cell>(it->second));
    }
    if (lossy && Chance(30)) return;
    flying.push_back(f);
    if (lossy && Chance(20)) {
      f.due += static_cast<std::uint64_t>(Roll(6));
      flying.push_back(f);
    }
  }

  void ServerTakes(const Flying& f) {
    const Game& g = games.at(f.from);
    if (!g.connected || g.connection != f.connection) return;  // that connection is gone
    Room& r = rooms[f.room];
    for (std::size_t i = 0; i < f.proposals.size(); ++i) {
      const Proposal& pr = f.proposals[i];
      auto before = r.cells().find(pr.key);
      const std::optional<Cell> pre = before == r.cells().end() ? std::nullopt : std::optional<Cell>(before->second);
      const std::uint64_t merges = r.stats().merges;
      Send(f.room, r.OnSample(f.from, f.epoch, f.seen, {pr}));
      if (r.stats().merges == merges) continue;
      ++accepted;
      ActsAsOwner(f.room, f.epoch, f.from);
      // No state undone: the owner proposed this knowing the key exactly as the server kept it.
      CHECK(pre.has_value() == f.basis[i].has_value());
      if (pre && f.basis[i]) {
        CHECK(pre->rev == f.basis[i]->rev);
        CHECK(pre->Same(*f.basis[i]));
      }
    }
  }
  void GameTakes(Player p, Game& g, const Down& d) {
    if (d.room != g.room) return;  // another room's object
    View::Out out;
    if (const auto* s = std::get_if<Snapshot>(&d.what)) {
      g.view.On(*s, out);
    } else if (const auto* o = std::get_if<Owner>(&d.what)) {
      g.view.On(*o);
    } else {
      g.view.On(std::get<Merge>(d.what), out);
    }
    SendAsks(g, out);
    if (g.view.is_owner()) ActsAsOwner(g.room, g.view.epoch(), p);
  }
  void Deliver() {
    for (auto& [p, g] : games) {
      while (!g.presence.empty() && g.presence.front().due <= now) {
        const Presence pr = g.presence.front();
        g.presence.pop_front();
        Room& r = rooms[pr.room];
        switch (pr.kind) {
          case PresenceKind::enter:
            server_in[p].insert(pr.room);
            Send(pr.room, r.Enter(p, g.can_own));
            break;
          case PresenceKind::leave:
            server_in[p].erase(pr.room);
            Send(pr.room, r.Leave(p));
            break;
          case PresenceKind::freeze:
            Send(pr.room, r.Freeze(p, true));
            break;
          case PresenceKind::thaw:
            Send(pr.room, r.Freeze(p, false));
            break;
        }
      }
      while (!g.up.empty() && g.up.front().due <= now) {
        const Up u = g.up.front();
        g.up.pop_front();
        Send(u.room, rooms[u.room].OnAsk(p, u.ask));
      }
    }
    std::vector<Flying> due;
    for (auto it = flying.begin(); it != flying.end();) {
      if (it->due <= now) {
        due.push_back(*it);
        it = flying.erase(it);
      } else {
        ++it;
      }
    }
    std::shuffle(due.begin(), due.end(), rng);
    for (const Flying& f : due) ServerTakes(f);
    for (auto& [p, g] : games) {
      while (!g.frozen && !g.down.empty() && g.down.front().due <= now) {
        const Down d = g.down.front();
        g.down.pop_front();
        GameTakes(p, g, d);
      }
    }
  }
  void Tick(bool acting) {
    ++now;
    std::vector<Player> order;
    for (auto& [p, g] : games) order.push_back(p);
    std::shuffle(order.begin(), order.end(), rng);
    for (Player p : order) {
      if (acting) Act(p, games.at(p));
      Beat(p, games.at(p));
    }
    Deliver();
  }
  bool Quiet() const {
    if (!flying.empty()) return false;
    for (const auto& [p, g] : games) {
      if (!g.down.empty() || !g.presence.empty() || !g.up.empty() || !g.view.proposals().empty()) return false;
    }
    return true;
  }
  // Everybody stops acting and wakes up; the network delivers what is under way.
  void Settle() {
    lossy = false;
    for (auto& [p, g] : games) {
      if (g.connected && g.frozen) {
        g.frozen = false;
        if (g.room >= 0) SayPresence(g, g.room, PresenceKind::thaw, Delay());
      }
    }
    for (int i = 0; i < 400 && !(i > 20 && Quiet()); ++i) Tick(false);
    REQUIRE(Quiet());
  }
  void CheckSettled() {
    for (int room = 0; room < kRooms; ++room) {
      const Room& r = rooms[room];
      std::set<Player> here;
      bool can_own = false;
      std::map<Key, std::uint32_t> bodies;
      for (const auto& [p, g] : games) {
        CHECK(g.view.out_of_order() == 0);
        if (!g.connected || g.room != room) continue;
        here.insert(p);
        can_own = can_own || g.can_own;
        for (const auto& [key, on] : g.view.pressing()) bodies[key] |= Bit(p);
        // Every game in the room holds exactly the kept state.
        REQUIRE(g.view.live());
        CHECK(g.view.epoch() == r.epoch());
        CHECK(g.view.owner() == r.owner());
        CHECK(g.view.seen() == r.rev());
        CHECK(g.view.cells().size() == r.cells().size());
        for (const auto& [key, cell] : r.cells()) {
          auto it = g.view.cells().find(key);
          REQUIRE(it != g.view.cells().end());
          CHECK(it->second.Same(cell));
          CHECK(it->second.rev == cell.rev);
        }
      }
      std::set<Player> members;
      for (const auto& [p, m] : r.members()) members.insert(p);
      CHECK(members == here);
      if (can_own) {
        REQUIRE(here.count(r.owner()) == 1);
        CHECK(games.at(r.owner()).can_own);
      } else {
        CHECK(r.owner() == 0);
      }
      // No press lost, none left behind: a hold's player bits are the bodies on it.
      std::map<Key, std::uint32_t> kept;
      for (const auto& [key, cell] : r.cells()) {
        if (cell.hold && (cell.holders & ~Bit(0)) != 0) kept[key] = cell.holders & ~Bit(0);
      }
      CHECK(kept == bodies);
      if (here.empty()) CHECK(r.cells().empty());
      CHECK(r.stats().kind_clash == 0);
    }
  }
};

Effects Run(Effects effects, std::map<Player, View*> views) {
  // A direct wire for the small cases: every effect at once, to the views given (the members under test).
  for (const Effect& e : effects) {
    View::Out out;
    if (const auto* one = std::get_if<ToOne>(&e)) {
      if (views.count(one->to)) views.at(one->to)->On(one->snapshot, out);
      continue;
    }
    for (auto& [p, v] : views) {
      const auto& what = std::get<ToAll>(e).what;
      if (const auto* m = std::get_if<Merge>(&what)) {
        v->On(*m, out);
      } else {
        v->On(std::get<Owner>(what));
      }
    }
  }
  return effects;
}

}  // namespace

TEST_CASE("room object: all games converge over random orders, delay, loss, reorder and doubles") {
  std::uint64_t accepted = 0;
  std::uint64_t stale = 0;
  std::uint64_t epochs = 0;
  for (unsigned seed = 1; seed <= 400; ++seed) {
    CAPTURE(seed);
    const int players = std::vector<int>{2, 3, 4, 8, 16}[seed % 5];
    Sim sim(seed, players, seed % 3 == 0);
    for (int round = 0; round < 4; ++round) {  // settled and checked four times, with what was kept going on
      sim.lossy = true;
      for (int i = 0; i < 250; ++i) sim.Tick(true);
      sim.Settle();
      sim.CheckSettled();
    }
    accepted += sim.accepted;
    for (const Room& r : sim.rooms) {
      stale += r.stats().proposals_stale;
      epochs += r.epoch();
    }
  }
  // The runs did reach the cases the rules are for.
  CHECK(accepted > 1000);
  CHECK(stale > 100);
  CHECK(epochs > 1000);
}

TEST_CASE("room object: two players on one switch, each with its own bit (row 244)") {
  Room room;
  View a(1), b(2);
  const std::map<Player, View*> views{{1, &a}, {2, &b}};
  a.Enter();
  b.Enter();
  Run(room.Enter(1), views);
  Run(room.Enter(2), views);
  const Key plug = HoldKey(0);
  View::Out out;
  a.Press(plug, true, out);
  b.Press(plug, true, out);
  Run(room.OnAsk(1, out.asks.at(0)), views);
  Run(room.OnAsk(2, out.asks.at(1)), views);
  CHECK(room.cells().at(plug).holders == (Bit(1) | Bit(2)));
  out.asks.clear();
  a.Press(plug, false, out);
  Run(room.OnAsk(1, out.asks.at(0)), views);
  CHECK(a.HoldOn(plug));  // player 2 still stands on it: on in every game, with no timer
  CHECK(b.HoldOn(plug));
  // Player 2 leaves the room without stepping off: the server takes its bit.
  Run(room.Leave(2), {{1, &a}});
  CHECK_FALSE(a.HoldOn(plug));
  CHECK(room.cells().count(plug) == 0);
}

TEST_CASE("room object: a press is kept when the owner goes, and a late player gets it") {
  Room room;
  View a(1), b(2), c(3);
  a.Enter();
  b.Enter();
  Run(room.Enter(1), {{1, &a}, {2, &b}});
  Run(room.Enter(2), {{1, &a}, {2, &b}});
  CHECK(room.owner() == 1);
  View::Out out;
  b.Set(SetKey(0), std::int32_t{1}, out);
  Run(room.OnAsk(2, out.asks.at(0)), {{2, &b}});  // player 1's connection drops before it hears of it
  const std::uint16_t before = room.epoch();
  Run(room.Leave(1), {{2, &b}});
  CHECK(room.owner() == 2);
  CHECK(room.epoch() == before + 1);
  c.Enter();
  Run(room.Enter(3), {{2, &b}, {3, &c}});
  CHECK(c.cells().at(SetKey(0)).value == KeyedData{std::int32_t{1}});
  CHECK(room.owner() == 2);  // sticky: a lower number that comes later does not take it
  View again(1);
  again.Enter();
  Run(room.Enter(1), {{1, &again}, {2, &b}, {3, &c}});
  CHECK(room.owner() == 2);
  CHECK(again.cells().at(SetKey(0)).value == KeyedData{std::int32_t{1}});
}

TEST_CASE("room object: an owner that has not seen a press cannot undo it, an old epoch is refused") {
  Room room;
  View a(1), b(2);
  const std::map<Player, View*> views{{1, &a}, {2, &b}};
  a.Enter();
  b.Enter();
  Run(room.Enter(1), views);
  Run(room.Enter(2), views);
  const Key torch = SetKey(1);
  // The owner's logic says 0 (its timer ran out) while player 2 lights it again; the press reaches the server first.
  a.Derive(torch, false, std::int32_t{0});
  const std::vector<Proposal> late = a.Sample();
  const std::uint32_t seen = a.seen();
  const std::uint16_t epoch = a.epoch();
  View::Out out;
  b.Set(torch, std::int32_t{1}, out);
  Run(room.OnAsk(2, out.asks.at(0)), views);
  CHECK(room.OnSample(1, epoch, seen, late).empty());
  CHECK(room.stats().proposals_stale == 1);
  CHECK(room.cells().at(torch).value == KeyedData{std::int32_t{1}});
  CHECK(a.proposals().empty());  // the merge beat it: the owner's logic decides again from the new value
  // It decides 0 again, now knowing the press: taken.
  a.Derive(torch, false, std::int32_t{0});
  Run(room.OnSample(1, a.epoch(), a.seen(), a.Sample()), views);
  CHECK(b.cells().at(torch).value == KeyedData{std::int32_t{0}});
  // Player 1 pauses: player 2 owns the next epoch, and player 1's sample of the old one is refused.
  a.Derive(torch, false, std::int32_t{3});
  const std::vector<Proposal> old = a.Sample();
  Run(room.Freeze(1, true), {{2, &b}});
  CHECK(room.owner() == 2);
  CHECK(room.OnSample(1, epoch, a.seen(), old).empty());
  CHECK(room.stats().proposals_refused == 1);
  CHECK(room.OnSample(1, room.epoch(), room.rev(), old).empty());  // nor with the new epoch: not the owner
  CHECK(room.cells().at(torch).value == KeyedData{std::int32_t{0}});
  Run(room.Freeze(1, false), {{2, &b}});
  CHECK(room.owner() == 2);  // sticky
}

TEST_CASE("room object: an empty room keeps nothing, an older module never owns") {
  Room room;
  View a(1);
  a.Enter();
  Run(room.Enter(1, false), {{1, &a}});
  CHECK(room.owner() == 0);
  View::Out out;
  a.Set(SetKey(0), std::int32_t{2}, out);
  a.Press(HoldKey(0), true, out);
  for (const Ask& ask : out.asks) Run(room.OnAsk(1, ask), {{1, &a}});
  CHECK(room.cells().size() == 2);  // its asks count all the same
  const std::uint32_t rev = room.rev();
  room.Leave(1);
  CHECK(room.cells().empty());
  CHECK(room.rev() > rev);
  CHECK(room.OnAsk(1, out.asks.at(0)).empty());  // a late ask for the room nobody is in
  CHECK(room.cells().empty());
  CHECK(room.stats().asks_refused == 1);
}
