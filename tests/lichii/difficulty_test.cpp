// Tests for the difficulty rules: the scaler and presets every side shares
// (coop/protocol/difficulty.hpp) and the game's side of them
// (lichii/hook/coop_difficulty.cpp, no emulator code).
#include <doctest/doctest.h>

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "coop/protocol/difficulty.hpp"
#include "coop_difficulty.h"

using CoopDifficulty::Choice;
using CoopDifficulty::Preset;
using CoopDifficulty::Slider;
using CoopDifficulty::Values;
namespace proto = coop::protocol;

TEST_CASE("difficulty: the automatic scaler grows with each further player and stops at its caps") {
  CHECK(proto::auto_difficulty(0) == Values{100, 100, 100});
  CHECK(proto::auto_difficulty(1) == Values{100, 100, 100});  // alone: the game as it is
  CHECK(proto::auto_difficulty(2) == Values{140, 120, 125});
  CHECK(proto::auto_difficulty(3) == Values{180, 140, 150});
  CHECK(proto::auto_difficulty(4) == Values{220, 160, 175});
  CHECK(proto::auto_difficulty(5) == Values{260, 180, 200});
  CHECK(proto::auto_difficulty(6) == Values{300, 200, 200});
  CHECK(proto::auto_difficulty(16) == Values{300, 200, 200});
  CHECK(proto::auto_difficulty(1000000) == Values{300, 200, 200});
  Values before = proto::auto_difficulty(1);
  for (std::size_t players = 2; players <= 16; ++players) {
    const Values now = proto::auto_difficulty(players);
    CHECK(proto::valid_difficulty(now));
    CHECK(now.hp_pct >= before.hp_pct);
    CHECK(now.damage_pct >= before.damage_pct);
    CHECK(now.count_pct >= before.count_pct);
    before = now;
  }
}

TEST_CASE("difficulty: the host's own choice is fixed, whatever the number of players") {
  Choice hard;
  hard.auto_scale = false;
  hard.preset = Preset::hard;
  Choice custom;
  custom.auto_scale = false;
  custom.preset = Preset::custom;
  custom.custom = {325, 75, 250};
  for (std::size_t players : {1u, 2u, 7u, 16u}) {
    CHECK(proto::difficulty_in_force(hard, players) == Values{200, 150, 150});
    CHECK(proto::difficulty_in_force(custom, players) == Values{325, 75, 250});
  }
  // Only the automatic scaler follows them.
  CHECK(proto::difficulty_in_force(Choice{}, 1) == Values{100, 100, 100});
  CHECK(proto::difficulty_in_force(Choice{}, 3) == Values{180, 140, 150});
  // The sliders do not change a preset, and are kept within their ends.
  hard.custom = {500, 400, 300};
  CHECK(proto::difficulty_in_force(hard, 2) == Values{200, 150, 150});
  custom.custom = {9999, 1, 50};
  CHECK(proto::difficulty_in_force(custom, 2) == Values{500, 25, 100});
}

TEST_CASE("difficulty: presets and slider ends are valid values") {
  for (const Preset p : {Preset::easy, Preset::normal, Preset::hard, Preset::custom}) {
    CHECK(proto::valid_difficulty(proto::preset_difficulty(p)));
  }
  CHECK(proto::preset_difficulty(Preset::normal) == Values{100, 100, 100});
  CHECK(proto::preset_difficulty(Preset::easy).damage_pct < 100);
  CHECK(proto::preset_difficulty(Preset::hard).hp_pct > 100);
  CHECK(proto::valid_difficulty({25, 25, 100}));
  CHECK(proto::valid_difficulty({500, 400, 300}));
  CHECK_FALSE(proto::valid_difficulty({24, 100, 100}));
  CHECK_FALSE(proto::valid_difficulty({100, 401, 100}));
  CHECK_FALSE(proto::valid_difficulty({100, 100, 99}));  // enemies are never taken away
  CHECK_FALSE(proto::valid_difficulty({100, 100, 301}));
}

TEST_CASE("difficulty: the menu's rows turn the automatic scaler off") {
  const Choice start;
  CHECK(start.auto_scale);
  const Choice hard = CoopDifficulty::WithPreset(start, Preset::hard);
  CHECK_FALSE(hard.auto_scale);
  CHECK(hard.preset == Preset::hard);
  CHECK(CoopDifficulty::WithAuto(hard, true).auto_scale);
  CHECK(CoopDifficulty::WithAuto(hard, true).preset == Preset::hard);  // comes back when turned off

  // A slider starts from what is shown, here the automatic scaler's values
  // for 2 players, and lands on the slider's marks.
  const Values shown = proto::auto_difficulty(2);  // 140, 120, 125
  Choice c = CoopDifficulty::WithSlider(start, shown, Slider::hp, +1);
  CHECK_FALSE(c.auto_scale);
  CHECK(c.preset == Preset::custom);
  CHECK(c.custom == Values{150, 120, 125});
  c = CoopDifficulty::WithSlider(start, shown, Slider::hp, -1);
  CHECK(c.custom == Values{125, 120, 125});
  c = CoopDifficulty::WithSlider(start, shown, Slider::damage, +2);
  CHECK(c.custom == Values{140, 150, 125});
  c = CoopDifficulty::WithSlider(start, shown, Slider::count, -1);
  CHECK(c.custom == Values{140, 120, 100});
  c = CoopDifficulty::WithSlider(start, shown, Slider::count, 0);
  CHECK(c.custom == shown);
  // The ends hold.
  CHECK(CoopDifficulty::WithSlider(start, shown, Slider::hp, +100).custom.hp_pct == 500);
  CHECK(CoopDifficulty::WithSlider(start, shown, Slider::hp, -100).custom.hp_pct == 25);
  CHECK(CoopDifficulty::WithSlider(start, shown, Slider::damage, +100).custom.damage_pct == 400);
  CHECK(CoopDifficulty::WithSlider(start, shown, Slider::damage, -100).custom.damage_pct == 25);
  CHECK(CoopDifficulty::WithSlider(start, shown, Slider::count, +100).custom.count_pct == 300);
  CHECK(CoopDifficulty::WithSlider(start, shown, Slider::count, -100).custom.count_pct == 100);
}

