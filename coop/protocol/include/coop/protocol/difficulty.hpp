// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty: how much harder the enemies are in a session (docs/protocol.md,
// "Difficulty"). Values in percent of the game's own: enemy life, the damage
// enemies deal to players, how many enemies there are, and the bosses' life
// and damage (bosses and mini-bosses: the automatic scaler grows them faster). The server
// decides them for everyone: by the number of players until the host sets
// them, then the host's, whoever joins or leaves. The rules are here so the
// server and the games cannot differ.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace coop::protocol {

// The values in force, in percent (100: the game as it is).
struct DifficultyValues {
  std::uint16_t hp_pct = 100;      // enemy life
  std::uint16_t damage_pct = 100;  // damage enemies deal to players
  std::uint16_t count_pct = 100;   // enemies: 150 is one more for every second one
  std::uint16_t boss_hp_pct = 100;      // boss and mini-boss life
  std::uint16_t boss_damage_pct = 100;  // damage bosses and mini-bosses deal to players
  bool operator==(const DifficultyValues&) const = default;
};

// What the host picks. `custom` is the three sliders.
enum class DifficultyPreset : std::uint8_t {
  easy = 0,
  normal = 1,
  hard = 2,
  custom = 3,
};
inline constexpr std::uint8_t kDifficultyPresets = 4;

// The sliders' ends and step. Enemies are never taken away (count from 100).
inline constexpr std::uint16_t kDifficultyStep = 25;
inline constexpr std::uint16_t kMinHpPct = 25;
inline constexpr std::uint16_t kMaxHpPct = 500;
inline constexpr std::uint16_t kMinDamagePct = 25;
inline constexpr std::uint16_t kMaxDamagePct = 400;
inline constexpr std::uint16_t kMinCountPct = 100;
inline constexpr std::uint16_t kMaxCountPct = 300;
// The bosses' values have no slider: the automatic scaler's at 16 players
// (kMaxPlayers) are the most, life 1600 and damage 400.
inline constexpr std::uint16_t kMaxBossHpPct = 1600;
inline constexpr std::uint16_t kMaxBossDamagePct = 400;

constexpr bool valid_difficulty(const DifficultyValues& v) {
  return v.hp_pct >= kMinHpPct && v.hp_pct <= kMaxHpPct && v.damage_pct >= kMinDamagePct &&
         v.damage_pct <= kMaxDamagePct && v.count_pct >= kMinCountPct &&
         v.count_pct <= kMaxCountPct && v.boss_hp_pct >= kMinHpPct && v.boss_hp_pct <= kMaxBossHpPct &&
         v.boss_damage_pct >= kMinDamagePct && v.boss_damage_pct <= kMaxBossDamagePct;
}

// The host's own choice (a preset, the sliders): bosses as the other enemies.
constexpr DifficultyValues bosses_as_enemies(DifficultyValues v) {
  v.boss_hp_pct = v.hp_pct;
  v.boss_damage_pct = v.damage_pct;
  return v;
}

constexpr DifficultyValues clamp_difficulty(const DifficultyValues& v) {
  return bosses_as_enemies({std::clamp(v.hp_pct, kMinHpPct, kMaxHpPct),
                            std::clamp(v.damage_pct, kMinDamagePct, kMaxDamagePct),
                            std::clamp(v.count_pct, kMinCountPct, kMaxCountPct)});
}

// A preset's values; `custom` has none of its own and gives normal's.
constexpr DifficultyValues preset_difficulty(DifficultyPreset preset) {
  switch (preset) {
    case DifficultyPreset::easy: return bosses_as_enemies({75, 50, 100});
    case DifficultyPreset::hard: return bosses_as_enemies({200, 150, 150});
    case DifficultyPreset::normal:
    case DifficultyPreset::custom: break;
  }
  return {100, 100, 100};
}

// The automatic scaler: one player plays the game as it is, and each further
// player adds 40 life, 20 damage and 25 enemies in percent, up to 300, 200
// and 200 (reached with 6 players). All the enemies' life together then
// grows about as fast as the number of swords: 1.75 times for 2 players,
// 2.7 for 3, 3.85 for 4, and 6 times from 6 players on.
// Bosses and mini-bosses (owner, 2026-11-14): each further player adds 100
// life and 20 damage, with no cap but the most players (16: 1600 and 400),
// so one boss lasts about as long for every number of swords.
inline constexpr std::uint16_t kAutoHpStep = 40;
inline constexpr std::uint16_t kAutoDamageStep = 20;
inline constexpr std::uint16_t kAutoCountStep = 25;
inline constexpr std::uint16_t kAutoMaxHpPct = 300;
inline constexpr std::uint16_t kAutoMaxDamagePct = 200;
inline constexpr std::uint16_t kAutoMaxCountPct = 200;
inline constexpr std::uint16_t kAutoBossHpStep = 100;
inline constexpr std::uint16_t kAutoBossDamageStep = 20;
inline constexpr std::size_t kAutoBossMostExtra = 15;  // 16 players
static_assert(100 + kAutoBossHpStep * kAutoBossMostExtra == kMaxBossHpPct);
static_assert(100 + kAutoBossDamageStep * kAutoBossMostExtra == kMaxBossDamagePct);

