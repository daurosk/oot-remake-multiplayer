#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "coop/host/event_object.hpp"

using namespace coop::host;

namespace {

constexpr std::uint64_t kMs = 1'000;

// A talk with a reward (a Deku Scrub's sale, an NPC's gift): binds its starter only.
constexpr EventPhase kTalkPhases[] = {
    {"talk", EventResume::restart_phase, EventGiveUp::cancel, true, 0},
    {"reward", EventResume::restart_phase, EventGiveUp::cancel, true, 2000},
    {"bye", EventResume::finish, EventGiveUp::complete, true, 2000},
};
constexpr EventKind kTalk{"talk", kTalkPhases, 1, false, true};
// A story scene: binds everyone in the scene, also who enters while it runs.
constexpr EventPhase kStoryPhases[] = {
    {"intro", EventResume::from_start, EventGiveUp::cancel, true, 3000},
    {"main", EventResume::restart_phase, EventGiveUp::cancel, true, 3000},
    {"result", EventResume::next_phase, EventGiveUp::cancel, true, 3000},
    {"out", EventResume::finish, EventGiveUp::complete, false, 3000},
};
constexpr EventKind kStory{"story", kStoryPhases, 2, true, true};
// A room gimmick's camera shot: no reward, runs again.
constexpr EventPhase kGimmickPhases[] = {{"play", EventResume::finish, EventGiveUp::complete, false, 3000}};
constexpr EventKind kGimmick{"gimmick", kGimmickPhases, kNoReward, false, false};

EventKey key(const std::string& scene, const std::string& name, std::uint64_t subject = 0) {
  return EventKey{scene, name, subject};
}

}  // namespace

TEST_CASE("event object: a second start of a running key joins it, never a second run") {
  EventTable t;
  EventTable::Out out;
  t.Enter(1, "kokiri", 0, out);
  t.Enter(2, "kokiri", 0, out);
  const std::uint64_t a = t.Start(1, kTalk, key("kokiri", "scrub", 7), 0, out);
  const std::uint64_t b = t.Start(2, kTalk, key("kokiri", "scrub", 7), 0, out);
  CHECK(a != 0);
  CHECK(a == b);
  CHECK(t.Running() == 1);
  CHECK(t.Find(a)->owner == 1);
  CHECK(t.Find(a)->member(2));
  // another scrub (another subject) is another event
  CHECK(t.Start(2, kTalk, key("kokiri", "scrub", 8), 0, out) != a);
  // a player in another scene, or away, starts nothing
  t.Enter(3, "market", 0, out);
  CHECK(t.Start(3, kTalk, key("kokiri", "scrub", 9), 0, out) == 0);
}

TEST_CASE("event object: only the owner's asks with the current epoch move it, one step at a time") {
  EventTable t;
  EventTable::Out out;
  t.Enter(1, "s", 0, out);
  t.Enter(2, "s", 0, out);
  const std::uint64_t id = t.Start(1, kStory, key("s", "zelda"), 0, out);
  CHECK(t.Find(id)->members == (event_bit(1) | event_bit(2)));  // shared: both bound
  CHECK_FALSE(t.Phase(2, id, 1, 1, 10, out));  // not the owner
  CHECK_FALSE(t.Phase(1, id, 2, 1, 10, out));  // wrong epoch
  CHECK_FALSE(t.Phase(1, id, 1, 2, 10, out));  // two steps
  CHECK(t.Phase(1, id, 1, 1, 10, out));
  CHECK(t.Phase(1, id, 1, 2, 20, out));
  CHECK_FALSE(t.Phase(1, id, 1, 3, 30, out));  // past the reward before it is granted
  CHECK_FALSE(t.End(1, id, 1, out));
  CHECK(t.Reward(1, id, 30, out));
  CHECK_FALSE(t.Reward(1, id, 31, out));       // once
  CHECK(t.Phase(1, id, 1, 3, 40, out));
  CHECK(t.End(1, id, 1, out));
  CHECK(t.Running() == 0);
  CHECK(t.Grants(id) == 1);
  CHECK(t.Ends(id) == 1);
  CHECK(out.back().status == EventStatus::ended);
  CHECK(t.Start(2, kStory, key("s", "zelda"), 50, out) == 0);  // once: never again
}