TEST_CASE("difficulty: the line for the chat and the menu") {
  CHECK(CoopDifficulty::Describe(Choice{}, proto::auto_difficulty(3), 3) ==
        "Automatic (3 players): life 180%, damage 140%, enemies 150%");
  CHECK(CoopDifficulty::Describe(Choice{}, proto::auto_difficulty(1), 1) ==
        "Automatic (1 player): life 100%, damage 100%, enemies 100%");
  const Choice hard = CoopDifficulty::WithPreset(Choice{}, Preset::hard);
  CHECK(CoopDifficulty::Describe(hard, proto::preset_difficulty(Preset::hard), 3) ==
        "Hard: life 200%, damage 150%, enemies 150%");
  CHECK(CoopDifficulty::PresetName(Preset::easy) == "Easy");
  CHECK(CoopDifficulty::PresetName(Preset::custom) == "Custom");
}

TEST_CASE("difficulty: the host's choice is kept as text and read back") {
  Choice c;
  c.auto_scale = false;
  c.preset = Preset::custom;
  c.custom = {325, 75, 250};
  const std::string text = CoopDifficulty::FormatChoice(c);
  CHECK(CoopDifficulty::ParseChoice(text) == c);
  CHECK(CoopDifficulty::ParseChoice(CoopDifficulty::FormatChoice(Choice{})) == Choice{});
  const Choice easy = CoopDifficulty::WithPreset(Choice{}, Preset::easy);
  CHECK(CoopDifficulty::ParseChoice(CoopDifficulty::FormatChoice(easy)) == easy);
  // Windows line ends, comments and blank lines; keys left out keep their defaults.
  CHECK(CoopDifficulty::ParseChoice("; mine\r\n\r\nauto = 0\r\npreset=hard\r\n") ==
        CoopDifficulty::WithPreset(Choice{}, Preset::hard));
  // Anything else is no choice at all.
  CHECK_FALSE(CoopDifficulty::ParseChoice(""));
  CHECK_FALSE(CoopDifficulty::ParseChoice("# nothing\n"));
  CHECK_FALSE(CoopDifficulty::ParseChoice("auto = yes\n"));
  CHECK_FALSE(CoopDifficulty::ParseChoice("preset = brutal\n"));
  CHECK_FALSE(CoopDifficulty::ParseChoice("hp = 9999\n"));
  CHECK_FALSE(CoopDifficulty::ParseChoice("hp = 20x\n"));
  CHECK_FALSE(CoopDifficulty::ParseChoice("count = 50\n"));
  CHECK_FALSE(CoopDifficulty::ParseChoice("speed = 2\n"));
  CHECK_FALSE(CoopDifficulty::ParseChoice("auto\n"));
}

TEST_CASE("difficulty: a new host offers its kept choice once, and only when nobody chose") {
  const std::optional<Choice> hard = CoopDifficulty::WithPreset(Choice{}, Preset::hard);
  CHECK(CoopDifficulty::OffersKept(true, 0, hard, false));
  CHECK_FALSE(CoopDifficulty::OffersKept(false, 0, hard, false));  // a guest never
  CHECK_FALSE(CoopDifficulty::OffersKept(true, 2, hard, false));   // the host before chose
  CHECK_FALSE(CoopDifficulty::OffersKept(true, 0, hard, true));    // once
  CHECK_FALSE(CoopDifficulty::OffersKept(true, 0, std::nullopt, false));
  CHECK_FALSE(CoopDifficulty::OffersKept(true, 0, Choice{}, false));  // automatic is the default already
}

TEST_CASE("difficulty: enemy life is how much of each hit an enemy takes") {
  using CoopDifficulty::EnemyTakes;
  std::int32_t carry = 0;
  CHECK(EnemyTakes(4, 100, carry) == 4);
  CHECK(carry == 0);
  CHECK(EnemyTakes(6, 300, carry) == 2);  // a third
  CHECK(carry == 0);
  CHECK(EnemyTakes(0, 300, carry) == 0);   // no damage this frame
  CHECK(EnemyTakes(-3, 300, carry) == -3);  // a heal is not scaled
  CHECK(carry == 0);
  // A one-life Keese at 200% takes two hits, at 300% three.
  for (std::uint16_t pct : {200, 300, 500}) {
    carry = 0;
    int hits = 0;
    std::int32_t life = 1;
    while (life > 0) {
      life -= EnemyTakes(1, pct, carry);
      ++hits;
    }
    CHECK(hits == pct / 100);
  }
  // At less than 100% a hit takes more: 75% is four thirds.
  carry = 0;
  CHECK(EnemyTakes(3, 75, carry) == 4);
  CHECK(carry == 0);
  CHECK(EnemyTakes(1, 75, carry) == 1);  // 1.33
  CHECK(EnemyTakes(1, 75, carry) == 1);  // 1.33 + .33
  CHECK(EnemyTakes(1, 75, carry) == 2);  // the parts add up to one more
  // Many small hits take what one big one does, at every slider mark.
  for (std::uint16_t pct = coop::protocol::kMinHpPct; pct <= coop::protocol::kMaxHpPct;
       pct += coop::protocol::kDifficultyStep) {
    std::int32_t small_carry = 0;
    std::int32_t big_carry = 0;
    std::int32_t small = 0;
    for (int hit = 0; hit < 600; ++hit) {
      small += EnemyTakes(1, pct, small_carry);
      CHECK(small_carry >= 0);
      CHECK(small_carry < pct);
    }
    CHECK(small == EnemyTakes(600, pct, big_carry));
    CHECK(small == 600 * 100 / pct);
  }
  // A carry out of range (another value's) cannot make a hit take more.
  carry = 99999;
  CHECK(EnemyTakes(1, 300, carry) == 1);  // (299 + 100) / 300
  carry = -5;
  CHECK(EnemyTakes(1, 300, carry) == 0);
  CHECK(EnemyTakes(INT32_MAX, 25, carry) == INT32_MAX);  // no overflow
}

