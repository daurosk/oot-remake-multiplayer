// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_enemy_shots.h"

#include <array>
#include <atomic>
#include <chrono>
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
// sells seeds (it has its own talk, Event/EventFlow), each game's own shop
// (CoopObjects::IsOwnPuzzle, GitHub 253).
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
    {"DungeonBossGanondrof_Bullet_", {"Enemy_DungeonBoss_PhantomGanon", "Enemy_DungeonBoss_Ganondorf"}},
    {"DungeonBossPhantomGanon_Bullet_", {"Enemy_DungeonBoss_PhantomGanon"}},
    {"PhantomGanon_Bullet_", {"Enemy_DungeonBoss_PhantomGanon"}},
    {"Enemy_MoblinClub_ShockWave", {"Enemy_Moblin"}},
}};
// Twinrova and Ganon(dorf) shots: their own table (more makers).
constexpr std::array<Shot, 5> kBossShots{{
    {"Twinrova_Bullet_", {"Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume"}},
    {"KotakeKoumeTwinrova_Bullet_", {"Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume"}},
    {"Ganon_Bullet_", {"Enemy_DungeonBoss_Ganon"}},
    {"Ganondorf_Bullet_", {"Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganon"}},
    {"DungeonBossGanon_Bullet_", {"Enemy_DungeonBoss_Ganon"}},
}};

// Split shots (docs/boss-multiplayer.md): every game keeps the shot its own copy of the boss fires and the owner's
// is not sent, so the attack is real in every game and hits every player as it would alone. Phantom Ganon's leap
// out of a painting (stage 1): his thunder bolt to the room's middle and the four shockwaves it sends out in a
// cross. As copies flown on the owner's path the shockwaves hurt nobody but the owner's Link. Stage 2: his
// rally ball (Ganondorf's the same): each player rallies their own; the owner's Ganon sends it back as alone, a
// copy cannot (its AI runs in the owner's game), so a ball sent back into a copy stuns him there, and the stun
// hands him to that player (the stun hand-over) for the sword: the first ball back stuns him for the group.
// Each game sees only its own: none is sent.
// The rest of the bosses the same way (every-stage two-game runs, 2026-11-21: each of these was one shot, sent from
// the owner's game): Twinrova's beams, the floor they leave and the guard breakers (each player soaks their own in
// the Mirror Shield; one sent back into her stuns her there, and the stun hand-over gives her to that player),
// Ganondorf's and Ganon's balls, shocks and shockwaves, King Dodongo's fire breath, and the Moblin's club
// shockwave (a ground wave like Phantom Ganon's).
// FlareDancer_Bullet_ (FireBall, FireDanceBall, made by Enemy_MiddleBoss_FlareDancer_FireBody; enemy survey
// 2026-11-21). PoeHyrule_Bullet_ (the Poe's fireball) the same: each player gets their own. Gohma's eggs
// (made minions), Volvagia's rocks (the arena's, each game's own) and the beams are not shots. Never split: Octorok_Bullet and ScrubDeku_DekuNut (the player shot at sends them back).
constexpr std::array<std::string_view, 13> kSplit{{"PhantomGanon_Bullet_Thunder", "DungeonBossPhantomGanon_Bullet_",
                                                    "DungeonBossGanondrof_Bullet_", "Twinrova_Bullet_",
                                                    "KotakeKoumeTwinrova_Bullet_", "Ganondorf_Bullet_", "Ganon_Bullet_",
                                                    "DungeonBossGanon_Bullet_", "DungeonBossDodongo_Bullet_",
                                                    "Enemy_MoblinClub_ShockWave", "Dodongo_Bullet_",
                                                    "FlareDancer_Bullet_", "PoeHyrule_Bullet_"}};

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
std::chrono::steady_clock::time_point g_noted{};  // (this PC's clock, for StopAtCreate)
std::atomic<std::uint64_t> g_stopped{0};

} // namespace

bool IsEnemyShot(std::string_view name) {
    return Find(name) != nullptr;
}

bool IsRallyBall(std::string_view name) {
    return IsEnemyShot(name) && (name.ends_with("_Bullet_LightBall") || name.ends_with("_Bullet_ChargeLightBall"));
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

bool IsSplitShot(std::string_view name) {
    if (!SplitOn() || !IsEnemyShot(name)) {
        return false;
    }
    for (const std::string_view s : kSplit) {
        if (name.starts_with(s)) {
            return true;
        }
    }
    return false;
}

Verdict Decide(std::string_view shot, const Vec3& at, std::span<const Shooter> shooters) {
    const auto makers = MakersOf(shot);
    if (makers.empty() || IsSplitShot(shot)) {
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
    g_noted = std::chrono::steady_clock::now();
}

bool StopAtCreate(std::string_view shot, const Vec3& at, std::uint64_t shooter) {
    if (!IsEnemyShot(shot) || IsSplitShot(shot) || !On() || !HideOn() || !StopOn()) {
        return false;
    }
    // (waits: a busy list once let a copy's own shot be made beside the owner's; its holders only copy the list)
    std::lock_guard lock(g_mutex);
    if (g_at == 0 ||
        std::chrono::steady_clock::now() - g_noted > std::chrono::microseconds(kFreshUs)) {
        return false;
    }
    const Shooter* by = nullptr;
    for (const Shooter& s : g_shooters) {
        if (shooter != 0 && s.address == shooter) {
            by = &s;
            break;
        }
    }
    if (by != nullptr) {
        if (by->owner != Owner::theirs) {
            return false;  // ours fires; no owner known: left alone
        }
    } else if ((at[0] == 0.0f && at[1] == 0.0f && at[2] == 0.0f) || Decide(shot, at, g_shooters) != Verdict::hide) {
        return false;
    }
    g_stopped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::uint64_t StoppedAtCreate() {
    return g_stopped.load(std::memory_order_relaxed);
}

bool StopOn() {
    static const bool on = !EnvOff("LICHII_COOP_ENEMY_SHOTS_STOP");
    return on;
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

bool SplitOn() {
    static const bool on = !EnvOff("LICHII_COOP_SPLIT_SHOTS");
    return on;
}

} // namespace CoopEnemyShots