constexpr DifficultyValues auto_difficulty(std::size_t players) {
  // More than 10 further players change nothing (every cap is reached by 5).
  const auto extra = static_cast<std::uint16_t>(std::min<std::size_t>(players > 0 ? players - 1 : 0, 10));
  auto grown = [extra](std::uint16_t step, std::uint16_t most) {
    return static_cast<std::uint16_t>(std::min<unsigned>(100u + step * extra, most));
  };
  const auto boss_extra =
      static_cast<std::uint16_t>(std::min<std::size_t>(players > 0 ? players - 1 : 0, kAutoBossMostExtra));
  return {grown(kAutoHpStep, kAutoMaxHpPct), grown(kAutoDamageStep, kAutoMaxDamagePct),
          grown(kAutoCountStep, kAutoMaxCountPct),
          static_cast<std::uint16_t>(100u + kAutoBossHpStep * boss_extra),
          static_cast<std::uint16_t>(100u + kAutoBossDamageStep * boss_extra)};
}

// What a session's difficulty is set to: automatic until the host chooses.
struct DifficultyChoice {
  bool auto_scale = true;
  DifficultyPreset preset = DifficultyPreset::normal;  // when not automatic
  DifficultyValues custom;                             // when the preset is custom
  bool operator==(const DifficultyChoice&) const = default;
};

// The values in force for `choice` with `players` in the session. Only the
// automatic scaler looks at `players`: the host's own choice is fixed.
constexpr DifficultyValues difficulty_in_force(const DifficultyChoice& choice, std::size_t players) {
  if (choice.auto_scale) return auto_difficulty(players);
  if (choice.preset == DifficultyPreset::custom) return clamp_difficulty(choice.custom);
  return preset_difficulty(choice.preset);
}

// (Navi) New Game+ (lichii/hook/coop_ngplus.h): each level after Ganon is
// beaten multiplies the values in force, on top of the players' or the
// host's: enemy and boss life by 1 + 0.5 x level, the damage they deal by
// 1 + 0.25 x level; the number of enemies stays. Kept within every end
// (valid_difficulty), so the codec takes it. Level 0: unchanged.
inline constexpr std::uint8_t kMaxNgPlus = 9;
inline constexpr std::uint16_t kNgPlusHpStep = 50;
inline constexpr std::uint16_t kNgPlusDamageStep = 25;

constexpr DifficultyValues ngplus_difficulty(DifficultyValues v, std::uint8_t level) {
  if (level == 0) return v;
  const unsigned l = level < kMaxNgPlus ? level : kMaxNgPlus;
  auto grown = [l](std::uint16_t pct, unsigned step, std::uint16_t least, std::uint16_t most) {
    const unsigned to = static_cast<unsigned>(pct) * (100u + step * l) / 100u;
    return static_cast<std::uint16_t>(std::clamp<unsigned>(to, least, most));
  };
  v.hp_pct = grown(v.hp_pct, kNgPlusHpStep, kMinHpPct, kMaxHpPct);
  v.damage_pct = grown(v.damage_pct, kNgPlusDamageStep, kMinDamagePct, kMaxDamagePct);
  v.boss_hp_pct = grown(v.boss_hp_pct, kNgPlusHpStep, kMinHpPct, kMaxBossHpPct);
  v.boss_damage_pct = grown(v.boss_damage_pct, kNgPlusDamageStep, kMinDamagePct, kMaxBossDamagePct);
  return v;
}
static_assert(ngplus_difficulty({100, 100, 100, 100, 100}, 1) == DifficultyValues{150, 125, 100, 150, 125});
static_assert(ngplus_difficulty({300, 200, 200, 400, 160}, 9) == DifficultyValues{500, 400, 200, 1600, 400});
static_assert(ngplus_difficulty({75, 50, 100, 75, 50}, 0) == DifficultyValues{75, 50, 100, 75, 50});

}  // namespace coop::protocol