TEST_CASE("difficulty: enemy damage on Link carries the parts of a quarter heart") {
  using CoopDifficulty::LinkTakes;
  std::int32_t carry = 0;
  CHECK(LinkTakes(4, 100, carry) == 4);
  CHECK(LinkTakes(4, 150, carry) == 6);
  CHECK(LinkTakes(4, 200, carry) == 8);
  CHECK(LinkTakes(0, 200, carry) == 0);
  CHECK(LinkTakes(-2, 200, carry) == -2);  // a heal is not damage
  // Quarter-heart hits at 120%: every fifth costs one more.
  carry = 0;
  int total = 0;
  for (int hit = 0; hit < 10; ++hit) {
    total += LinkTakes(1, 120, carry);
  }
  CHECK(total == 12);
  // At 50% every second one is free; ten cost five.
  carry = 0;
  CHECK(LinkTakes(1, 50, carry) == 0);
  CHECK(LinkTakes(1, 50, carry) == 1);
  // Many small hits cost what one big one does.
  for (std::uint16_t pct = coop::protocol::kMinDamagePct; pct <= coop::protocol::kMaxDamagePct;
       pct += coop::protocol::kDifficultyStep) {
    std::int32_t small_carry = 0;
    std::int32_t big_carry = 0;
    int sum = 0;
    for (int hit = 0; hit < 100; ++hit) {
      sum += LinkTakes(1, pct, small_carry);
    }
    CHECK(sum == LinkTakes(100, pct, big_carry));
    CHECK(sum == pct);
  }
  CHECK(LinkTakes(INT32_MAX, 400, carry) == INT32_MAX);  // no overflow
}

TEST_CASE("difficulty: enemies are Enemy_ actors and their shots") {
  CHECK(CoopDifficulty::IsEnemy("Enemy_Keese", false));
  CHECK(CoopDifficulty::IsEnemy("Enemy_DungeonBoss_Gohma", false));
  CHECK(CoopDifficulty::IsEnemy("Octorok_Bullet", true));
  CHECK_FALSE(CoopDifficulty::IsEnemy("Player_Child", false));
  CHECK_FALSE(CoopDifficulty::IsEnemy("Animal_Cucco", false));
  CHECK_FALSE(CoopDifficulty::IsEnemy("Obj_Pot", false));
}

TEST_CASE("difficulty: the Life step scales enemies' hits, and enemies' hits on our Link only") {
  using CoopDifficulty::Kind;
  constexpr std::uint64_t kNow = 1000;
  auto kinds = std::make_unique<CoopDifficulty::Kinds>();
  constexpr std::uint64_t kKeeseLife = 0x10'0000'1000, kKeese = 0x10'0000'2000;
  constexpr std::uint64_t kLinkLife = 0x10'0000'3000, kOctorokShot = 0x10'0000'4000;
  constexpr std::uint64_t kPot = 0x10'0000'5000, kCopyLife = 0x10'0000'6000;
  CHECK(kinds->Mark(kKeeseLife, Kind::enemy_life));
  CHECK(kinds->Mark(kKeese, Kind::enemy));
  CHECK(kinds->Mark(kOctorokShot, Kind::enemy));
  CHECK(kinds->Mark(kLinkLife, Kind::our_link_life));
  CHECK_FALSE(kinds->Mark(0, Kind::enemy));
  CHECK(kinds->Of(kKeese) == Kind::enemy);
  CHECK(kinds->Of(kPot) == Kind::none);
  CHECK(kinds->Of(0) == Kind::none);

  const Values hard{300, 200, 100};
  std::uint64_t attacker = kKeese;
  int asked = 0;
  auto attacker_of = [&] {
    ++asked;
    return attacker;
  };
  // An enemy: a third of each hit, the parts carried.
  CHECK(kinds->Step(kKeeseLife, 1, hard, kNow, attacker_of) == 0);
  CHECK(kinds->Step(kKeeseLife, 1, hard, kNow, attacker_of) == 0);
  CHECK(kinds->Step(kKeeseLife, 1, hard, kNow, attacker_of) == 1);
  CHECK(kinds->Step(kKeeseLife, -1, hard, kNow, attacker_of) == -1);
  CHECK(asked == 0);  // an enemy's attacker is never read
  CHECK(kinds->EnemyHits() == 3);
  // A component the walk did not mark (another player's copy not seen yet, a pot): as the game has it.
  CHECK(kinds->Step(kCopyLife, 5, hard, kNow, attacker_of) == 5);
  // Our Link: double from an enemy and from an enemy's shot...
  CHECK(kinds->Step(kLinkLife, 4, hard, kNow, attacker_of) == 8);
  attacker = kOctorokShot;
  CHECK(kinds->Step(kLinkLife, 2, hard, kNow, attacker_of) == 4);
  CHECK(kinds->LinkHits() == 2);
  // ...but not from anything else: a fall, fire, a PvP hit (our own Link stands in as its attacker).
  attacker = 0;
  CHECK(kinds->Step(kLinkLife, 4, hard, kNow, attacker_of) == 4);
  attacker = kLinkLife;
  CHECK(kinds->Step(kLinkLife, 4, hard, kNow, attacker_of) == 4);
  attacker = kPot;
  CHECK(kinds->Step(kLinkLife, 4, hard, kNow, attacker_of) == 4);
  CHECK(kinds->LinkHits() == 2);
  // At 100% nothing changes and nothing is counted.
  attacker = kKeese;
  const Values normal{};
  CHECK(kinds->Step(kLinkLife, 3, normal, kNow, attacker_of) == 3);
  CHECK(kinds->Step(kKeeseLife, 3, normal, kNow, attacker_of) == 3);
  CHECK(kinds->EnemyHits() == 3);
  CHECK(kinds->LinkHits() == 2);
  // A carry made at other values is dropped: at 200% the Keese starts afresh.
  CHECK(kinds->Step(kKeeseLife, 1, hard, kNow, attacker_of) == 0);  // carry 100 at 300
  CHECK(kinds->Step(kKeeseLife, 1, Values{200, 100, 100}, kNow, attacker_of) == 0);  // 100 / 200, not 200 / 200
  CHECK(kinds->Step(kKeeseLife, 1, Values{200, 100, 100}, kNow, attacker_of) == 1);
  // Marked again as something else (the address is another actor's now): its carry goes.
  CHECK(kinds->Step(kKeeseLife, 1, hard, kNow, attacker_of) == 0);
  CHECK(kinds->Mark(kKeeseLife, Kind::our_link_life));
  CHECK(kinds->Of(kKeeseLife) == Kind::our_link_life);
  // A new scene forgets everything.
  kinds->Clear();
  CHECK(kinds->Of(kKeese) == Kind::none);
  CHECK(kinds->Step(kKeeseLife, 3, hard, kNow, attacker_of) == 3);
}

