// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_enemy_shots.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace CoopEnemyShots {

namespace {

// Actor names from the game's Pack/Actor (build e0f3ccfaf50065ae). Beams,
// breath and auras (BeamosBeam, Enemy_DungeonBoss_Barinade_Beam,
// DungeonBossVolvagia_FireBreath, Bubble_Green_Bullet_Aura) are not shots:
// they last and follow their maker, so they are left to each game.
// Enemy_Scrub_Business_Seed is not a shot: it is the business scrub that
// sells seeds (it has its own talk, Event/EventFlow), a shared enemy.
struct Shot {
    std::string_view prefix;  // the shot's name starts with it
    std::array<std::string_view, 3> makers;
};
constexpr std::array<Shot, 12> kShots{{
    {"Octorok_Bullet", {"Enemy_Octorok"}},
    {"Dodongo_Bullet_", {"Enemy_Dodongo"}},
    {"DungeonBossDodongo_Bullet_", {"Enemy_DungeonBoss_Dodongo"}},
    {"ScrubDeku_DekuNut", {"Enemy_Scrub_"}},
    {"ScrubForestStage_DekuNut", {"Enemy_Scrub_ForestStage"}},  // each game's own show: kept
    {"SkullKid_Bullet", {"Enemy_SkullKid"}},
    {"PoeHyrule_Bullet_", {"Enemy_Poe"}},
    {"FlareDancer_Bullet_", {"Enemy_MiddleBoss_FlareDancer"}},
    {"DungeonBossGanondrof_Bullet_", {"Enemy_DungeonBoss_PhantomGanon"}},
    {"DungeonBossPhantomGanon_Bullet_", {"Enemy_DungeonBoss_PhantomGanon"}},
    {"PhantomGanon_Bullet_", {"Enemy_DungeonBoss_PhantomGanon"}},
    {"Enemy_MoblinClub_ShockWave", {"Enemy_Moblin"}},
}};
// Twinrova and Ganon(dorf) shots: their own table (more makers).
constexpr std::array<Shot, 5> kBossShots{{
    {"Twinrova_Bullet_", {"Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume"}},
    {"KotakeKoumeTwinrova_Bullet_", {"Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume"}},
    {"Ganon_Bullet_", {"Enemy_DungeonBoss_Ganon"}},
    {"Ganondorf_Bullet_", {"Enemy_DungeonBoss_Ganon"}},
    {"DungeonBossGanon_Bullet_", {"Enemy_DungeonBoss_Ganon"}},
}};

const Shot* Find(std::string_view name) {
    if (name.ends_with("_MirrorShieldReflect")) {
        return nullptr;  // sent back by Link's shield: his, not the boss's
    }
    for (const Shot& s : kShots) {
        if (name.starts_with(s.prefix)) {
            return &s;
        }
    }
    for (const Shot& s : kBossShots) {
        if (name.starts_with(s.prefix)) {
            return &s;
        }
    }
    return nullptr;
}

bool EnvOff(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool off = std::string_view(value) == "0";
    std::free(value);
    return off;
#else
    const char* v = std::getenv(name);
    return v != nullptr && std::string_view(v) == "0";
#endif
}

float Distance(const Vec3& a, const Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

std::mutex g_mutex;
std::vector<Shooter> g_shooters;
std::uint64_t g_at = 0;

} // namespace

bool IsEnemyShot(std::string_view name) {
    return Find(name) != nullptr;
}

std::vector<std::string_view> MakersOf(std::string_view shot) {
    std::vector<std::string_view> out;
    if (const Shot* s = Find(shot)) {
        for (const std::string_view m : s->makers) {
            if (!m.empty()) {
                out.push_back(m);
            }
        }
    }
    return out;
}

float MakerRange(std::string_view shot) {
    for (const std::string_view m : MakersOf(shot)) {
        if (m.starts_with("Enemy_DungeonBoss_") || m.starts_with("Enemy_MiddleBoss_")) {
            return 30.0f;
        }
    }
    return 10.0f;
}

Verdict Decide(std::string_view shot, const Vec3& at, std::span<const Shooter> shooters) {
    const auto makers = MakersOf(shot);
    if (makers.empty()) {
        return Verdict::keep;
    }
    const float range = MakerRange(shot);
    const Shooter* best = nullptr;
    float best_distance = range;
    for (const Shooter& s : shooters) {
        bool maker = false;
        for (const std::string_view m : makers) {
            maker = maker || s.name.starts_with(m);
        }
        if (!maker || IsEnemyShot(s.name)) {
            continue;
        }
        const float d = Distance(s.position, at);
        if (std::isfinite(d) && d <= best_distance) {
            best = &s;
            best_distance = d;
        }
    }
    if (best == nullptr) {
        return Verdict::keep;
    }
    switch (best->owner) {
    case Owner::ours:
        return Verdict::send;
    case Owner::theirs:
        return Verdict::hide;
    default:
        return Verdict::keep;
    }
}

const char* NameOf(Verdict verdict) {
    switch (verdict) {
    case Verdict::send:
        return "sent (our enemy's)";
    case Verdict::hide:
        return "deleted (a copy's; the owner's comes)";
    default:
        return "kept here (no owner known)";
    }
}

void NoteShooters(std::vector<Shooter> shooters, std::uint64_t now_us) {
    std::lock_guard lock(g_mutex);
    g_shooters = std::move(shooters);
    g_at = now_us;
}

std::vector<Shooter> Shooters(std::uint64_t now_us) {
    std::lock_guard lock(g_mutex);
    if (g_at == 0 || now_us < g_at || now_us - g_at > kFreshUs) {
        return {};
    }
    return g_shooters;
}

bool On() {
    static const bool on = !EnvOff("LICHII_COOP_ENEMY_SHOTS");
    return on;
}

bool HideOn() {
    static const bool on = !EnvOff("LICHII_COOP_ENEMY_SHOTS_HIDE");
    return on;
}

} // namespace CoopEnemyShots