TEST_CASE("event object: the owner leaves: the next member goes on by the phase's resume rule, epoch up") {
  EventTable t;
  EventTable::Out out;
  for (std::uint8_t p = 1; p <= 3; ++p) t.Enter(p, "s", 0, out);
  const std::uint64_t id = t.Start(2, kStory, key("s", "zelda"), 0, out);
  CHECK(t.Phase(2, id, 1, 1, 10, out));
  t.Away(2, true, 20, out);  // pauses in "main": restart_phase
  const EventState* s = t.Find(id);
  REQUIRE(s);
  CHECK(s->owner == 1);
  CHECK(s->epoch == 2);
  CHECK(s->phase == 1);
  CHECK(s->phase_start_us == 20);
  CHECK_FALSE(s->member(2));
  CHECK_FALSE(t.Phase(2, id, 1, 2, 30, out));  // the old owner's late ask
  t.Away(2, false, 40, out);                   // back: a shared scene binds it again
  CHECK(t.Find(id)->member(2));
  t.Enter(4, "s", 50, out);                    // a late entrant too
  CHECK(t.Find(id)->member(4));
  t.Leave(1, 60, out);                         // "main" again at player 2
  CHECK(t.Find(id)->owner == 2);
  CHECK(t.Find(id)->epoch == 3);
}

TEST_CASE("event object: the old owner gave the reward, its ask comes after the hand-over: one grant, no replay") {
  EventTable t;
  EventTable::Out out;
  t.Enter(1, "s", 0, out);
  t.Enter(2, "s", 0, out);
  const std::uint64_t id = t.Start(1, kStory, key("s", "zelda"), 0, out);
  t.Phase(1, id, 1, 1, 10, out);
  t.Phase(1, id, 1, 2, 20, out);  // "result": the game gives the reward here
  t.Tick(20 + 3001 * kMs, out);   // player 1 stalls: handed to 2
  REQUIRE(t.Find(id));
  CHECK(t.Find(id)->owner == 2);
  CHECK(t.Find(id)->phase == 2);  // not rewarded: never skipped
  CHECK(t.Reward(1, id, 4000 * kMs, out));  // the old owner's ask arrives
  CHECK(t.Find(id)->phase == 3);            // the new owner's run goes on after the reward
  CHECK_FALSE(t.Reward(2, id, 4001 * kMs, out));
  CHECK(t.End(2, id, 2, out));
  CHECK(t.Grants(id) == 1);
  // a player that never owned the run cannot grant
  const std::uint64_t id2 = t.Start(1, kTalk, key("s", "scrub"), 0, out);
  CHECK_FALSE(t.Reward(2, id2, 0, out));
}

TEST_CASE("event object: nobody can take it over: cancelled before the reward (may run again), ended after it") {
  EventTable t;
  EventTable::Out out;
  t.Enter(1, "s", 0, out);
  const std::uint64_t a = t.Start(1, kTalk, key("s", "scrub"), 0, out);
  t.Leave(1, 10, out);
  CHECK(out.back().status == EventStatus::cancelled);
  CHECK(t.Grants(a) == 0);
  CHECK(t.Running() == 0);
  t.Enter(1, "s", 20, out);
  const std::uint64_t b = t.Start(1, kTalk, key("s", "scrub"), 20, out);
  CHECK(b != 0);
  CHECK(b != a);
  t.Phase(1, b, 1, 1, 30, out);
  CHECK(t.Reward(1, b, 30, out));
  t.Away(1, true, 40, out);  // dies right after the reward
  CHECK(out.back().status == EventStatus::ended);
  CHECK(t.Grants(b) == 1);
  CHECK(t.Played(key("s", "scrub")));
}