TEST_CASE("difficulty: a full kinds table marks nothing more and scales nothing it does not know") {
  using CoopDifficulty::Kind;
  auto kinds = std::make_unique<CoopDifficulty::Kinds>();
  std::size_t marked = 0;
  for (std::uint64_t i = 1; i <= 4 * CoopDifficulty::Kinds::kSlots; ++i) {
    if (kinds->Mark(i << 4, Kind::enemy_life)) {
      ++marked;
    }
  }
  CHECK(marked >= CoopDifficulty::Kinds::kSlots / 2);
  CHECK(marked <= CoopDifficulty::Kinds::kSlots);
  // Everything marked is found again; nothing else is.
  std::size_t found = 0;
  for (std::uint64_t i = 1; i <= 4 * CoopDifficulty::Kinds::kSlots; ++i) {
    found += kinds->Of(i << 4) == Kind::enemy_life ? 1 : 0;
  }
  CHECK(found == marked);
  CHECK(kinds->Of(0x7777'0000'0000) == Kind::none);
  // The refused ones are counted (for the log), until a new scene.
  CHECK(kinds->Unmarked() == 4 * CoopDifficulty::Kinds::kSlots - marked);
  kinds->Clear();
  CHECK(kinds->Unmarked() == 0);
}

TEST_CASE("difficulty: a reused address is no enemy any more once the walk sees another actor there") {
  using CoopDifficulty::Kind;
  auto kinds = std::make_unique<CoopDifficulty::Kinds>();
  constexpr std::uint64_t kKeese = 0x20'0000'1000, kLinkLife = 0x20'0000'2000, kKeeseLife = 0x20'0000'3000;
  constexpr std::uint64_t kNow = 1000;
  CHECK(kinds->Mark(kKeese, Kind::enemy));
  CHECK(kinds->Mark(kKeeseLife, Kind::enemy_life));
  CHECK(kinds->Mark(kLinkLife, Kind::our_link_life));
  const CoopDifficulty::Values hard{300, 200, 100};
  std::uint64_t attacker = kKeese;
  auto attacker_of = [&] { return attacker; };
  CHECK(kinds->Step(kLinkLife, 2, hard, kNow, attacker_of) == 4);
  // The Keese died; our own bomb is at its address now: its blast is not the enemies' damage.
  kinds->Forget(kKeese);
  CHECK(kinds->Of(kKeese) == Kind::none);
  CHECK(kinds->Step(kLinkLife, 2, hard, kNow, attacker_of) == 2);
  // A pot's Life component where the Keese's was: taken as the game has it.
  kinds->Forget(kKeeseLife);
  CHECK(kinds->Step(kKeeseLife, 3, hard, kNow, attacker_of) == 3);
  // Forgetting what is no enemy (our Link, an address never marked) changes nothing and adds nothing.
  kinds->Forget(kLinkLife);
  kinds->Forget(0x20'0000'9000);
  CHECK(kinds->Of(kLinkLife) == Kind::our_link_life);
  CHECK(kinds->Of(0x20'0000'9000) == Kind::none);
  // An enemy there again: marked again.
  CHECK(kinds->Mark(kKeese, Kind::enemy));
  CHECK(kinds->Step(kLinkLife, 2, hard, kNow, attacker_of) == 4);
}

TEST_CASE("difficulty: another player's replayed hit is not scaled a second time") {
  using CoopDifficulty::Kind;
  using CoopDifficulty::Kinds;
  auto kinds = std::make_unique<Kinds>();
  constexpr std::uint64_t kLife = 0x30'0000'1000, kUnknown = 0x30'0000'2000;
  const CoopDifficulty::Values hard{200, 100, 100};
  auto none = [] { return std::uint64_t{0}; };
  CHECK(kinds->Mark(kLife, Kind::enemy_life));
  // The dealer's game scaled it (4 of 8 at 200%); the replay of 4 here takes 4.
  kinds->Pass(kLife, 4, 1000);
  CHECK(kinds->Step(kLife, 4, hard, 1010, none) == 4);
  CHECK(kinds->EnemyHits() == 0);
  // Our own hit after it is scaled again.
  CHECK(kinds->Step(kLife, 4, hard, 1020, none) == 2);
  // A replay and our own hit in the same frame: the replay whole, ours halved.
  kinds->Pass(kLife, 2, 2000);
  CHECK(kinds->Step(kLife, 6, hard, 2010, none) == 2 + 2);
  // Two replays before one step add up.
  kinds->Pass(kLife, 1, 3000);
  kinds->Pass(kLife, 2, 3005);
  CHECK(kinds->Step(kLife, 3, hard, 3010, none) == 3);
  // A replay the game never took as a hit (written as life after 400 ms) does not let a later hit through.
  kinds->Pass(kLife, 4, 4000);
  CHECK(kinds->Step(kLife, 4, hard, 4000 + Kinds::kPassMs, none) == 2);
  CHECK(kinds->Step(kLife, 4, hard, 4000 + Kinds::kPassMs + 10, none) == 2);
  // An old pass is not added to a new one.
  kinds->Pass(kLife, 4, 5000);
  kinds->Pass(kLife, 1, 5000 + Kinds::kPassMs);
  CHECK(kinds->Step(kLife, 5, hard, 5000 + Kinds::kPassMs + 1, none) == 1 + 2);
  // Nothing for an unmarked component or a non-positive amount; marked again, the pass goes.
  kinds->Pass(kUnknown, 4, 6000);
  CHECK(kinds->Step(kUnknown, 4, hard, 6001, none) == 4);
  kinds->Pass(kLife, -3, 6000);
  kinds->Pass(kLife, 4, 6000);
  CHECK(kinds->Mark(kLife, Kind::our_link_life));
  CHECK(kinds->Mark(kLife, Kind::enemy_life));
  CHECK(kinds->Step(kLife, 4, hard, 6001, none) == 2);
}

