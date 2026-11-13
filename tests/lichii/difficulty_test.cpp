// Tests for the difficulty rules: the scaler and presets every side shares
// (coop/protocol/difficulty.hpp) and the game's side of them
// (lichii/hook/coop_difficulty.cpp, no emulator code).
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

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

TEST_CASE("difficulty: enemy life") {
  using CoopDifficulty::ScaledLife;
  using CoopDifficulty::ScaledMax;
  CHECK(ScaledMax(12, 100) == 12);
  CHECK(ScaledMax(12, 140) == 17);  // 16.8
  CHECK(ScaledMax(12, 300) == 36);
  CHECK(ScaledMax(12, 75) == 9);
  CHECK(ScaledMax(1, 140) == 1);  // a Keese stays one hit
  CHECK(ScaledMax(1, 150) == 2);
  CHECK(ScaledMax(1, 25) == 1);  // never under 1
  CHECK(ScaledMax(3, 25) == 1);
  CHECK(ScaledMax(0, 300) == 0);  // nothing to scale
  CHECK(ScaledMax(-1, 300) == -1);
  CHECK(ScaledMax(INT32_MAX, 500) == INT32_MAX);

  // Full stays full, and a hurt enemy keeps its share, rounded up when the
  // most grows.
  CHECK(ScaledLife(12, 12, 36) == 36);
  CHECK(ScaledLife(6, 12, 36) == 18);
  CHECK(ScaledLife(1, 12, 36) == 3);
  CHECK(ScaledLife(36, 36, 12) == 12);
  CHECK(ScaledLife(1, 36, 12) == 1);  // 0.33: alive stays alive
  CHECK(ScaledLife(5, 36, 12) == 1);  // 1.67: down when the most shrinks
  CHECK(ScaledLife(6, 36, 12) == 2);
  CHECK(ScaledLife(0, 12, 36) == 0);  // dead stays dead
  CHECK(ScaledLife(-3, 12, 36) == 0);
  CHECK(ScaledLife(7, 12, 12) == 7);
  CHECK(ScaledLife(20, 12, 36) == 36);  // over its max: full
  CHECK(ScaledLife(7, 0, 5) == 5);      // no max known: only the cap
  // Up and back down never gains or loses life.
  for (std::int32_t life = 1; life <= 12; ++life) {
    for (std::uint16_t pct : {25, 75, 140, 180, 300, 500}) {
      const std::int32_t max = ScaledMax(12, pct);
      const std::int32_t scaled = ScaledLife(life, 12, max);
      CHECK(scaled >= 1);
      CHECK(scaled <= max);
      if (pct >= 100) {
        CHECK(ScaledLife(scaled, max, 12) == life);
      }
    }
  }
}

TEST_CASE("difficulty: enemy damage carries the parts of a quarter heart") {
  CoopDifficulty::DamageScaler s;
  CHECK(s.Scale(4, 100) == 4);
  CHECK(s.Scale(4, 150) == 6);
  CHECK(s.Scale(4, 200) == 8);
  CHECK(s.Scale(0, 200) == 0);
  CHECK(s.Scale(-2, 200) == -2);  // a heal is not damage
  // Quarter-heart hits at 120%: every fifth costs one more.
  s.Reset();
  int total = 0;
  for (int hit = 0; hit < 10; ++hit) {
    total += s.Scale(1, 120);
  }
  CHECK(total == 12);
  // At 50% every second one is free; ten cost five.
  s.Reset();
  CHECK(s.Scale(1, 50) == 0);
  CHECK(s.Scale(1, 50) == 1);
  total = 0;
  for (int hit = 0; hit < 10; ++hit) {
    total += s.Scale(1, 50);
  }
  CHECK(total == 5);
  // Many small hits cost what one big one does.
  for (std::uint16_t pct : {25, 75, 120, 140, 160, 275}) {
    CoopDifficulty::DamageScaler small;
    CoopDifficulty::DamageScaler big;
    int sum = 0;
    for (int hit = 0; hit < 100; ++hit) {
      sum += small.Scale(1, pct);
    }
    CHECK(sum == big.Scale(100, pct));
    CHECK(sum == pct);
  }
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
