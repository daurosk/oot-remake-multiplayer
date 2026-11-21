// Boss turns (server/boss_turns.hpp): when an enemy's turn ends, and who has the next one.
#include <doctest/doctest.h>

#include "boss_turns.hpp"

using namespace coop::server::turns;

namespace {
constexpr std::uint64_t kS = 1'000'000;

Why feed(State& s, std::uint64_t at, bool attacking, const Config& c = {}, bool blocked = false,
         std::uint16_t epoch = 1) {
  return Step(s, Sample{epoch, attacking, blocked, true}, at, c);
}
}  // namespace

TEST_CASE("boss turns: a turn ends when an attack ends, after one attack and the least time") {
  State s;
  CHECK(feed(s, 0, false) == Why::none);
  CHECK(feed(s, 1 * kS, true) == Why::none);   // its attack starts
  CHECK(feed(s, 2 * kS, false) == Why::none);  // ends before the 4 s: the turn goes on
  CHECK(s.attacks == 1);
  CHECK(feed(s, 3 * kS, true) == Why::none);
  CHECK(feed(s, 4 * kS, true) == Why::none);   // never mid-attack
  CHECK(feed(s, 5 * kS, false) == Why::attack_ended);
}

TEST_CASE("boss turns: never while blocked (an event, a stun, its death, a hand-over asked) or out of a fight") {
  State s;
  feed(s, 0, true);
  CHECK(feed(s, 5 * kS, false, {}, true) == Why::none);  // blocked when it fell: that edge is gone
  CHECK(feed(s, 6 * kS, false) == Why::none);            // (the idle rule waits from that attack's end)
  CHECK(Step(s, Sample{1, false, false, false}, 20 * kS, Config{}) == Why::none);  // out of a fight
}

TEST_CASE("boss turns: no attack for its owner's Link for idle_us: the turn goes on (a puppet target waits)") {
  State s;
  feed(s, 0, false);
  CHECK(feed(s, 7 * kS, false) == Why::none);
  CHECK(feed(s, 8 * kS, false) == Why::idle);
  State a;
  feed(a, 0, true);
  feed(a, 1 * kS, false);  // too early: no turn
  CHECK(feed(a, 8 * kS, false) == Why::none);
  CHECK(feed(a, 9 * kS, false) == Why::idle);  // 8 s after its attack's end
}

TEST_CASE("boss turns: a new owner (any hand-over, a stun's too) starts a new turn") {
  State s;
  feed(s, 0, true);
  feed(s, 5 * kS, true);
  CHECK(feed(s, 6 * kS, false, {}, false, 2) == Why::none);  // epoch 2: new turn, its attack counted from now
  CHECK(s.started_at == 6 * kS);
  CHECK(s.attacks == 0);
}

TEST_CASE("boss turns: off: one owner for the fight, as before") {
  Config off;
  off.on = false;
  State s;
  feed(s, 0, true, off);
  CHECK(feed(s, 5 * kS, false, off) == Why::none);
  CHECK(feed(s, 30 * kS, false, off) == Why::none);
}

TEST_CASE("boss turns: next is the player who hurt it most, else the next number round, never unfit or far") {
  const auto p = [](std::uint64_t id, std::uint8_t n, double d, std::int64_t aggro = 0, bool fit = true) {
    return Player{id, n, fit, d, aggro};
  };
  // Round robin after the owner's number, wrapping.
  CHECK(Next({p(11, 1, 5), p(12, 2, 5), p(13, 3, 5)}, 2, 40) == 13u);
  CHECK(Next({p(11, 1, 5), p(12, 2, 5), p(13, 3, 5)}, 3, 40) == 11u);
  // Aggro first; ties: nearer, then the lower number.
  CHECK(Next({p(11, 1, 5), p(13, 3, 5, 2), p(14, 4, 9, 6)}, 1, 40) == 14u);
  CHECK(Next({p(12, 2, 9, 3), p(13, 3, 4, 3)}, 1, 40) == 13u);
  CHECK(Next({p(12, 2, 4, 3), p(13, 3, 4, 3)}, 1, 40) == 12u);
  // Unfit (downed, away, its intro not played), out of reach or of unknown place: skipped.
  CHECK_FALSE(Next({p(11, 1, 5), p(12, 2, 5, 9, false)}, 1, 40).has_value());
  CHECK_FALSE(Next({p(11, 1, 5), p(12, 2, 41)}, 1, 40).has_value());
  CHECK_FALSE(Next({p(11, 1, 5), Player{12, 2, true, std::nullopt, 0}}, 1, 40).has_value());
  // One player: nobody else (the game as it is).
  CHECK_FALSE(Next({p(11, 1, 5)}, 1, 40).has_value());
  CHECK_FALSE(Next({}, 1, 40).has_value());
}

TEST_CASE("boss turns: a boss's turn never ends on the idle rule (its no-attack phases: paintings, ceiling, burrowed)") {
  Config c;
  State s;
  const auto boss = [&](std::uint64_t at, bool attacking) {
    return Step(s, Sample{1, attacking, false, true, false}, at, c);
  };
  CHECK(boss(0, false) == Why::none);
  CHECK(boss(30 * kS, false) == Why::none);  // 30 s inside his painting: still his owner's
  CHECK(boss(31 * kS, true) == Why::none);
  CHECK(boss(33 * kS, false) == Why::attack_ended);  // his own attack's end
}