TEST_CASE("difficulty: extra enemies are picked by the placement key alone") {
  using CoopDifficulty::ExtraCount;
  for (std::uint64_t key = 0; key < 50; ++key) {
    CHECK(ExtraCount(key, 100) == 0);
    CHECK(ExtraCount(key, 99) == 0);
    CHECK(ExtraCount(key, 200) == 1);
    CHECK(ExtraCount(key, 300) == 2);
    // Raising the count only ever adds.
    CHECK(ExtraCount(key, 125) <= ExtraCount(key, 150));
    CHECK(ExtraCount(key, 150) <= ExtraCount(key, 175));
    CHECK(ExtraCount(key, 175) <= ExtraCount(key, 200));
    CHECK(ExtraCount(key, 250) >= 1);
    CHECK(ExtraCount(key, 250) <= 2);
  }
  // About the asked share, over many keys (keys of one room differ in few bits).
  for (std::uint16_t pct : {125, 150, 175, 250}) {
    unsigned extra = 0;
    for (std::uint64_t i = 0; i < 4000; ++i) {
      extra += ExtraCount(0x5a17c0de00000000ULL + i * 0x10, pct);
    }
    const double share = extra / 4000.0;
    CHECK(share > (pct - 100) / 100.0 - 0.04);
    CHECK(share < (pct - 100) / 100.0 + 0.04);
  }
}

TEST_CASE("difficulty: which placed enemies get extras") {
  using CoopDifficulty::GetsExtras;
  constexpr std::uint64_t kPlaced = 0x1234'5678'9abc'def0ULL;
  CHECK(GetsExtras("Enemy_Keese", kPlaced, true, false));
  CHECK(GetsExtras("Enemy_Dekubaba", kPlaced, true, false));
  CHECK(GetsExtras("Enemy_Octorok", kPlaced, true, false));
  CHECK_FALSE(GetsExtras("Enemy_Keese", 0, true, false));  // made by the game, not placed
  CHECK_FALSE(GetsExtras("Enemy_Keese", ~std::uint64_t{0}, true, false));
  CHECK_FALSE(GetsExtras("Enemy_Keese", kPlaced, false, false));  // no life read: a helper
  CHECK_FALSE(GetsExtras("Enemy_Keese", kPlaced, true, true));    // the sync's own kind
  CHECK_FALSE(GetsExtras("Animal_Cucco", kPlaced, true, false));
  CHECK_FALSE(GetsExtras("Npc_Kokiri", kPlaced, true, false));
  for (const char* never : {"Enemy_DungeonBoss_Gohma", "Enemy_MiddleBoss_DarkLink", "Enemy_Dodongo_Attention",
                            "Enemy_FlockGuayManager", "Enemy_Guay_Flock", "Enemy_Skullwalltula",
                            "Enemy_DeadHand_Hand", "Enemy_Gohma_Larva", "Enemy_BabyDodongo", "Enemy_Gohma_Egg",
                            "Enemy_Skulltula_Gold", "Enemy_Scrub_Business_Seed", "Enemy_Open_DodongoCave_01",
                            "Enemy_PhantomGanonHorse", "Enemy_Peahat_AircraftCarrier", "Enemy_MoblinClub_ShockWave",
                            "Enemy_Stalchild_LandBattle", "Enemy_KotakeKoumeBattleManager"}) {
    CHECK_MESSAGE(!GetsExtras(never, kPlaced, true, false), std::string(never));
  }
}

TEST_CASE("difficulty: an extra's placement id and place are the same in every game") {
  using CoopDifficulty::ExtraHash;
  std::set<std::uint64_t> ids;
  for (std::uint64_t hash : {1ULL, 2ULL, 0x8000'0000'0000'0000ULL, 0x1234'5678'9abc'def0ULL}) {
    for (unsigned i = 0; i < 3; ++i) {
      const std::uint64_t id = ExtraHash(hash, i);
      CHECK(id == ExtraHash(hash, i));
      CHECK(id != 0);
      CHECK(id != ~std::uint64_t{0});
      CHECK(id != hash);
      ids.insert(id);
    }
  }
  CHECK(ids.size() == 12);  // all different
  const std::array<float, 3> at{10.0f, 2.0f, -5.0f};
  for (std::uint64_t id = 0; id < 16; ++id) {
    const auto p = CoopDifficulty::ExtraPlace(at, id);
    CHECK(p[1] == at[1]);  // the same height: on the original's floor
    const float dx = p[0] - at[0];
    const float dz = p[2] - at[2];
    CHECK(std::sqrt(dx * dx + dz * dz) == doctest::Approx(CoopDifficulty::kExtraOffsetM));
  }
}