TEST_CASE("event object: a player alone that takes its time keeps its event") {
  EventTable t;
  EventTable::Out out;
  t.Enter(1, "s", 0, out);
  const std::uint64_t id = t.Start(1, kTalk, key("s", "scrub"), 0, out);
  t.Phase(1, id, 1, 1, 10, out);
  t.Tick(60'000 * kMs, out);
  REQUIRE(t.Find(id));
  CHECK(t.Find(id)->owner == 1);
  CHECK(t.Find(id)->epoch == 1);
}

TEST_CASE("event object follower: late, double and out-of-order states, the snapshot heals a lost end") {
  EventTable t;
  EventTable::Out out;
  t.Enter(1, "s", 0, out);
  t.Enter(2, "s", 0, out);
  const std::uint64_t id = t.Start(1, kStory, key("s", "zelda"), 0, out);
  t.Phase(1, id, 1, 1, 10, out);
  REQUIRE(out.size() == 2);
  EventFollower f(2);
  auto acts = f.Apply(out[1]);  // the newer one first
  REQUIRE(acts.size() == 1);
  CHECK(acts[0].what == EventFollower::What::play);
  CHECK(f.HoldsLink());
  CHECK(f.Apply(out[0]).empty());  // the older one late
  CHECK(f.Apply(out[1]).empty());  // twice
  CHECK(f.Find(id)->phase == 1);
  t.Away(1, true, 20, out);  // handed to 2
  acts = f.Apply(out.back());
  REQUIRE(acts.size() == 1);
  CHECK(acts[0].what == EventFollower::What::own);
  CHECK(f.Owns(id));
  // the end is lost; the snapshot does not list the event
  t.Leave(2, 30, out);
  CHECK(t.Running() == 0);
  acts = f.Snapshot(t.Snapshot("s"));
  REQUIRE(acts.size() == 1);
  CHECK(acts[0].what == EventFollower::What::cancelled);
  CHECK_FALSE(f.HoldsLink());
  CHECK(f.Apply(out.back()).empty());  // the lost end comes after all: nothing twice
}

// The model: random players, random order, delay and loss of the table's states (worse than the real channel,
// which is ordered and reliable: a loss there is a disconnect), delayed asks of the games, pauses, scene changes,
// disconnects and rejoins. At the end: every run ended exactly once, at most one reward, a reward for every run
// that ended with its result, none for a cancelled one, a once-event played at most once, and no game holds its
// Link or knows a running event.
namespace {

struct Model {
  explicit Model(std::uint32_t seed) : rng(seed) {
    for (std::uint8_t p = 1; p <= kPlayers; ++p) followers.emplace(p, EventFollower(p));
  }

  static constexpr std::uint8_t kPlayers = 4;
  struct Down {
    bool snapshot = false;
    EventState state;
    std::vector<EventState> live;
  };
  struct Game {
    bool connected = false;
    bool away = false;
    std::string scene;
    std::deque<Down> down;                    // table -> game
    std::deque<std::function<void()>> up;     // game -> table, in order
    std::map<std::uint64_t, int> stops;       // ended / cancelled acts per id
  };

  std::mt19937 rng;
  EventTable table;
  std::map<std::uint8_t, EventFollower> followers;
  std::map<std::uint8_t, Game> games;
  std::map<std::uint64_t, const EventKind*> kinds;
  std::map<std::uint64_t, EventStatus> finals;
  std::map<EventKey, int> ended_once;
  std::uint64_t now = 0;
  bool chaos = true;

  int roll(int n) { return static_cast<int>(rng() % static_cast<std::uint32_t>(n)); }

  void send(EventTable::Out& out) {
    for (const EventState& s : out) {
      if (s.status != EventStatus::running) {
        finals[s.id] = s.status;
        if (s.status == EventStatus::ended && kinds.at(s.id)->once) ++ended_once[s.key];
      }
      for (auto& [p, g] : games) {
        if (!g.connected) continue;
        if (chaos && roll(10) == 0) continue;  // lost
        Down d;
        d.state = s;
        g.down.push_back(d);
        if (chaos && roll(10) == 0) g.down.push_back(d);  // twice
      }
    }
    out.clear();
  }
  void snapshot(std::uint8_t p) {
    Game& g = games[p];
    if (!g.connected) return;
    Down d;
    d.snapshot = true;
    d.live = table.Snapshot(g.scene);
    g.down.push_back(d);
  }
  void acts(std::uint8_t p, const std::vector<EventFollower::Act>& list) {
    for (const auto& a : list) {
      if (a.what == EventFollower::What::ended || a.what == EventFollower::What::cancelled) ++games[p].stops[a.state.id];
    }
  }
  // One message to the game: any of those before the first snapshot (order is free among states), or that snapshot
  // (everything sent before it is then dropped: it is newer than all of it).
  void deliver(std::uint8_t p, bool in_order) {
    Game& g = games[p];
    if (g.down.empty()) return;
    std::size_t limit = 0;
    while (limit < g.down.size() && !g.down[limit].snapshot) ++limit;
    std::size_t i = in_order ? 0 : static_cast<std::size_t>(roll(static_cast<int>(std::min(limit + 1, g.down.size()))));
    EventFollower& f = followers.at(p);
    if (g.down[i].snapshot) {
      const Down d = g.down[i];
      g.down.erase(g.down.begin(), g.down.begin() + static_cast<std::ptrdiff_t>(i) + 1);
      acts(p, f.Snapshot(d.live));
    } else {
      const Down d = g.down[i];
      g.down.erase(g.down.begin() + static_cast<std::ptrdiff_t>(i));
      acts(p, f.Apply(d.state));
    }
  }
  void up(std::uint8_t p, std::function<void()> ask) { games[p].up.push_back(std::move(ask)); }
  void flush_up(std::uint8_t p, int count) {
    Game& g = games[p];
    while (count-- > 0 && !g.up.empty()) {
      auto ask = std::move(g.up.front());
      g.up.pop_front();
      ask();
    }
  }

  void connect(std::uint8_t p, const std::string& scene) {
    Game& g = games[p];
    g.connected = true;
    g.away = false;
    g.scene = scene;
    followers.at(p).NewScene();
    EventTable::Out out;
    table.Enter(p, scene, now, out);
    send(out);
    snapshot(p);
  }
  void disconnect(std::uint8_t p) {
    Game& g = games[p];
    g.connected = false;
    g.down.clear();
    g.up.clear();
    followers.at(p).LetGo();
    followers.at(p).NewScene();
    EventTable::Out out;
    table.Leave(p, now, out);
    send(out);
  }
  void change_scene(std::uint8_t p, const std::string& scene) {
    Game& g = games[p];
    g.scene = scene;
    followers.at(p).LetGo();
    followers.at(p).NewScene();
    up(p, [this, p, scene] {
      EventTable::Out out;
      table.Enter(p, scene, now, out);
      send(out);
      snapshot(p);
    });
  }
  void set_away(std::uint8_t p, bool away) {
    Game& g = games[p];
    g.away = away;
    if (away) followers.at(p).LetGo();
    up(p, [this, p, away] {
      EventTable::Out out;
      table.Away(p, away, now, out);
      send(out);
      if (!away) snapshot(p);
    });
  }
  void start(std::uint8_t p) {
    static const EventKind* const kKinds[] = {&kTalk, &kStory, &kGimmick};
    const EventKind* kind = kKinds[roll(3)];
    const EventKey k = key(games[p].scene, std::string(kind->name), static_cast<std::uint64_t>(roll(2)));
    up(p, [this, p, kind, k] {
      EventTable::Out out;
      const std::uint64_t id = table.Start(p, *kind, k, now, out);
      if (id) kinds[id] = kind;
      send(out);
    });
  }
  // The owner's game plays on, by what it knows.
  void play(std::uint8_t p) {
    const EventFollower& f = followers.at(p);
    for (const auto& [id, s] : f.Known()) {
      if (s.owner != p) continue;
      const EventKind* kind = kinds.at(id);
      const std::uint32_t epoch = s.epoch;
      const std::uint8_t phase = s.phase;
      if (kind->reward_phase == phase && !s.rewarded) {
        up(p, [this, p, id = id] {
          EventTable::Out out;
          table.Reward(p, id, now, out);
          send(out);
        });
      } else if (phase + 1u == kind->phases.size()) {
        up(p, [this, p, id = id, epoch] {
          EventTable::Out out;
          table.End(p, id, epoch, out);
          send(out);
        });
      } else {
        up(p, [this, p, id = id, epoch, phase] {
          EventTable::Out out;
          table.Phase(p, id, epoch, static_cast<std::uint8_t>(phase + 1), now, out);
          send(out);
        });
      }
    }
  }

  void step() {
    now += 50 * kMs;
    static const std::string kScenes[] = {"kokiri", "market"};
    for (std::uint8_t p = 1; p <= kPlayers; ++p) {
      Game& g = games[p];
      if (!g.connected) {
        if (roll(8) == 0) connect(p, kScenes[roll(2)]);
        continue;
      }
      switch (roll(40)) {
        case 0: disconnect(p); continue;
        case 1: change_scene(p, kScenes[roll(2)]); break;
        case 2: case 3: set_away(p, !g.away); break;
        case 4: case 5: case 6: if (!g.away) start(p); break;
        default: break;
      }
      if (!g.away && roll(3) == 0) play(p);
      flush_up(p, roll(3));
      for (int n = roll(4); n > 0; --n) deliver(p, false);
      if (roll(40) == 0) snapshot(p);  // the digest's resync
    }
    EventTable::Out out;
    table.Tick(now, out);
    send(out);
  }

  // No more chaos: everyone back, every message delivered, the owners play their events to the end.
  void settle() {
    chaos = false;
    for (std::uint8_t p = 1; p <= kPlayers; ++p) {
      if (!games[p].connected) connect(p, "kokiri");
      if (games[p].away) set_away(p, false);
    }
    for (int round = 0; round < 200; ++round) {
      now += 50 * kMs;
      for (std::uint8_t p = 1; p <= kPlayers; ++p) {
        flush_up(p, 1000);
        while (!games[p].down.empty()) deliver(p, true);
      }
      for (std::uint8_t p = 1; p <= kPlayers; ++p) snapshot(p);
      for (std::uint8_t p = 1; p <= kPlayers; ++p) {
        while (!games[p].down.empty()) deliver(p, true);
        play(p);
      }
      bool idle = table.Running() == 0;
      for (std::uint8_t p = 1; p <= kPlayers && idle; ++p) idle = games[p].up.empty() && games[p].down.empty();
      if (idle) break;
    }
    for (std::uint8_t p = 1; p <= kPlayers; ++p) {
      flush_up(p, 1000);
      snapshot(p);
      while (!games[p].down.empty()) deliver(p, true);
    }
  }
};

}  // namespace

TEST_CASE("event object model: random orders, delays, losses, pauses, leaves and joins") {
  std::uint64_t runs = 0, ended = 0, cancelled = 0, grants = 0;
  for (std::uint32_t seed = 1; seed <= 300; ++seed) {
    CAPTURE(seed);
    Model m(seed);
    for (int i = 0; i < 400; ++i) m.step();
    m.settle();
    REQUIRE(m.table.Running() == 0);
    for (std::uint64_t id = 1; id <= m.table.LastId(); ++id) {
      CAPTURE(id);
      ++runs;
      REQUIRE(m.table.Ends(id) == 1);  // one end
      REQUIRE(m.finals.count(id) == 1);
      const EventKind* kind = m.kinds.at(id);
      const std::uint32_t g = m.table.Grants(id);
      grants += g;
      if (m.finals.at(id) == EventStatus::ended) {
        ++ended;
        REQUIRE(g == (kind->reward_phase == kNoReward ? 0u : 1u));  // one reward
      } else {
        ++cancelled;
        REQUIRE(g == 0);  // as never played
      }
      for (std::uint8_t p = 1; p <= Model::kPlayers; ++p) REQUIRE(m.games[p].stops[id] <= 1);
    }
    for (const auto& [k, n] : m.ended_once) REQUIRE(n == 1);
    for (std::uint8_t p = 1; p <= Model::kPlayers; ++p) {
      CAPTURE(int(p));
      REQUIRE_FALSE(m.followers.at(p).HoldsLink());  // nobody locked
      REQUIRE(m.followers.at(p).Known().empty());    // every game out of every event
    }
  }
  // the model does exercise all three outcomes
  CHECK(runs > 1000);
  CHECK(ended > 100);
  CHECK(cancelled > 100);
  CHECK(grants > 50);
  MESSAGE("runs " << runs << ", ended " << ended << ", cancelled " << cancelled << ", rewards " << grants);
}
