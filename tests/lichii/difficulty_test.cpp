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
  CHECK(kinds->Step(kKeeseLife, 1, hard, attacker_of) == 0);
  CHECK(kinds->Step(kKeeseLife, 1, hard, attacker_of) == 0);
  CHECK(kinds->Step(kKeeseLife, 1, hard, attacker_of) == 1);
  CHECK(kinds->Step(kKeeseLife, -1, hard, attacker_of) == -1);
  CHECK(asked == 0);  // an enemy's attacker is never read
  CHECK(kinds->EnemyHits() == 3);
  // A component the walk did not mark (another player's copy not seen yet, a pot): as the game has it.
  CHECK(kinds->Step(kCopyLife, 5, hard, attacker_of) == 5);
  // Our Link: double from an enemy and from an enemy's shot...
  CHECK(kinds->Step(kLinkLife, 4, hard, attacker_of) == 8);
  attacker = kOctorokShot;
  CHECK(kinds->Step(kLinkLife, 2, hard, attacker_of) == 4);
  CHECK(kinds->LinkHits() == 2);
  // ...but not from anything else: a fall, fire, a PvP hit (our own Link stands in as its attacker).
  attacker = 0;
  CHECK(kinds->Step(kLinkLife, 4, hard, attacker_of) == 4);
  attacker = kLinkLife;
  CHECK(kinds->Step(kLinkLife, 4, hard, attacker_of) == 4);
  attacker = kPot;
  CHECK(kinds->Step(kLinkLife, 4, hard, attacker_of) == 4);
  CHECK(kinds->LinkHits() == 2);
  // At 100% nothing changes and nothing is counted.
  attacker = kKeese;
  const Values normal{};
  CHECK(kinds->Step(kLinkLife, 3, normal, attacker_of) == 3);
  CHECK(kinds->Step(kKeeseLife, 3, normal, attacker_of) == 3);
  CHECK(kinds->EnemyHits() == 3);
  CHECK(kinds->LinkHits() == 2);
  // A carry made at other values is dropped: at 200% the Keese starts afresh.
  CHECK(kinds->Step(kKeeseLife, 1, hard, attacker_of) == 0);  // carry 100 at 300
  CHECK(kinds->Step(kKeeseLife, 1, Values{200, 100, 100}, attacker_of) == 0);  // 100 / 200, not 200 / 200
  CHECK(kinds->Step(kKeeseLife, 1, Values{200, 100, 100}, attacker_of) == 1);
  // Marked again as something else (the address is another actor's now): its carry goes.
  CHECK(kinds->Step(kKeeseLife, 1, hard, attacker_of) == 0);
  CHECK(kinds->Mark(kKeeseLife, Kind::our_link_life));
  CHECK(kinds->Of(kKeeseLife) == Kind::our_link_life);
  // A new scene forgets everything.
  kinds->Clear();
  CHECK(kinds->Of(kKeese) == Kind::none);
  CHECK(kinds->Step(kKeeseLife, 3, hard, attacker_of) == 3);
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
  CHECK(x.HashOf(0x7000 + x.All()[2].key * 0x10) == x.All()[2].hash);
  CHECK_FALSE(x.HashOf(0x9999).has_value());
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
  CHECK_FALSE(x.HashOf(0x7000 + 1 * 0x10).has_value());
  // Back in the first scene: its enemies get their extras again, with the same ids.
  CHECK(x.Plan("Dungeon", walk, 200, false, 5000) == 4);
  CHECK(x.All()[0].hash == CoopDifficulty::ExtraHash(walk[0].hash, 0));
}