TEST_CASE("difficulty: a scene visit's extras are planned once, bound to their actors, and forgotten after it") {
  CoopDifficulty::Extras x;
  using Original = CoopDifficulty::Extras::Original;
  std::vector<Original> walk;
  for (std::uint64_t i = 1; i <= 4; ++i) {
    walk.push_back({"Enemy_Keese", 0x5a17c0de00000000ULL + i, {float(i), 0.0f, 0.0f}});
  }
  CHECK(x.Plan("Dungeon", walk, 100, false, 0) == 0);  // the game as it is
  CHECK(x.Plan("Dungeon", walk, 200, true, 0) == 0);   // a boss here: none
  CHECK(x.All().empty());
  CHECK(x.Plan("Dungeon", walk, 200, false, 1000) == 4);  // one each
  CHECK(x.Plan("Dungeon", walk, 200, false, 1100) == 0);  // once
  REQUIRE(x.All().size() == 4);
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(x.All()[i].name == "Enemy_Keese");
    CHECK(x.All()[i].hash == CoopDifficulty::ExtraHash(walk[i].hash, 0));
  }
  // Raised to 300: one more each; lowered: none taken away.
  CHECK(x.Plan("Dungeon", walk, 300, false, 1200) == 4);
  CHECK(x.Plan("Dungeon", walk, 150, false, 1300) == 0);
  CHECK(x.All().size() == 8);
  // Until the director made it, a made Keese may be one of ours.
  CHECK(x.Holds("Enemy_Keese", 2000));
  CHECK_FALSE(x.Holds("Enemy_Octorok", 2000));
  CHECK_FALSE(x.Holds("Enemy_Keese", 1200 + CoopDifficulty::Extras::kBindWaitMs));  // waited long enough
  std::map<std::uint64_t, std::uint64_t> live;
  for (const auto& e : x.All()) {
    live[e.key] = 0x7000 + e.key * 0x10;
  }
  live[999] = 0x9999;  // another copy of the director's
  x.Bind(live);
  CHECK_FALSE(x.Holds("Enemy_Keese", 2000));
  CHECK(x.HashOf(0x7000 + x.All()[2].key * 0x10, "Enemy_Keese") == x.All()[2].hash);
  CHECK_FALSE(x.HashOf(0x9999, "Enemy_Keese").has_value());
  // At most kMost in a visit.
  std::vector<Original> many;
  for (std::uint64_t i = 0; i < 100; ++i) {
    many.push_back({"Enemy_Octorok", 0x0c70'0000'0000ULL + i, {}});
  }
  x.Plan("Dungeon", many, 300, false, 3000);
  CHECK(x.All().size() == CoopDifficulty::Extras::kMost);
  // Another scene: the old ones are forgotten (gone with their scene).
  CHECK(x.Plan("Field", {}, 300, false, 4000) == 0);
  CHECK(x.All().empty());
  CHECK_FALSE(x.HashOf(0x7000 + 1 * 0x10, "Enemy_Keese").has_value());
  // Back in the first scene: its enemies get their extras again, with the same ids.
  CHECK(x.Plan("Dungeon", walk, 200, false, 5000) == 4);
  CHECK(x.All()[0].hash == CoopDifficulty::ExtraHash(walk[0].hash, 0));
}

TEST_CASE("difficulty: an extra's address is its own only while its actor is there") {
  CoopDifficulty::Extras x;
  using Original = CoopDifficulty::Extras::Original;
  const std::vector<Original> walk{{"Enemy_Keese", 0x5a17'0000'0001ULL, {}}, {"Enemy_Keese", 0x5a17'0000'0002ULL, {}}};
  REQUIRE(x.Plan("Dungeon", walk, 200, false, 0) == 2);
  const auto first = x.All()[0];
  const auto second = x.All()[1];
  constexpr std::uint64_t kFirst = 0x8000, kSecond = 0x8100;
  x.Bind({{first.key, kFirst}, {second.key, kSecond}});
  x.Keep({kFirst, kSecond, 0x1234});
  CHECK(x.HashOf(kFirst, "Enemy_Keese") == first.hash);
  // Another actor at that address now (a drop, a shot): not the extra.
  CHECK_FALSE(x.HashOf(kFirst, "Item_Heart").has_value());
  // The first one died: a whole walk no longer has it. Its address is no extra's,
  // even when the heap gives it to another Keese, and it is not bound again.
  x.Keep({kSecond, 0x1234});
  CHECK_FALSE(x.HashOf(kFirst, "Enemy_Keese").has_value());
  CHECK(x.HashOf(kSecond, "Enemy_Keese") == second.hash);
  x.Bind({{first.key, kFirst}, {second.key, kSecond}});
  x.Keep({kFirst, kSecond});
  CHECK_FALSE(x.HashOf(kFirst, "Enemy_Keese").has_value());
  CHECK(x.All()[0].gone);
  CHECK_FALSE(x.All()[1].gone);
  CHECK_FALSE(x.Holds("Enemy_Keese", 10));  // a gone one is not waited for
  // Not made yet: nothing to keep or forget.
  CoopDifficulty::Extras y;
  REQUIRE(y.Plan("Dungeon", walk, 200, false, 0) == 2);
  y.Keep({});
  CHECK_FALSE(y.All()[0].gone);
}

TEST_CASE("difficulty: every game plans the same extras whatever order its walk saw the enemies in") {
  using Original = CoopDifficulty::Extras::Original;
  std::vector<Original> walk;
  for (std::uint64_t i = 0; i < 2 * CoopDifficulty::Extras::kMost; ++i) {
    walk.push_back({"Enemy_Octorok", 0x0c70'0000'0000ULL + (i * 7919) % 1000, {}});
  }
  std::vector<Original> reversed(walk.rbegin(), walk.rend());
  CoopDifficulty::Extras a, b;
  a.Plan("Lake", walk, 200, false, 0);
  b.Plan("Lake", reversed, 200, false, 0);
  REQUIRE(a.All().size() == CoopDifficulty::Extras::kMost);
  REQUIRE(b.All().size() == a.All().size());
  for (std::size_t i = 0; i < a.All().size(); ++i) {
    CHECK(a.All()[i].hash == b.All()[i].hash);
  }
}

TEST_CASE("difficulty: the game's placement data keeps copies off designed encounters") {
  using CoopDifficulty::Treatment;
  using CoopDifficulty::TreatmentOf;
  using CoopDifficulty::TreatmentWhy;
  // From coop_difficulty_placements.inc (the game's Banc placements).
  constexpr std::uint64_t kMeadowMoblin = 0x1671480c27778737ULL;    // on a rail: a patrol
  constexpr std::uint64_t kCastleStalfos = 0x42e984907f639cadULL;   // a shutter waits for it
  constexpr std::uint64_t kSpiritDinolfos = 0x0df91796e703e5bbULL;  // linked with another Dinolfos
  constexpr std::uint64_t kStageWolfos = 0x173a1753b8898689ULL;     // drops a key
  constexpr std::uint64_t kBongoHand = 0x032aee3737186d25ULL;       // listed, but a boss's part
  constexpr std::uint64_t kPlain = 0x1234'5678'9abc'def0ULL;
  CHECK(TreatmentOf("Enemy_Moblin", kMeadowMoblin) == Treatment::fold);
  CHECK(TreatmentWhy("Enemy_Moblin", kMeadowMoblin).starts_with("rail"));
  CHECK(TreatmentOf("Enemy_Stalfos", kCastleStalfos) == Treatment::fold);
  CHECK(TreatmentWhy("Enemy_Stalfos", kCastleStalfos).starts_with("shutter"));
  CHECK(TreatmentOf("Enemy_Dinolfos", kSpiritDinolfos) == Treatment::fold);
  CHECK(TreatmentWhy("Enemy_Dinolfos", kSpiritDinolfos).starts_with("group"));
  CHECK(TreatmentOf("Enemy_Wolfos", kStageWolfos) == Treatment::fold);
  CHECK(TreatmentWhy("Enemy_Wolfos", kStageWolfos).starts_with("drop"));
  // A boss or a part stays as placed whatever its links say.
  CHECK(TreatmentOf("Enemy_DungeonBoss_BongoBongo_RightHand", kBongoHand) == Treatment::as_placed);
  CHECK(TreatmentWhy("Enemy_DungeonBoss_BongoBongo_RightHand", kBongoHand) == "boss");
  // The same kinds elsewhere, with no such links, get copies (a beat-them-all room's
  // Obj_EnemyCounter counts only its own linked placements, never a copy).
  CHECK(TreatmentOf("Enemy_Moblin", kPlain) == Treatment::copies);
  CHECK(TreatmentOf("Enemy_Stalfos", kPlain) == Treatment::copies);
  CHECK(TreatmentWhy("Enemy_Stalfos", kPlain).empty());
  CHECK_FALSE(CoopDifficulty::GetsExtras("Enemy_Moblin", kMeadowMoblin, true, false));
  CHECK(CoopDifficulty::GetsExtras("Enemy_Moblin", kPlain, true, false));
}

TEST_CASE("difficulty: puzzle and set-piece kinds get their weight, never copies") {
  using CoopDifficulty::Treatment;
  using CoopDifficulty::TreatmentOf;
  constexpr std::uint64_t kPlain = 0x1234'5678'9abc'def0ULL;
  for (const char* fold :
       {"Enemy_Amos", "Enemy_Amos_AlwaysRespawn", "Enemy_Beamos", "Enemy_Beamos_For_High_Place", "Enemy_LikeLike",
        "Enemy_Wallmaster", "Enemy_Floormaster", "Enemy_Floormaster_Little", "Enemy_Floormaster_Truth",
        "Enemy_DeadHand", "Enemy_Poe_Big", "Enemy_Poe_Amy", "Enemy_Poe_Beth", "Enemy_Poe_Joelle", "Enemy_Poe_Meg",
        "Enemy_Poe_Sharp", "Enemy_Poe_Flat", "Enemy_Shabom", "Enemy_Shabom_AlwaysRespawn", "Enemy_ReDead",
        "Enemy_ReDead_ClimaxOnly", "Enemy_TorchSlug", "Enemy_Anubis", "Enemy_IronKnuckle_Black", "Enemy_Keese_Truth",
        "Enemy_Skulltula_Truth", "Enemy_Stalfos_AlwaysRespawn", "Enemy_Guay_Boss", "Enemy_Moblin_Club",
        "Enemy_FlyingPot_ShadowTemple"}) {
    CHECK_MESSAGE(TreatmentOf(fold, kPlain) == Treatment::fold, std::string(fold));
    CHECK_MESSAGE(!CoopDifficulty::TreatmentWhy(fold, kPlain).empty(), std::string(fold));
  }
  for (const char* placed :
       {"Enemy_DungeonBoss_Gohma", "Enemy_MiddleBoss_DarkLink", "Enemy_KotakeKoumeBattleManager",
        "Enemy_DeadHand_Hand", "Enemy_Anubis_MagicField", "Enemy_Shabom_BigOctoChild", "Enemy_Skullwalltula",
        "Enemy_Skulltula_Gold", "Enemy_Scrub_Business", "Enemy_Stalfos_Haribote", "Enemy_Amos_Dictionary",
        "Enemy_GhostPicturePerformer_Poe_Meg", "Enemy_CrazyFloorTile", "Enemy_TrickyDoor", "Enemy_SkullKid",
        "Enemy_Peahat_Larva_Independent", "Npc_Gerudo_FortressGuard", "Animal_Cucco"}) {
    CHECK_MESSAGE(TreatmentOf(placed, kPlain) == Treatment::as_placed, std::string(placed));
  }
  for (const char* copies : {"Enemy_Keese", "Enemy_Keese_Fire", "Enemy_Octorok", "Enemy_Stalfos", "Enemy_Wolfos",
                             "Enemy_Tektite_Red", "Enemy_Lizalfos", "Enemy_Dekubaba", "Enemy_Gibdo", "Enemy_Leever"}) {
    CHECK_MESSAGE(TreatmentOf(copies, kPlain) == Treatment::copies, std::string(copies));
  }
}

TEST_CASE("difficulty: an enemy that gets no copies has the enemy count folded into its life and damage") {
  using CoopDifficulty::Folded;
  using CoopDifficulty::Kind;
  CHECK(Folded(100, 100) == 100);
  CHECK(Folded(150, 80) == 150);  // a count at or below 100% leaves it as it is
  CHECK(Folded(100, 200) == 200);
  CHECK(Folded(200, 150) == 300);
  CHECK(Folded(500, 300) == 1500);
  CHECK(Folded(60000, 300) == 0xffff);

  constexpr std::uint64_t kNow = 1000;
  auto kinds = std::make_unique<CoopDifficulty::Kinds>();
  constexpr std::uint64_t kPoeLife = 0x20'0000'1000, kPoe = 0x20'0000'2000;
  constexpr std::uint64_t kKeese = 0x20'0000'3000, kLinkLife = 0x20'0000'4000;
  CHECK(kinds->Mark(kPoeLife, Kind::folded_enemy_life));
  CHECK(kinds->Mark(kPoe, Kind::folded_enemy));
  CHECK(kinds->Mark(kKeese, Kind::enemy));
  CHECK(kinds->Mark(kLinkLife, Kind::our_link_life));
  std::uint64_t attacker = kPoe;
  auto attacker_of = [&] { return attacker; };
  // Enemies at 200%, life and damage as the game has them: the folded one takes half and hits twice as hard.
  const Values count_only{100, 100, 200};
  CHECK(kinds->Step(kPoeLife, 4, count_only, kNow, attacker_of) == 2);
  CHECK(kinds->Step(kLinkLife, 4, count_only, kNow, attacker_of) == 8);
  attacker = kKeese;  // an enemy with copies keeps the plain damage
  CHECK(kinds->Step(kLinkLife, 4, count_only, kNow, attacker_of) == 4);
  // On top of life and damage: life 200% and enemies 200% make 400% for it.
  const Values hard{200, 150, 200};
  CHECK(kinds->Step(kPoeLife, 4, hard, kNow, attacker_of) == 1);
  attacker = kPoe;
  CHECK(kinds->Step(kLinkLife, 2, hard, kNow, attacker_of) == 6);  // 150% * 2
  // A reused address is forgotten as for any enemy.
  kinds->Forget(kPoe);
  CHECK(kinds->Of(kPoe) == Kind::none);
}

TEST_CASE("difficulty: the night horde grows with the players near a roll") {
  using CoopDifficulty::Horde;
  CHECK(Horde::PerRoll(4, 100) == 0);  // the game as it is
  CHECK(Horde::PerRoll(1, 300) == 0);  // alone at the roll: the game's own
  CHECK(Horde::PerRoll(2, 125) == 1);
  CHECK(Horde::PerRoll(3, 150) == 2);
  CHECK(Horde::PerRoll(8, 200) == Horde::kPerRollMost);

  Horde h;
  using Roll = Horde::Roll;
  std::vector<Roll> rolls{{0xa000, "Enemy_Stalchild", {10.0f, 0.0f, 10.0f}, 2}};
  // A roll is ours to grow only once it has settled (never one the director is still making).
  CHECK(h.Plan("HyruleField_Child", rolls, 1, 150, 0) == 0);
  CHECK(h.Plan("HyruleField_Child", rolls, 1, 150, Horde::kSettleMs - 1) == 0);
  CHECK(h.Plan("HyruleField_Child", rolls, 1, 150, Horde::kSettleMs) == 1);
  CHECK(h.Plan("HyruleField_Child", rolls, 1, 150, Horde::kSettleMs + 100) == 0);  // once
  REQUIRE(h.All().size() == 1);
  CHECK(h.All()[0].name == "Enemy_Stalchild");
  const auto& at = h.All()[0].position;
  CHECK(std::hypot(at[0] - 10.0f, at[2] - 10.0f) == doctest::Approx(CoopDifficulty::kExtraOffsetM));
  // A third player comes near: one more for the same roll.
  rolls[0].players_near = 3;
  CHECK(h.Plan("HyruleField_Child", rolls, 2, 150, 2000) == 1);
  CHECK(h.All().size() == 2);
  // Made and bound: ours, never grown itself.
  h.Bind({{h.All()[0].key, 0xb000}, {h.All()[1].key, 0xb100}});
  CHECK(h.Ours(0xb000));
  rolls.push_back({0xb000, "Enemy_Stalchild", {}, 4});
  CHECK(h.Plan("HyruleField_Child", rolls, 3, 150, 5000) == 0);
  // Gone from the walk: no longer ours, never made again.
  h.Keep({0xa000, 0xb100});
  CHECK_FALSE(h.Ours(0xb000));
  CHECK(h.All()[0].gone);
  // The cap: never more than kMostAlive rolled enemies in our walk at once.
  std::vector<Roll> many;
  for (std::uint64_t i = 0; i < 20; ++i) {
    many.push_back({0xc000 + i, "Enemy_Stalchild", {}, 8});
  }
  CHECK(h.Plan("HyruleField_Child", many, 16, 300, 10000) == 0);  // (settling)
  CHECK(h.Plan("HyruleField_Child", many, 16, 300, 12000) == 2);  // 16 alive: two more fill it
  CHECK(h.Plan("HyruleField_Child", many, 16, 300, 12100) == 0);  // those two still being made count
  CHECK(h.All().size() <= Horde::kMost);
  // Another scene forgets them.
  CHECK(h.Plan("Graveyard", {}, 0, 300, 13000) == 0);
  CHECK(h.All().empty());
}
