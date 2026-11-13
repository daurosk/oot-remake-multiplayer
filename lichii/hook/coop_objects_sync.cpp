// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_objects_sync.h"
#include "coop_enemy_shots.h"
#include "coop_guard.h"
#include "coop_world.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <type_traits>
#include <variant>

namespace CoopObjects {

namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kNoHash = ~std::uint64_t{0};

bool Moved(const proto::Vec3& a, const proto::Vec3& b) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::fabs(a[i] - b[i]) > 0.01f) {
            return true;
        }
    }
    return false;
}

bool Turned(const std::optional<proto::Quat>& a, const std::optional<proto::Quat>& b) {
    if (a.has_value() != b.has_value()) {
        return true;
    }
    if (!a) {
        return false;
    }
    float dot = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        dot += (*a)[i] * (*b)[i];
    }
    return std::fabs(dot) < 0.99995f;  // about 1 degree
}

proto::Vec3 Lerp(const proto::Vec3& a, const proto::Vec3& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// Normalised linear blend, the short way round.
proto::Quat Nlerp(const proto::Quat& a, proto::Quat b, float t) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        dot += a[i] * b[i];
    }
    if (dot < 0.0f) {
        for (float& v : b) {
            v = -v;
        }
    }
    proto::Quat q{};
    float len = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        q[i] = a[i] + (b[i] - a[i]) * t;
        len += q[i] * q[i];
    }
    len = std::sqrt(len);
    if (len < 1e-6f) {
        return a;
    }
    for (float& v : q) {
        v /= len;
    }
    return q;
}

bool Finite(const proto::Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float Distance(const proto::Vec3& a, const proto::Vec3& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

} // namespace

bool RemovedWhenTaken(std::string_view name) {
    constexpr std::string_view kPrefixes[] = {
        "Obj_TBox_",          // chests
        "Obj_RupeeToken",     // Gold Skulltula tokens and silver rupees
        "Obj_Token_Gold",     // a Gold Skulltula's token
        "Enemy_Skulltula_Gold",
        "Obj_Rupee_",         // a rupee placed once
        "Obj_Heart",          // hearts, heart containers, hearts in water
        "Obj_RedIce_",        // melted by blue fire
        "Obj_Ice_",
        "Obj_BrokenRock_",    // broken by a bomb or the hammer
        "WallCrack",          // bombed open
    };
    constexpr std::string_view kParts[] = {
        "Heart_Piece",        // Obj_*Heart_Piece*
        "SpiderWeb",          // burnt or broken (Deku Tree, Shadow Temple, Ganon's Castle)
        "_CrackWall_",        // bombed open
        "_CrackFloor_",
        "InsideTheDekuTree_FallingFloor_",  // fallen (the Shadow Temple's stays as ground: kept)
    };
    for (const std::string_view p : kPrefixes) {
        if (name.starts_with(p)) return true;
    }
    for (const std::string_view p : kParts) {
        if (name.find(p) != std::string_view::npos) return true;
    }
    return false;
}

std::optional<bool> BossAlive(const std::vector<Seen>& seen, std::span<const std::string_view> bodies) {
    bool unknown = false;
    for (const Seen& s : seen) {
        if (std::find(bodies.begin(), bodies.end(), s.name) == bodies.end()) continue;
        if (!s.life) {
            unknown = true;
        } else if (s.life->life > 0) {
            return true;
        }
    }
    if (unknown) return std::nullopt;
    return false;
}

bool IsShared(std::string_view name) {
    if (IsOwnPuzzle(name)) return false;  // before the Npc_ rule: the pond owner is one
    // Enemy shots (Enemy_MoblinClub_ShockWave is named like an enemy)
    // stay with the spawn sync: the game whose enemy fired one sends it.
    if (CoopEnemyShots::IsEnemyShot(name)) return false;
    if (name.starts_with("Npc_")) {
        // Each player's own: Navi (the fairy sync has her), the equipment
        // a player or NPC holds, and invisible helpers.
        return !name.starts_with("Npc_Navi") && !name.starts_with("Npc_Equipment") &&
               !name.starts_with("Npc_Invisible");
    }
    // The horse a player rides (CoopRide::kHorse) is each game's own: shared,
    // one game's Epona was a copy, frozen or pulled back under its rider, or
    // the other game drew two horses (its copy and the ride's stand-in).
    // The ride sync (coop_ride.h) shows the other player's horse instead.
    if (name == "Animal_Epona_Adult") return false;
    return name.starts_with("Enemy_") || name.starts_with("Animal_") ||
           CoopWorld::IsBreakable(name);
}

bool IsOwnPuzzle(std::string_view name) {
    // Issue 149. The Deku Tree's (and Kokiri Forest's) Scrub Brothers are an
    // order puzzle (hit 2, 3, 1): Scrub_Deku_Brothers_Manager counts the hits
    // in its own code, from the hits made in its own game; a stun changes no
    // life, so no DAMAGE ever carries another player's hit. Shared as enemy
    // copies, the owner's animation was played over a non-owner's own hit
    // reaction, which could spoil that player's solo try. So each game runs
    // its own brothers; the solve (InsideTheDekuTree_RoomK_Cleared,
    // ..._EnemyScrubDekuDestroy_003, set by the last scrub's talk) is a shared
    // dungeon flag and opens the room for everyone. The Lost Woods stage scrubs
    // (Enemy_Scrub_ForestStage, ScrubForestStage_ProcessOrderIndex) are an
    // ordered show of the same kind.
    // Issue 151. The fishing pond's owner (Npc_Hylia_Fishman_001_Child,
    // _Adult, Lake Hylia, in the Hyrule Field Banc) takes each player's fee,
    // hands over the rod and judges the catch in that game's own events, so
    // each game runs its own; shared, his talk animations came from the
    // other game. Only the prizes are shared (MiniGame_Fishing_GetHeartPiece
    // and _GetGoldScale, coop/host game_items). The fish are shared (IsFish,
    // fishing-2): the lure rules give each fish to the game whose lure it
    // goes for.
    return name.starts_with("Enemy_Scrub_Deku_Brothers") ||
           name.starts_with("Enemy_Scrub_ForestStage") || name.starts_with("Npc_Hylia_Fishman");
}

bool IsFish(std::string_view name) {
    return name.starts_with("Animal_Fish_");
}

bool IsGuard(std::string_view name) {
    return name.starts_with("Npc_Hylia_Soldier") || name.starts_with("Npc_Hylia_ImperialSoldier") ||
           name.starts_with("Npc_Gerudo_GEN_SecurityGuard") ||
           name.starts_with("Npc_Gerudo_FortressGuard");
}

bool IsGuardReaction(std::string_view name) {
    constexpr std::string_view kWords[] = {"Notice", "Discover", "Surprise", "Alert",
                                           "Found",  "Salute",   "Greeting", "Talk"};
    for (const std::string_view w : kWords) {
        if (name.find(w) != std::string_view::npos) return true;
    }
    return false;
}

bool IsRolled(std::string_view name) {
    // Enemy_Stalchild and Enemy_Stalchild_Big; not the picture book's
    // Enemy_Stalchild_Dictionary actors.
    return name == "Enemy_Stalchild" || name == "Enemy_Stalchild_Big";
}

namespace {

// Parts by exact actor name (Pack/Actor in the game's romfs, 2026-11-09).
// The names a boss's own parts carry are sure; the summoned ones (eggs,
// larvae, a flock) are read from the names and checked in a fight.
struct PartRule {
    std::string_view member;
    GroupPart part;
};

const std::vector<PartRule>& PartRules() {
    constexpr float kAny = 0.0f;
    constexpr float kSummoned = 40.0f;  // what a boss calls up stays near it
    static const std::vector<PartRule> rules = [] {
        std::vector<PartRule> r;
        auto boss = [&r](std::string_view root, std::initializer_list<std::string_view> parts,
                         float reach) {
            for (std::string_view p : parts) r.push_back({p, GroupPart{{root}, reach, true}});
        };
        auto enemy = [&r](std::string_view root, std::initializer_list<std::string_view> parts,
                          float reach) {
            for (std::string_view p : parts) r.push_back({p, GroupPart{{root}, reach, false}});
        };
        // Bosses.
        boss("Enemy_DungeonBoss_Gohma",
             {"Enemy_DungeonBoss_Gohma_Egg", "Enemy_Gohma_Egg", "Enemy_Gohma_Larva"}, kSummoned);
        boss("Enemy_DungeonBoss_Dodongo",
             {"Enemy_DungeonBoss_Dodongo_Attention",
              "Enemy_DungeonBoss_Dodongo_Attention_RollingCenter",
              "Enemy_DungeonBoss_Dodongo_SpawnBabyDodongo"},
             kAny);
        boss("Enemy_DungeonBoss_Dodongo", {"Enemy_BabyDodongo"}, kSummoned);
        boss("Enemy_DungeonBoss_Barinade",
             {"Enemy_DungeonBoss_Barinade_Arm", "Enemy_DungeonBoss_Barinade_Attention",
              "Enemy_DungeonBoss_Barinade_Beam", "Enemy_DungeonBoss_Barinade_Jellyfish"},
             kAny);
        boss("Enemy_DungeonBoss_PhantomGanon", {"Enemy_PhantomGanonHorse"}, kAny);
        boss("Enemy_DungeonBoss_Morpha",
             {"Enemy_DungeonBoss_Morpha_Attention", "Enemy_DungeonBoss_Morpha_Attention_Spin",
              "Enemy_DungeonBoss_Morpha_Tentacle"},
             kAny);
        boss("Enemy_DungeonBoss_BongoBongo",
             {"Enemy_DungeonBoss_BongoBongo_LeftHand", "Enemy_DungeonBoss_BongoBongo_RightHand"},
             kAny);
        boss("Enemy_DungeonBoss_Ganon",
             {"Enemy_DungeonBoss_Ganon_Attention", "Enemy_DungeonBoss_Ganon_Crystal",
              "Enemy_DungeonBoss_Ganon_LegAttention", "Enemy_DungeonBoss_Ganon_SecondHalfBattle"},
             kAny);
        // Twinrova: the two witches and the fused one follow the battle
        // manager when it is loaded, else Twinrova.
        for (std::string_view w : {"Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume"}) {
            r.push_back({w, GroupPart{{"Enemy_KotakeKoumeBattleManager",
                                       "Enemy_DungeonBoss_Twinrova"},
                                      kAny, true}});
        }
        boss("Enemy_KotakeKoumeBattleManager", {"Enemy_DungeonBoss_Twinrova"}, kAny);
        // Mini-bosses.
        boss("Enemy_MiddleBoss_BigOcto", {"Enemy_MiddleBoss_BigOcto_Attention"}, kAny);
        boss("Enemy_MiddleBoss_BigOcto", {"Enemy_Shabom_BigOctoChild"}, kSummoned);
        boss("Enemy_MiddleBoss_DarkLink", {"Enemy_MiddleBoss_DarkLink_EffectLocater"}, kAny);
        boss("Enemy_MiddleBoss_FlareDancer", {"Enemy_MiddleBoss_FlareDancer_FireBody"}, kAny);
        boss("Enemy_MiddleBoss_Poe_Meg", {"Enemy_MiddleBoss_Poe_Meg_Dummy"}, kAny);
        boss("Enemy_DeadHand", {"Enemy_DeadHand_Hand"}, kSummoned);
        // Enemies with a part or a brood. Several of one kind may share a
        // room: a part follows the nearest.
        enemy("Enemy_Dodongo", {"Enemy_Dodongo_Attention"}, 10.0f);
        enemy("Enemy_Tailpasaran", {"Enemy_Tailpasaran_Attention"}, 10.0f);
        enemy("Enemy_Anubis", {"Enemy_Anubis_MagicField"}, 20.0f);
        enemy("Enemy_FlockGuayManager", {"Enemy_Guay_Flock"}, 60.0f);
        r.push_back({"Enemy_Peahat_Larva",
                     GroupPart{{"Enemy_Peahat_AircraftCarrier", "Enemy_Peahat"}, 30.0f, false}});
        return r;
    }();
    return rules;
}

}  // namespace

const GroupPart* PartOf(std::string_view name) {
    for (const PartRule& rule : PartRules()) {
        if (rule.member == name) return &rule.part;
    }
    return nullptr;
}

std::optional<std::int32_t> RaisedMaxLife(std::int32_t link_max, std::int32_t flag) {
    if (link_max <= 0 || flag <= link_max || flag > kMostLifeQuarters) return std::nullopt;
    return flag;
}

bool IsBoss(std::string_view name) {
    if (name.find("_Dictionary") != std::string_view::npos) return false;
    if (name.starts_with("Enemy_DungeonBoss_") || name.starts_with("Enemy_MiddleBoss_")) {
        return true;
    }
    for (const PartRule& rule : PartRules()) {
        if (rule.part.boss && rule.part.roots.front() == name) return true;
    }
    return false;
}

std::vector<proto::ObjectGroupEntry> GroupsOf(
    const std::vector<GroupInput>& walk,
    const std::map<proto::ObjectId, proto::ObjectId>& linked) {
    const auto dist2 = [](const GroupInput& a, const GroupInput& b) {
        float d2 = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) {
            const float d = a.position[i] - b.position[i];
            d2 += d * d;
        }
        return d2;
    };
    std::vector<proto::ObjectGroupEntry> out;
    for (const GroupInput& m : walk) {
        const GroupPart* part = PartOf(m.name);
        if (part == nullptr) {
            if (IsBoss(m.name)) out.push_back({m.id, m.id, proto::object_group::boss});
            continue;
        }
        const GroupInput* root = nullptr;
        // The root linked last time stays while within kGroupKeep times the
        // reach, so a part near the edge does not flip between groups.
        if (const auto was = linked.find(m.id); was != linked.end()) {
            for (const GroupInput& r : walk) {
                if (r.id != was->second) continue;
                const float keep = part->reach * kGroupKeep;
                const float d2 = dist2(r, m);
                if (std::isfinite(d2) && (part->reach <= 0.0f || d2 <= keep * keep)) root = &r;
                break;
            }
        }
        for (std::size_t n = 0; root == nullptr && n < part->roots.size(); ++n) {
            float best = 0.0f;
            for (const GroupInput& r : walk) {
                if (r.name != part->roots[n] || r.id == m.id) continue;
                const float d2 = dist2(r, m);
                if (!std::isfinite(d2)) continue;
                if (part->reach > 0.0f && d2 > part->reach * part->reach) continue;
                // Nearest; on a tie the lower id, so every game picks the same.
                if (root == nullptr || d2 < best || (d2 == best && r.id < root->id)) {
                    root = &r;
                    best = d2;
                }
            }
        }
        if (root != nullptr) {
            out.push_back({m.id, root->id, part->boss || IsBoss(m.name)
                                               ? proto::object_group::boss
                                               : std::uint8_t{0}});
        } else if (linked.contains(m.id) || IsBoss(m.name)) {
            // Linked before, its root gone or out of reach: once, on its own
            // again. A part never linked here sends nothing, so it cannot
            // undo another game's link (and a stray larva gets no boss lock);
            // a boss that is a part (Twinrova) still gets its own boss lock.
            out.push_back({m.id, m.id, IsBoss(m.name) ? proto::object_group::boss
                                                      : std::uint8_t{0}});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.member < b.member; });
    // One entry per member (the codec refuses a member twice).
    out.erase(std::unique(out.begin(), out.end(),
                          [](const auto& a, const auto& b) { return a.member == b.member; }),
              out.end());
    if (out.size() > proto::kMaxGroupEntries) out.resize(proto::kMaxGroupEntries);
    return out;
}

bool IsHatchingEgg(std::string_view name) {
    return name == "Enemy_Gohma_Egg" || name == "Enemy_DungeonBoss_Gohma_Egg";
}

bool IsHatchling(std::string_view name) {
    return name == "Enemy_Gohma_Larva";
}

bool IsAnnounced(std::string_view name) {
    return name == "Enemy_Skulltula_Gold";
}

std::string_view AnimationBase(std::string_view name) {
    for (const std::string_view suffix : {"_ModelOnlyLOD", "_LOD"}) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            return name.substr(0, name.size() - suffix.size());
        }
    }
    return name;
}

bool IsDeathAnimation(std::string_view name) {
    for (const std::string_view word : {"Die", "Dead", "Death"}) {
        for (std::size_t at = name.find(word); at != std::string_view::npos;
             at = name.find(word, at + 1)) {
            const bool starts = at == 0 || name[at - 1] == '_';
            const std::size_t end = at + word.size();
            const bool ends = end == name.size() || name[end] == '_' ||
                              (name[end] >= '0' && name[end] <= '9');
            if (starts && ends) {
                return true;
            }
        }
    }
    return false;
}

bool IsStunAnimation(std::string_view name) {
    const std::string_view base = AnimationBase(name);
    const std::string_view first = base.substr(0, base.find('_'));
    return first == "Stun" || first == "Down" || first == "Fall" || first == "Faint";
}

proto::Quat QuatOf(const Mat3& m) {
    // m[3 * row + column], Shepperd's method. Rows that are not unit length
    // (a read of something else) give no turn.
    for (std::size_t row = 0; row < 3; ++row) {
        const float len = m[3 * row] * m[3 * row] + m[3 * row + 1] * m[3 * row + 1] +
                          m[3 * row + 2] * m[3 * row + 2];
        if (!std::isfinite(len) || std::fabs(len - 1.0f) > 0.1f) {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        }
    }
    const float trace = m[0] + m[4] + m[8];
    proto::Quat q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, 0.25f * s};
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q = {0.25f * s, (m[1] + m[3]) / s, (m[2] + m[6]) / s, (m[7] - m[5]) / s};
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q = {(m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s, (m[2] - m[6]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q = {(m[2] + m[6]) / s, (m[5] + m[7]) / s, 0.25f * s, (m[3] - m[1]) / s};
    }
    float len = 0.0f;
    for (const float v : q) {
        len += v * v;
    }
    len = std::sqrt(len);
    if (!std::isfinite(len) || len < 1e-6f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    for (float& v : q) {
        v /= len;
    }
    return q;
}

Mat3 MatOf(const proto::Quat& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
}

std::uint64_t SceneSpawnId(std::string_view scene, std::string_view name, unsigned order) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
        h ^= 0xff;  // a separator no name has
        h *= 0x100000001b3ULL;
    };
    mix(scene);
    mix(name);
    for (int i = 0; i < 4; ++i) {
        h ^= (order >> (8 * i)) & 0xff;
        h *= 0x100000001b3ULL;
    }
    return h | (std::uint64_t{1} << 63);
}

void Sync::Receive(const proto::Message& message) {
    if (const auto* p = std::get_if<proto::PlayerSample>(&message)) {
        if (p->player_number != proto::kPlayerSelf) {
            std::lock_guard lock(inbox_mutex_);
            if (p->scene) {
                scenes_[p->player_number] = {*p->scene, p->time.server_time_us};
            }
            if (p->position && Finite(*p->position)) {
                places_[p->player_number] = {*p->position, p->time.server_time_us};
            }
        }
        return;
    }
    const bool wanted = std::visit(
        [](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            return std::is_same_v<T, proto::ObjectOwner> || std::is_same_v<T, proto::ObjectSample> ||
                   std::is_same_v<T, proto::HandoverAsk> || std::is_same_v<T, proto::HandoverState> ||
                   std::is_same_v<T, proto::Damage> || std::is_same_v<T, proto::ObjectSpawn> || std::is_same_v<T, proto::ObjectRemove>;
        },
        message);
    if (!wanted) {
        return;
    }
    std::lock_guard lock(inbox_mutex_);
    if (inbox_.size() < 4096) {
        inbox_.push_back(message);
    }
}

void Sync::Take(std::uint64_t hash) {
    if (hash == 0 || hash == kNoHash) {
        return;
    }
    std::lock_guard lock(inbox_mutex_);
    if (takes_in_.size() < 1024) {
        takes_in_.push_back(hash);
    }
}

bool Sync::Hatch(std::uint8_t self, const Seen& s,
                 std::vector<proto::Message>& send) {
    // An egg that already left our walk alive: it hatched into this one.
    for (auto it = eggs_.begin(); it != eggs_.end(); ++it) {
        if (Distance(it->position, s.position) <= kHatchReachM) {
            proto::ObjectRemove r;
            r.id = it->id;
            r.epoch = it->epoch;
            r.reason = proto::RemoveReason::gone;
            r.position = it->position;
            send.emplace_back(r);
            eggs_.erase(it);
            ++hatched_count_;
            notes_.emplace_back("hatched here: " + s.name);
            return true;
        }
    }
    // An egg of ours still on the walk (it goes a walk or two later).
    Object* best = nullptr;
    float best_d = kHatchReachM;
    for (auto& [address, e] : objects_) {
        if (!IsHatchingEgg(e.name) || e.hatched || e.id.kind != proto::ObjectKind::placed) {
            continue;
        }
        const auto owner = owners_.find(e.id);
        if (owner == owners_.end() || owner->second.player != self) {
            continue;
        }
        const float d = Distance(e.position, s.position);
        if (d <= best_d) {
            best = &e;
            best_d = d;
        }
    }
    if (best == nullptr) {
        return false;
    }
    best->hatched = true;
    ++hatched_count_;
    notes_.emplace_back("hatched here: " + s.name);
    return true;
}

// An egg of ours still on the walk, marked hatched, within two hatch reaches
// of `at` (a larva's egg is at most kHatchReachM from it either way).
Sync::Object* Sync::HatchedSibling(std::uint8_t self, const proto::Vec3& at) {
    Object* best = nullptr;
    float best_d = 2.0f * kHatchReachM;
    for (auto& [address, e] : objects_) {
        if (!IsHatchingEgg(e.name) || !e.hatched || e.id.kind != proto::ObjectKind::placed) {
            continue;
        }
        const auto owner = owners_.find(e.id);
        if (owner == owners_.end() || owner->second.player != self) {
            continue;
        }
        const float d = Distance(e.position, at);
        if (d <= best_d) {
            best = &e;
            best_d = d;
        }
    }
    return best;
}

void Sync::Reset() {
    scene_.clear();
    objects_.clear();
    taken_.clear();  // a new scene's actors were made knowing every key
    chest_animations_.clear();
    chest_shown_.clear();
    hidden_.clear();
    dead_.clear();
    order_.clear();
    owners_.clear();
    present_sent_.clear();
    present_at_ = 0;
    present_once_ = false;
    groups_sent_.clear();
    group_links_.clear();
    fish_sent_.clear();
    fish_at_ = 0;
    decided_.clear();
    eggs_.clear();
    bound_.clear();
    refused_.clear();
    decider_ = 0;
    // next_key_ and made_counter_ go on: the director never makes a key
    // twice, and the server keeps made ids until they are removed.
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
    takes_in_.clear();  // the new scene's actors are made knowing every key
}

std::uint8_t Sync::DeciderOf(std::uint8_t self, std::uint64_t now_us) const {
    std::uint8_t decider = self;
    std::lock_guard lock(inbox_mutex_);
    for (const auto& [player, seen] : scenes_) {
        const bool fresh = seen.second <= now_us ? now_us - seen.second < kSceneStaleUs
                                                 : seen.second - now_us < kSceneStaleUs;
        if (player != self && player != 0 && player < decider && fresh && seen.first == scene_) {
            decider = player;
        }
    }
    return decider;
}

std::uint8_t Sync::RollerFor(std::uint8_t self, const proto::Vec3& at, std::uint64_t now_us) const {
    const auto fresh = [now_us](std::uint64_t t) {
        return t <= now_us ? now_us - t < kSceneStaleUs : t - now_us < kSceneStaleUs;
    };
    std::uint8_t roller = self;
    std::lock_guard lock(inbox_mutex_);
    for (const auto& [player, seen] : scenes_) {
        if (player == self || player == 0 || player >= roller || !fresh(seen.second) || seen.first != scene_) {
            continue;
        }
        const auto place = places_.find(player);
        const bool near = place == places_.end() || !fresh(place->second.second) ||
                          Distance(place->second.first, at) <= kRollNearM;
        if (near) {
            roller = player;
        }
    }
    return roller;
}

void Sync::Bind(const std::map<std::uint64_t, std::uint64_t>& live) {
    bound_.clear();
    for (const auto& [key, address] : live) {
        if (decided_.contains(key) && address != 0) {
            bound_[address] = key;
        }
    }
}

std::vector<EnemyCopy> Sync::Creates() const {
    std::vector<EnemyCopy> out;
    for (const auto& [key, d] : decided_) {
        out.push_back({key, d.name, d.position, d.removed, d.dead ? std::uint8_t{0} : kRemakes});
    }
    return out;
}

std::optional<proto::ObjectId> Sync::IdOf(std::uint64_t address) const {
    const auto it = objects_.find(address);
    if (it == objects_.end()) {
        return std::nullopt;
    }
    return it->second.id;
}

std::optional<std::uint8_t> Sync::OwnerOf(std::uint64_t address) const {
    const auto it = objects_.find(address);
    if (it == objects_.end()) {
        return std::nullopt;
    }
    const auto owner = owners_.find(it->second.id);
    if (owner == owners_.end()) {
        return std::nullopt;
    }
    return owner->second.player;
}

std::vector<std::pair<std::uint64_t, std::string>> Sync::Owned() const {
    std::vector<std::pair<std::uint64_t, std::string>> out;
    for (const auto& [address, o] : objects_) {
        const auto owner = owners_.find(o.id);
        if (owner != owners_.end() && owner->second.player != 0 && owner->second.player == self_) {
            out.emplace_back(address, o.name);
        }
    }
    return out;
}

void Sync::Written(std::uint64_t address, std::int32_t life, std::uint64_t now_us) {
    const auto it = objects_.find(address);
    if (it == objects_.end()) {
        return;
    }
    Object& o = it->second;
    // While it waited the walk read the old life; that is the start of the drop.
    o.late_left = 0;  // nothing of it may be put back as a second fall
    o.late_until = 0;
    if (o.life && *o.life > life) {
        o.hurt_left += *o.life - life;
        o.hurt_target = life;
        // If the game does not take it, the life is written after kHurtWaitUs and hurt_left
        // is cleared, so it cannot swallow a later hit of our Link's.
        o.hurt_until = now_us + kHurtWaitUs;
    }
}

std::uint8_t Sync::OwnerOf(const proto::ObjectId& id) const {
    const auto it = owners_.find(id);
    return it == owners_.end() ? 0 : it->second.player;
}

proto::ObjectState Sync::StateOf(Object& o, std::uint64_t now_us) {
    proto::ObjectState state;
    state.time = {now_us, ++o.sequence};
    state.position = o.position;
    state.velocity = o.velocity;
    state.animation = o.animation;
    if (!o.slots.empty()) {
        state.slots = o.slots;  // issue 150: a boss's eye guard and the like
    }
    if (o.rotation) {
        state.rotation = QuatOf(*o.rotation);
    }
    if (o.life) {
        state.life = proto::ObjectLife{*o.life, o.max};
    }
    return state;
}

void Sync::ChaseStep(Object& o, std::uint64_t now_us) {
    if (o.chase_last == 0 || now_us < o.chase_last || now_us - o.chase_last > kChaseGapUs) {
        o.chase_first = now_us;
    }
    o.chase_last = now_us;
    if (now_us - o.chase_first >= kChaseHoldUs) {
        o.chase_until = now_us + kChaseKeepUs;
    }
}

bool Sync::EventNpc(std::uint64_t address, std::uint64_t now_us) const {
    const auto it = objects_.find(address);
    return it != objects_.end() && it->second.npc && it->second.event_until > now_us;
}

std::uint8_t Sync::OwnerAt(std::uint64_t address) const {
    const auto it = objects_.find(address);
    return it == objects_.end() ? 0 : OwnerOf(it->second.id);
}

std::size_t Sync::EventNpcs(std::uint64_t now_us) const {
    return static_cast<std::size_t>(std::count_if(objects_.begin(), objects_.end(), [now_us](const auto& e) {
        return e.second.npc && e.second.event_until > now_us;
    }));
}

std::size_t Sync::EventNpcsOurs(std::uint64_t now_us) const {
    if (self_ == 0) {
        return 0;
    }
    return static_cast<std::size_t>(std::count_if(objects_.begin(), objects_.end(), [&](const auto& e) {
        return e.second.npc && e.second.event_until > now_us && OwnerOf(e.second.id) == self_;
    }));
}

void Sync::NoteTakeover(std::uint64_t address, std::uint64_t now_us, std::string_view name) {
    const auto it = objects_.find(address);
    if (it == objects_.end()) {
        return;
    }
    Object& o = it->second;
    if (o.reacts || (o.guard && IsGuardReaction(name))) {
        o.took_over_at = now_us;
        o.took_over_name = std::string(name);
    }
}

std::optional<float> Sync::LureDistance(const proto::Vec3& at) const {
    std::optional<float> best;
    for (const proto::Vec3& lure : lures_) {
        if (!Finite(lure)) {
            continue;
        }
        const float d = Distance(at, lure);
        if (!best || d < *best) {
            best = d;
        }
    }
    return best;
}

void Sync::NoteDrift(std::uint64_t address, const proto::Vec3& put, const proto::Vec3& now,
                     std::uint64_t now_us) {
    if (!Finite(put) || !Finite(now)) {
        return;
    }
    const auto it = objects_.find(address);
    if (it == objects_.end() || !(it->second.reacts || it->second.fish)) {
        return;
    }
    Object& o = it->second;
    const auto owner = owners_.find(o.id);
    if (owner == owners_.end() || owner->second.player == 0 || owner->second.player == self_) {
        return;  // ours, or nobody's: the Mover does not place it
    }
    if (o.fish) {
        // Issue 151: our game's fish swims at our lure, or holds on it (a
        // bite). Another player's lure is a stand-in here that no fish sees.
        const auto before = LureDistance(put);
        const auto after = LureDistance(now);
        if (before && after && *after <= kLureReachM &&
            (*before - *after >= kLureStepM || *after <= kLureHookM)) {
            ChaseStep(o, now_us);
        }
        return;
    }
    if (!player_) {
        return;
    }
    auto distance = [this](const proto::Vec3& at) {
        float d2 = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) {
            const float d = at[i] - (*player_)[i];
            d2 += d * d;
        }
        return std::sqrt(d2);
    };
    const float before = distance(put);
    const float after = distance(now);
    if (after <= kChaseReachM && before - after >= kChaseStepM) {
        ChaseStep(o, now_us);
    }
}

void Sync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
                const std::vector<Seen>& seen, std::vector<proto::Message>& send,
                std::vector<LifeWrite>& writes) {
    if (scene != scene_) {
        const std::string keep = scene;
        Reset();
        scene_ = keep;
        scene_since_ = now_us;
    }

    // What this walk shows: new enemies get ids, known ones their life and
    // place; a drop of life on a copy we do not own is our hit.
    // A pickup taken elsewhere (taken_, from the last Apply) is hidden.
    std::map<std::uint64_t, const Seen*> now_seen;
    std::vector<const Seen*> fresh;
    std::set<std::uint64_t> hidden;
    {
        std::vector<std::uint64_t> takes;
        {
            std::lock_guard lock(inbox_mutex_);
            takes.swap(takes_in_);
        }
        // Takes that came while no walk ran are for actors made after them.
        const bool gap = !ticked_ || now_us < last_tick_us_ ||
                         now_us - last_tick_us_ > kTakeGapUs;
        if (!gap) {
            for (const std::uint64_t hash : takes) {
                // The wait counts from the first take of a key: the chest's
                // key and its opened flag may come in different frames.
                if (const auto [t, added] = taken_.try_emplace(hash); added) {
                    t->second.since_us = now_us;
                }
            }
        } else if (!takes.empty()) {
            notes_.emplace_back(std::to_string(takes.size()) +
                                " keys written for others while no walk ran: nothing removed for them");
        }
        last_tick_us_ = now_us;
        ticked_ = true;
    }
    {  // every chest's animation, and when it changed (kChestOpeningUs)
        std::map<std::uint64_t, ChestAnimation> chests;
        for (const Seen& s : seen) {
            if (!s.name.starts_with("Obj_TBox_")) {
                continue;
            }
            ChestAnimation now{s.animation ? s.animation->name : std::string(), 0};
            if (const auto was = chest_animations_.find(s.address); was != chest_animations_.end()) {
                now.changed_us = was->second.name == now.name ? was->second.changed_us : now_us;
            }
            chests.emplace(s.address, std::move(now));
        }
        chest_animations_.swap(chests);
        std::erase_if(chest_shown_, [this](const auto& e) { return !chest_animations_.contains(e.first); });
    }
    for (const Seen& s : seen) {
        if (s.hash != 0 && s.hash != kNoHash) {
            if (const auto t = taken_.find(s.hash); t != taken_.end() && RemovedWhenTaken(s.name)) {
                // A chest has no near rule (issue 87): it is bound beside our
                // Link too, and shown open (below, issue 195).
                const bool chest = s.name.starts_with("Obj_TBox_");
                const bool beside = player_ && Finite(*player_) && Finite(s.position) &&
                                    CoopGuard::Near(player_, s.position, kTakeNearM);
                const bool near = !chest && beside;
                const std::string animation = s.animation ? s.animation->name : std::string();
                const bool open = animation.find("Open") != std::string::npos;
                // Opening now: changed to an "Open" animation just before (never
                // a name alone: a closed chest's idle may have "Open" in it).
                bool opening = false;
                if (const auto c = chest_animations_.find(s.address); chest && open && c != chest_animations_.end()) {
                    opening = c->second.changed_us != 0 && now_us - c->second.changed_us <= kChestOpeningUs;
                }
                if (near && t->second.ticks < kTakeBindTicks && !t->second.near_said) {
                    t->second.near_said = true;
                    notes_.emplace_back(s.name + ": taken or opened in another game, left here: our Link is beside it");
                }
                if (!near && t->second.ticks < kTakeBindTicks && t->second.addresses.insert(s.address).second) {
                    char key[24];
                    std::snprintf(key, sizeof key, "%#llx", static_cast<unsigned long long>(s.hash));
                    if (chest) {
                        t->second.bound_animation[s.address] = animation;
                    }
                    if (chest && opening && beside) {
                        // Ours started opening just before the other's flag came,
                        // our Link beside it: both opened it at once. It stays;
                        // its content may count twice (a small key) and cannot be
                        // taken back here.
                        t->second.shown_open.insert(s.address);
                        notes_.emplace_back(s.name + " " + key +
                                            ": opened here and in another game at once (its content may count twice)");
                    } else if (chest && opening) {
                        // Away from our Link: its AI reacted to the shared flag.
                        t->second.shown_open.insert(s.address);
                        notes_.emplace_back(s.name + " " + key + ": opened in another game, shows open here");
                    } else {
                        notes_.emplace_back(s.name + " " + key +
                                            (chest ? ": opened in another game"
                                                   : ": taken or opened in another game, removed here"));
                    }
                }
                if (t->second.addresses.contains(s.address)) {
                    // A chest (issues 87, 195): shown open here, never removed
                    // unless the play of its open idle does not take.
                    if (chest) {
                        Taking& tk = t->second;
                        if (tk.shown_open.contains(s.address)) {
                            continue;
                        }
                        // Issue 195: never removed while it can be shown open.
                        // Its AI does not react to the shared flag while loaded
                        // (autotest logs: closed until our remove), so we play its
                        // own open idle on it (Plays, only over its closed idle).
                        if (!tk.show_failed.contains(s.address)) {
                            if (animation == kChestOpenIdle && !chest_shown_.contains(s.address)) {
                                // Made open already (its flag came before it
                                // was made): left so (the old rule removed these too).
                                tk.shown_open.insert(s.address);
                                notes_.emplace_back(s.name + ": opened in another game, already shows open here");
                                continue;
                            }
                            const auto shown = chest_shown_.find(s.address);
                            if (shown == chest_shown_.end()) {
                                chest_shown_.emplace(s.address, ChestShow{now_us, false});
                                notes_.emplace_back(s.name + ": opened in another game, shown open here (" +
                                                    kChestOpenIdle + " played)");
                                continue;
                            }
                            if (animation == kChestOpenIdle) {
                                if (!shown->second.took) {
                                    shown->second.took = true;
                                    notes_.emplace_back(s.name + ": opened in another game, shows open here");
                                }
                                continue;
                            }
                            if (animation.find(kChestClosedIdle) == std::string::npos && !animation.empty()) {
                                // Its own AI plays another: the play is not
                                // forced over that (PlayWish::only_over). Beside
                                // our Link: ours opens it again here.
                                chest_shown_.erase(shown);
                                tk.shown_open.insert(s.address);
                                if (beside) {
                                    ++chests_opened_twice_;
                                    notes_.emplace_back(s.name + ": opened here too after another game ('" +
                                                        animation + "', its content may count twice; " +
                                                        std::to_string(chests_opened_twice_) + " this session)");
                                } else {
                                    notes_.emplace_back(s.name + ": its own AI shows it open ('" + animation + "')");
                                }
                                continue;
                            }
                            if (shown->second.took) {
                                // Its AI put its closed idle back after it showed
                                // open (we play it again): the lid may flicker.
                                const unsigned n = ++shown->second.flicks;
                                if (n == 1 || n % 10 == 0) {
                                    notes_.emplace_back(s.name + ": closed idle back after it showed open, " +
                                                        std::to_string(n) + " walks (the lid may flicker)");
                                }
                                continue;
                            }
                            if (now_us - shown->second.since_us < kChestShowFailUs) {
                                continue;  // still to take
                            }
                            // Never showed open: the play does not take here.
                            // Removed as before, so its content is not given twice.
                            chest_shown_.erase(shown);
                            tk.show_failed.insert(s.address);
                            notes_.emplace_back(s.name + ": could not be shown open here, removed");
                        }
                    }
                    // A falling floor or a web's parts (dungeon-switches, the
                    // issue 177: the bridge and webs vanished): the fall
                    // comes here as the shifter's moves, the web's own break
                    // on life 0; removed only after they had time to play.
                    if (CoopWorld::IsFallTaken(s.name) && now_us - t->second.since_us < kTakeFallUs) {
                        continue;
                    }
                    // Its own break where the game has one: life 0, then the
                    // delete only if it still stands (the review of breakables:
                    // a web just vanished in the other game).
                    const bool lives = s.life && s.life->life > 0 && CoopWorld::IsBreakable(s.name);
                    auto broke = t->second.broke_at.find(s.address);
                    if (lives && broke == t->second.broke_at.end()) {
                        writes.push_back({s.address, 0, 0, {}});
                        t->second.broke_at.emplace(s.address, now_us);
                        notes_.emplace_back(s.name + ": broken by its own break (life 0)");
                        continue;  // not shared, not sent, not deleted yet
                    }
                    if (broke != t->second.broke_at.end() && now_us - broke->second < kTakeBreakUs) {
                        continue;  // its break is playing
                    }
                    hidden.insert(s.address);
                    continue;  // gone for everyone: not shared, not sent
                }
            }
        }
        if (!IsShared(s.name)) {
            continue;
        }
        now_seen[s.address] = &s;
        const auto it = objects_.find(s.address);
        if (it == objects_.end()) {
            fresh.push_back(&s);
            continue;
        }
        Object& o = it->second;
        if (now_us > o.seen_at && now_us - o.seen_at < 500'000 && Finite(s.position)) {
            const float dt = static_cast<float>(now_us - o.seen_at) / 1e6f;
            for (std::size_t i = 0; i < 3; ++i) {
                o.velocity[i] = (s.position[i] - o.position[i]) / dt;
            }
        } else {
            o.velocity = {};
        }
        o.seen_at = now_us;
        o.position = s.position;
        o.rotation = s.rotation;
        // Issue 150: a boss copy whose own game went into a stun the owner's
        // boss is not in (a seed in Gohma's eye takes no life). Its stun is
        // kept on screen like a hit reaction, and a DAMAGE of 0 asks the
        // server to hand the boss to us, whose boss is the stunned one.
        if (s.animation && IsStunAnimation(s.animation->name) && IsBoss(s.name) &&
            !(o.animation && IsStunAnimation(o.animation->name))) {
            const auto owner = owners_.find(o.id);
            const std::string owners = !o.received.empty() && o.received.back().animation
                                           ? o.received.back().animation->name
                                           : std::string{};
            if (owner != owners_.end() && owner->second.player != 0 &&
                owner->second.player != self && !IsStunAnimation(owners)) {
                o.react_at = now_us;
                o.react_over = owners;
                if (o.stun_at == 0 || now_us - o.stun_at >= kStunEveryUs) {
                    o.stun_due = true;
                }
            }
        }
        o.animation = s.animation;
        o.slots = s.slots;
        // A breakable's life can no longer be read once it has died (a crate
        // at 0 for one frame, then its life object is gone): read that as 0.
        std::optional<proto::ObjectLife> life = s.life;
        if (!life && o.breakable && o.life && *o.life > 0) {
            life = proto::ObjectLife{0, o.max};
        }
        if (life) {
            if (o.life && life->life < *o.life) {
                o.fight_until = now_us + kFightUs;
                std::int32_t drop = *o.life - life->life;
                if (o.hurt_left > 0) {
                    // Our own hurt landing (or the game taking more for it):
                    // not a hit of ours.
                    o.hurt_left = std::max(0, o.hurt_left - drop);
                    if (o.hurt_left == 0) {
                        if (life->life == o.hurt_target) {
                            ++hurt_taken_;
                        }
                        o.hurt_until = std::min(o.hurt_until, now_us);  // checked below
                    }
                    drop = 0;
                } else if (o.late_left > 0 && now_us < o.late_until) {
                    // The game took a hurt after its life had been written
                    // for it: a second fall for one hit. Put it back; it is
                    // no hit of ours.
                    const std::int32_t back = std::min(drop, o.late_left);
                    o.late_left -= back;
                    life->life += back;
                    drop -= back;
                    writes.push_back({o.address, life->life, 0, {}});
                    ++hurt_late_;
                }
                const auto owner = owners_.find(o.id);
                if (drop > 0 && blast_near_ && owner != owners_.end() &&
                    owner->second.player != 0 && blast_near_(s.position, now_us)) {
                    // A projectile copy went off here: the owner's own bomb
                    // makes this hit in its game.
                    ++blast_drops_;
                    if (owner->second.player == self) {
                        if (o.damage_debit > 0 && now_us < o.debit_until) {
                            // The thrower's DAMAGE came first: this is the
                            // same hit again. Put it back.
                            const std::int32_t back = std::min(o.damage_debit, drop);
                            o.damage_debit -= back;
                            drop -= back;
                            life->life += back;
                            writes.push_back({o.address, life->life, 0, {}});
                        }
                        if (drop > 0) {
                            if (now_us >= o.blast_until) {
                                o.blast_credit = 0;
                            }
                            o.blast_credit += drop;
                            o.blast_until = now_us + kBlastCreditUs;
                        }
                    }
                    drop = 0;
                }
                // Another game's, or nobody's yet (kept until the owner is
                // known: an enemy hit the moment it loaded, issue 133).
                const bool ours = owner != owners_.end() && owner->second.player == self;
                if (drop > 0 && !ours) {
                    if (o.hit_sum == 0) {
                        o.hit_since = now_us;
                    }
                    o.hit_sum += drop;
                    if (life->life <= 0 && o.killed_at == 0) {
                        o.killed_at = now_us;  // our hit took it: a resend may follow
                        o.resent = 0;
                    }
                    // Its own game is playing the hit reaction now.
                    o.react_at = now_us;
                    o.react_over = !o.received.empty() && o.received.back().animation
                                       ? o.received.back().animation->name
                                       : std::string{};
                }
            }
            o.life = life->life;
            o.max = life->max;
            if (life->life > 0) {
                o.killed_at = 0;  // alive again: a later kill is a new one
            }
        }
    }
    for (auto& [hash, taking] : taken_) {
        if (taking.ticks < kTakeBindTicks) {
            ++taking.ticks;  // actors loaded after this knew the key
        }
    }
    hidden_ = std::move(hidden);
    // Gone only when two walks in a row miss it: an actor drops out of the
    // list for a moment when it is lifted or changes hands.
    std::vector<Object> gone;
    for (auto it = objects_.begin(); it != objects_.end();) {
        if (now_seen.contains(it->first)) {
            it->second.missed = 0;
            ++it;
            continue;
        }
        if (++it->second.missed < kMissedWalks) {
            ++it;
            continue;
        }
        // Our rolled enemy is gone (killed, or the game took it away): the
        // others delete their copies.
        const Object& o = it->second;
        const auto owner = owners_.find(o.id);
        if (o.id.kind == proto::ObjectKind::made && o.id.maker == self && owner != owners_.end() &&
            owner->second.player == self) {
            proto::ObjectRemove r;
            r.id = o.id;
            r.epoch = owner->second.epoch;
            r.reason = o.life && *o.life <= 0 ? proto::RemoveReason::dead : proto::RemoveReason::gone;
            r.position = o.position;
            send.emplace_back(r);
            owners_.erase(owner);
        } else {
            // gohma-larva: an egg we own left alive. Hatched (a hatchling
            // already came out of it): the others end their copy now. Else
            // its hatchling may still come (eggs_).
            if (IsHatchingEgg(o.name) && o.id.kind == proto::ObjectKind::placed && owner != owners_.end() &&
                owner->second.player == self && !(o.life && *o.life <= 0) &&
                !(o.animation && IsDeathAnimation(o.animation->name))) {
                if (o.hatched) {
                    proto::ObjectRemove r;
                    r.id = o.id;
                    r.epoch = owner->second.epoch;
                    r.reason = proto::RemoveReason::gone;
                    r.position = o.position;
                    send.emplace_back(r);
                } else if (Object* sibling = HatchedSibling(self, o.position); sibling != nullptr) {
                    // gohma-sync (issue 178, review of gohma-larva): the
                    // larva came while this egg was still listed and was
                    // matched to a sibling egg near it, which is still here.
                    // This one is the egg that hatched: ended now, and the
                    // sibling may match its own larva later.
                    sibling->hatched = false;
                    proto::ObjectRemove r;
                    r.id = o.id;
                    r.epoch = owner->second.epoch;
                    r.reason = proto::RemoveReason::gone;
                    r.position = o.position;
                    send.emplace_back(r);
                    notes_.emplace_back("hatched here (its sibling egg was matched first): " + o.name);
                } else {
                    eggs_.push_back({o.id, owner->second.epoch, o.position, now_us});
                }
            }
            // A decided copy the director made left our walk. Dead: not made
            // again. Otherwise it went by itself (issue 124: a Stalchild copy
            // sank into the ground while the decider's one still fights); the
            // director makes it again, and the new one gets the create wait.
            for (auto& [key, d] : decided_) {
                if (d.address == o.address && d.id == o.id && !d.removed) {
                    d.dead = d.dead || (o.life && *o.life <= 0) ||
                             (o.animation && IsDeathAnimation(o.animation->name));
                    d.address = 0;
                    d.got_us = now_us;
                }
            }
            gone.push_back(std::move(it->second));
        }
        it = objects_.erase(it);
    }
    Flush(self, now_us, gone, send);
    // Off our walk: its owner is forgotten with it (see OBJECT_OWNER in
    // Apply). Another actor with the same placed id still here keeps it.
    for (const Object& o : gone) {
        if (o.id.kind == proto::ObjectKind::placed &&
            std::none_of(objects_.begin(), objects_.end(),
                         [&o](const auto& e) { return e.second.id == o.id; })) {
            owners_.erase(o.id);
        }
    }
    for (auto it = decided_.begin(); it != decided_.end();) {
        // A copy removed by its maker and no longer on our list is done.
        const bool left = it->second.address == 0 || !now_seen.contains(it->second.address);
        it = it->second.removed && left ? decided_.erase(it) : std::next(it);
    }
    decider_ = DeciderOf(self, now_us);
    std::erase_if(eggs_, [now_us](const Egg& e) { return now_us - e.at > kHatchWindowUs; });
    std::set<std::uint64_t> refused;
    std::sort(fresh.begin(), fresh.end(), [](const Seen* a, const Seen* b) {
        return std::tie(a->name, a->position[0], a->position[2]) <
               std::tie(b->name, b->position[0], b->position[2]);
    });
    for (const Seen* s : fresh) {
        Object o;
        o.address = s->address;
        o.position = s->position;
        o.rotation = s->rotation;
        o.animation = s->animation;
        o.slots = s->slots;
        o.seen_at = now_us;
        o.name = s->name;
        o.breakable = CoopWorld::IsBreakable(s->name);
        o.reacts = s->name.starts_with("Enemy_");
        o.fish = IsFish(s->name);
        o.guard = IsGuard(s->name);
        o.npc = s->name.starts_with("Npc_");
        o.first_position = s->position;
        if (s->animation) {
            o.rest_animation = s->animation->name;
        }
        if (s->life) {
            o.life = s->life->life;
            o.max = s->life->max;
        }
        if (const auto bound = bound_.find(s->address); bound != bound_.end()) {
            // A copy the director made of a decided or announced one.
            Decided& d = decided_.at(bound->second);
            if (d.removed) {
                continue;  // being deleted
            }
            d.address = s->address;
            o.id = d.id;
        } else if (s->hash != 0 && s->hash != kNoHash) {
            o.id = {proto::ObjectKind::placed, 0, s->hash};
            o.late = IsAnnounced(s->name) && now_us - scene_since_ >= kLateUs;
            // Our own one of an announced copy came: the copy goes.
            for (auto& [key, d] : decided_) {
                if (d.id == o.id && !d.removed) {
                    d.removed = true;
                    if (d.address != 0) {
                        objects_.erase(d.address);
                    }
                }
            }
        } else if (IsRolled(s->name)) {
            if (RollerFor(self, s->position, now_us) == self) {
                // We roll this scene's enemies: share it.
                o.id = {proto::ObjectKind::made, self, ++made_counter_};
                owners_[o.id] = {self, 0};
                proto::ObjectSpawn spawn;
                spawn.id = o.id;
                spawn.actor = o.name;
                spawn.state = StateOf(o, now_us);
                send.emplace_back(std::move(spawn));
            } else {
                // Another game rolls here. A copy being made near this
                // place may be this one, not yet on the director's list:
                // wait. Anything else is our own roll: deleted.
                const bool waiting = std::any_of(decided_.begin(), decided_.end(), [&](const auto& e) {
                    const Decided& d = e.second;
                    return d.address == 0 && !d.removed && now_us - d.got_us < kCreateWaitUs &&
                           Distance(d.position, s->position) < kClaimDistance;
                });
                if (!waiting) {
                    if (!refused_.contains(s->address)) {
                        ++refused_count_;
                    }
                    refused.insert(s->address);
                }
                continue;
            }
        } else if (IsHatchling(s->name) && Hatch(self, *s, send)) {
            // Out of an egg we own: shared as a made enemy.
            o.id = {proto::ObjectKind::made, self, ++made_counter_};
            owners_[o.id] = {self, 0};
            proto::ObjectSpawn spawn;
            spawn.id = o.id;
            spawn.actor = o.name;
            spawn.state = StateOf(o, now_us);
            send.emplace_back(std::move(spawn));
        } else if (IsHatchling(s->name) &&
                   std::any_of(decided_.begin(), decided_.end(), [&](const auto& e) {
                       const Decided& d = e.second;
                       return d.address == 0 && !d.removed && d.name == s->name &&
                              now_us - d.got_us < kCreateWaitUs &&
                              Distance(d.position, s->position) < kClaimDistance;
                   })) {
            continue;  // the copy of another game's hatchling, not yet bound
        } else {
            o.id = {proto::ObjectKind::placed, 0, SceneSpawnId(scene_, s->name, order_[s->name]++)};
        }
        objects_.emplace(s->address, o);
    }
    refused_ = std::move(refused);

    self_ = self;
    Apply(self, now_us, send, writes);
    // Copies their owner saw die that are still here after the grace.
    dead_.clear();
    for (const auto& [address, o] : objects_) {
        if (o.dead_at != 0 && (o.dead_now || now_us - o.dead_at >= kDeadGraceUs)) {
            dead_.insert(address);
        }
    }

    for (auto& [address, o] : objects_) {
        // A hurt the game did not take (or took differently) in time: the
        // life is written as the owner has it.
        if (o.hurt_until != 0 && now_us >= o.hurt_until) {
            if (o.life && *o.life != o.hurt_target) {
                writes.push_back({o.address, o.hurt_target, 0, {}});
                // The hit given to the game may still be taken (a pause, a
                // text box): watch for that fall for a while.
                o.late_left = std::max(0, *o.life - o.hurt_target);
                o.late_until = now_us + kHurtLateUs;
                o.life = o.hurt_target;
                ++hurt_missed_;
                // A killing hit written as life 0 on an enemy of ours: it
                // may stand on at 0 (issue 133, the big Skulltula hit from
                // its armoured side, a wall Skulltula hit by a seed).
                const auto own = owners_.find(o.id);
                if (o.hurt_target <= 0 && own != owners_.end() && own->second.player == self) {
                    DeadByWrite(o, own->second.epoch, now_us, send, "a hit our game did not take");
                }
            }
            o.hurt_left = 0;
            o.hurt_until = 0;
        }
        const auto owner = owners_.find(o.id);
        const bool known = owner != owners_.end() && owner->second.player != 0;
        // Our hits on a copy, summed over 100 ms, go to the owner.
        if (o.hit_sum > 0 && now_us - o.hit_since >= kDamageSumUs &&
            (known || now_us - o.hit_since >= kHitKeepUs)) {
            if (known && owner->second.player != self) {
                proto::Damage d;
                d.id = o.id;
                d.epoch = owner->second.epoch;
                d.amount = o.hit_sum;
                d.cause = static_cast<std::uint8_t>(proto::DamageCause::unknown);
                d.position = o.position;
                send.emplace_back(d);
                o.hit_at = now_us;
            }
            o.hit_sum = 0;
        }
        // Issue 150: our boss copy's own stun, as a DAMAGE of 0.
        if (o.stun_due) {
            o.stun_due = false;
            if (known && owner->second.player != self) {
                proto::Damage d;
                d.id = o.id;
                d.epoch = owner->second.epoch;
                d.amount = 0;
                d.cause = static_cast<std::uint8_t>(proto::DamageCause::unknown);
                d.position = o.position;
                send.emplace_back(d);
                o.stun_at = now_us;
                o.fight_until = now_us + kFightUs;
                notes_.emplace_back("stunned here, said to the owner: " + o.name);
            }
        }
        // A Gold Skulltula that came out of a tree here: name it for the
        // games that do not have it.
        if (known && owner->second.player == self && o.late && !o.announced &&
            o.id.kind == proto::ObjectKind::placed) {
            proto::ObjectSpawn spawn;
            spawn.id = o.id;
            spawn.actor = o.name;
            spawn.state = StateOf(o, now_us);
            send.emplace_back(std::move(spawn));
            o.announced = true;
        }
        // The owner's samples.
        if (known && owner->second.player == self &&
            (!o.sent_once || now_us - o.sent_at >= kSampleEveryUs)) {
            const std::optional<proto::ObjectLife> life =
                o.life ? std::optional(proto::ObjectLife{*o.life, o.max}) : std::nullopt;
            const std::optional<proto::Quat> rotation =
                o.rotation ? std::optional(QuatOf(*o.rotation)) : std::nullopt;
            const std::string animation = o.animation ? o.animation->name : std::string();
            std::string slots;  // issue 150: a boss's other slots' names
            for (const proto::SlotAnimation& s : o.slots) {
                slots += std::to_string(s.slot) + ":" + s.animation.name + ";";
            }
            const bool changed = life != o.sent_life || Moved(o.position, o.sent_position) ||
                                 Turned(rotation, o.sent_rotation) ||
                                 animation != o.sent_animation || slots != o.sent_slots;
            if (!o.sent_once || changed || now_us - o.sent_at >= kStillEveryUs) {
                proto::ObjectSample sample;
                sample.id = o.id;
                sample.epoch = owner->second.epoch;
                sample.state = StateOf(o, now_us);
                send.emplace_back(sample);
                o.sent_at = now_us;
                o.sent_once = true;
                o.sent_life = life;
                o.sent_position = o.position;
                o.sent_rotation = rotation;
                o.sent_animation = animation;
                o.sent_slots = std::move(slots);
            }
        }
    }

    std::vector<proto::ObjectPresence> present;
    present.reserve(objects_.size());
    std::vector<proto::FishLureEntry> fish;  // issue 151: FISH_LURE
    for (auto& [address, o] : objects_) {
        if (o.dead_said) {
            // Said dead (DeadByWrite): off our list, as the server takes it
            // (still listed would read as a respawn and undo the kill).
            continue;
        }
        std::uint8_t flags = proto::object_presence::awake;
        if (o.fight_until > now_us) {
            flags |= proto::object_presence::in_fight;
        }
        // An enemy of ours coming at our Link (NoteDrift): a fight to the
        // server, so it stays with us.
        // Issue 191: a guard too, but only for a reaction to our Link
        // (IsGuardReaction), never its patrol, and from further.
        if ((o.reacts || o.guard) && player_) {
            const float wake_m = o.reacts ? kWakeM : kGuardWakeM;
            const auto owner = owners_.find(o.id);
            if (owner != owners_.end() && owner->second.player == self) {
                proto::Vec3 to{};
                float d2 = 0.0f;
                for (std::size_t i = 0; i < 3; ++i) {
                    to[i] = (*player_)[i] - o.position[i];
                    d2 += to[i] * to[i];
                }
                const float d = std::sqrt(d2);
                float speed = 0.0f;
                if (d > 0.0f) {
                    for (std::size_t i = 0; i < 3; ++i) {
                        speed += o.velocity[i] * to[i] / d;
                    }
                }
                if (o.reacts && d <= kChaseReachM && speed >= kChaseSpeed) {
                    ChaseStep(o, now_us);
                }
                // Woken by our Link (issue 132): out of the animation it
                // rested in, near us. Keeps it ours.
                // Only for kWakeOwnUs after it woke, so one that patrols or
                // flies near us is not held in a fight for good.
                const std::string_view now_playing =
                    o.animation ? AnimationBase(o.animation->name) : std::string_view();
                const bool resting =
                    o.reacts ? now_playing.empty() || o.rest_animation.empty() ||
                                   now_playing == AnimationBase(o.rest_animation)
                             : !IsGuardReaction(now_playing);
                if (resting) {
                    o.woke_at = 0;
                } else if (o.woke_at == 0) {
                    o.woke_at = now_us;
                }
                if (wake_ && d <= wake_m && o.woke_at != 0 && now_us - o.woke_at < kWakeOwnUs) {
                    ChaseStep(o, now_us);
                }
            } else if (wake_ && owner != owners_.end() && owner->second.player != 0 &&
                       now_seen.contains(address) && Distance(o.position, *player_) <= wake_m &&
                       !o.received.empty() && now_us >= o.received.back().time &&
                       now_us - o.received.back().time <= kPoseStaleUs &&
                       !(o.react_at != 0 && now_us < o.react_at + kReactHoldUs)) {
                // A copy near our Link whose own AI plays something else than
                // the owner's enemy (issue 132): it wants to act on our Link.
                const auto& wanted = o.received.back().animation;
                const bool differs = o.animation && !o.animation->name.empty() && wanted &&
                                     !wanted->name.empty() &&
                                     AnimationBase(o.animation->name) != AnimationBase(wanted->name) &&
                                     (o.reacts || IsGuardReaction(o.animation->name));
                // Or the Mover had to put the owner's one back lately (issue 157).
                const bool put_back = o.took_over_at != 0 && now_us >= o.took_over_at &&
                                      now_us - o.took_over_at <= kWakeLatchUs;
                if (differs || put_back) {
                    ChaseStep(o, now_us);
                }
            }
        }
        // Issue 151 (fishing-3): how near our lure is to each fish. The
        // server gives the fish to the nearest lure's game (FISH_LURE); a
        // fish is never flagged targeting or fighting, so no fight lock
        // holds it in the wrong game.
        if (o.fish) {
            fish.push_back(FishClaim(o, self, now_us));
        }
        // Issue 200: our cutscene's (or talk's) townspeople are ours while it
        // runs, so the other games see them move as they do here.
        if (o.npc && in_event_ && player_ && Distance(o.position, *player_) <= kEventNpcM) {
            o.event_until = now_us + kEventNpcKeepUs;
            o.chase_until = std::max(o.chase_until, o.event_until);
        }
        if (o.chase_until > now_us && !o.fish) {
            flags |= proto::object_presence::targeting;
        }
        present.push_back({o.id, flags});
    }
    // gohma-larva: an egg of ours that just left the walk alive stays on the
    // list while its hatchling may still come, so the server keeps it ours
    // and takes its OBJECT_REMOVE (it hands an unlisted object over at once).
    for (const Egg& e : eggs_) {
        if (std::none_of(present.begin(), present.end(), [&e](const auto& p) { return p.id == e.id; })) {
            present.push_back({e.id, proto::object_presence::awake});  // unchanged from its last listing
        }
    }
    std::sort(present.begin(), present.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    if (present.size() > proto::kMaxPresentObjects) {
        present.resize(proto::kMaxPresentObjects);
    }
    bool present_went = false;
    if (!present_once_ || present != present_sent_ || now_us - present_at_ >= kPresentEveryUs) {
        proto::ObjectPresent p;
        p.scene = scene_;
        p.objects = present;
        send.emplace_back(std::move(p));
        present_sent_ = std::move(present);
        present_at_ = now_us;
        present_once_ = true;
        present_went = true;
    }

    // Groups (docs/objects.md, "Groups"): after the presence, so the server
    // knows every member. Sent on a change and again with every presence (a
    // repeat changes nothing there, and the server forgets a scene's objects
    // when nobody has them).
    std::vector<GroupInput> walk;
    walk.reserve(objects_.size());
    for (const auto& [address, o] : objects_) {
        walk.push_back({o.id, o.name, o.position});
    }
    std::vector<proto::ObjectGroupEntry> groups = GroupsOf(walk, group_links_);
    group_links_.clear();
    for (const proto::ObjectGroupEntry& e : groups) {
        if (e.member != e.root) group_links_[e.member] = e.root;
    }
    if (!groups.empty() && (groups != groups_sent_ || present_went)) {
        proto::ObjectGroup g;
        g.entries = groups;
        send.emplace_back(std::move(g));
    }
    groups_sent_ = std::move(groups);

    // Fishing (issue 151): after the presence, so the server knows the fish.
    // On a change (at most every kFishEveryUs, a hook or bite at once) and
    // again every kFishRepeatUs (the server takes a list older than 2.5 s as
    // no lure); an empty list once, when the last fish went.
    std::sort(fish.begin(), fish.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    if (fish.size() > proto::kMaxFishLures) {
        fish.resize(proto::kMaxFishLures);
    }
    auto flags_of = [](const std::vector<proto::FishLureEntry>& v) {
        std::vector<std::pair<proto::ObjectId, std::uint8_t>> out;
        for (const auto& e : v) out.emplace_back(e.id, e.flags);
        return out;
    };
    const bool changed = fish != fish_sent_;
    const bool urgent = changed && flags_of(fish) != flags_of(fish_sent_);
    const bool due = fish_at_ == 0 || now_us < fish_at_ || now_us - fish_at_ >= kFishEveryUs;
    const bool repeat = fish_at_ == 0 || now_us < fish_at_ || now_us - fish_at_ >= kFishRepeatUs;
    if ((!fish.empty() || !fish_sent_.empty()) &&
        ((changed && (urgent || due)) || (!fish.empty() && (present_went || repeat)))) {
        proto::FishLure m;
        m.fish = fish;
        send.emplace_back(std::move(m));
        fish_sent_ = std::move(fish);
        fish_at_ = now_us;
    }
}

proto::FishLureEntry Sync::FishClaim(const Object& o, std::uint8_t self,
                                     std::uint64_t now_us) const {
    proto::FishLureEntry e;
    e.id = o.id;
    std::optional<float> best;
    float speed = 0.0f;  // towards the nearest lure, metres a second
    for (const proto::Vec3& lure : lures_) {
        if (!Finite(lure)) {
            continue;
        }
        const float d = Distance(o.position, lure);
        if (best && d >= *best) {
            continue;
        }
        best = d;
        speed = 0.0f;
        if (d > 0.0f) {
            for (std::size_t i = 0; i < 3; ++i) {
                speed += o.velocity[i] * (lure[i] - o.position[i]) / d;
            }
        }
    }
    if (!best || !(*best <= kLureReachM)) {
        return e;  // no lure of ours in reach
    }
    // Rounded down to kFishStepCm, so a fish that hovers does not send a new
    // list each walk.
    const auto cm = static_cast<std::uint32_t>(*best * 100.0f);
    e.distance_cm = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(cm - cm % kFishStepCm, proto::kFishLureNone - 1));
    const auto owner = owners_.find(o.id);
    const bool ours = owner != owners_.end() && owner->second.player == self;
    if (ours && *best <= kLureHookM) {
        e.flags |= proto::fish_lure::hooked;
    } else if (ours ? speed >= kLureSpeed : o.chase_until > now_us) {
        // Ours coming at our lure; a copy whose own AI swims at it (NoteDrift).
        e.flags |= proto::fish_lure::drawn;
    }
    return e;
}

// Objects that left our walk. A break read just before it went (a pot's life
// 0, then the pot is removed a few frames later) still reaches the others:
// the owner's last life 0 as a sample, a copy's unsent hit as DAMAGE. A
// breakable gone near our Link was broken here even when its life never
// read 0 (a thrown rock is removed where it lands, a crate a moment after its
// life reaches 0): within kBreakNearM when it was moved (lifted, thrown),
// within kBreakReachM (sword reach) when not. The same, with all its life.
void Sync::Flush(std::uint8_t self, std::uint64_t now_us, std::vector<Object>& gone,
                 std::vector<proto::Message>& send) {
    for (Object& o : gone) {
        const auto owner = owners_.find(o.id);
        if (owner == owners_.end() || owner->second.player == 0) {
            continue;
        }
        bool broken_here = false;
        if (o.breakable && player_ && (!o.life || *o.life > 0)) {
            const float dx = o.position[0] - (*player_)[0];
            const float dy = o.position[1] - (*player_)[1];
            const float dz = o.position[2] - (*player_)[2];
            const float mx = o.position[0] - o.first_position[0];
            const float my = o.position[1] - o.first_position[1];
            const float mz = o.position[2] - o.first_position[2];
            const bool moved = std::sqrt(mx * mx + my * my + mz * mz) >= 0.5f;
            const float from_link = std::sqrt(dx * dx + dy * dy + dz * dz);
            broken_here = from_link <= (moved ? kBreakNearM : kBreakReachM);
        }
        {
            float near = -1.0f;
            if (player_) {
                const float dx = o.position[0] - (*player_)[0];
                const float dy = o.position[1] - (*player_)[1];
                const float dz = o.position[2] - (*player_)[2];
                near = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            const float mx = o.position[0] - o.first_position[0];
            const float my = o.position[1] - o.first_position[1];
            const float mz = o.position[2] - o.first_position[2];
            char line[260];
            std::snprintf(line, sizeof line,
                          "gone %016llx %s owner %u life %d anim '%s' moved %.2f from Link %.2f: %s",
                          static_cast<unsigned long long>(o.id.value), o.name.c_str(),
                          owner->second.player, o.life.value_or(-1),
                          o.animation ? o.animation->name.c_str() : "",
                          std::sqrt(mx * mx + my * my + mz * mz), near,
                          broken_here ? "broken here" : "left");
            notes_.emplace_back(line);
        }
        if (owner->second.player == self) {
            if (broken_here) {
                o.life = 0;
            }
            if (o.life && *o.life <= 0 && (!o.sent_life || o.sent_life->life > 0)) {
                proto::ObjectSample sample;
                sample.id = o.id;
                sample.epoch = owner->second.epoch;
                sample.state = StateOf(o, now_us);
                send.emplace_back(sample);
            }
            // An enemy of ours that died here: dead for everyone (issue 133).
            // Its life 0 alone does not reach a copy whose life cannot be
            // read (the big Skulltula), nor end a copy whose game did not
            // take the life we wrote. Rolled ones say it in Tick already.
            const bool died = !o.dead_said && ((o.life && *o.life <= 0) ||
                                               (o.animation && IsDeathAnimation(o.animation->name)));
            // A fish of ours gone in our Link's hands: caught here (issue
            // 151), gone for everyone.
            const bool caught = o.fish && player_ && Distance(o.position, *player_) <= kCatchNearM;
            if (caught) {
                notes_.emplace_back("caught here: " + o.name);
            }
            if (((o.reacts && died) || caught) && o.id.kind == proto::ObjectKind::placed) {
                proto::ObjectRemove r;
                r.id = o.id;
                r.epoch = owner->second.epoch;
                r.reason = proto::RemoveReason::dead;
                r.position = o.position;
                send.emplace_back(r);
                notes_.emplace_back("dead, said to the others: " + o.name);
            }
        } else if (o.hit_sum > 0 || broken_here) {
            proto::Damage d;
            d.id = o.id;
            d.epoch = owner->second.epoch;
            d.amount = broken_here ? std::max(o.life.value_or(1), o.hit_sum) : o.hit_sum;
            d.cause = static_cast<std::uint8_t>(proto::DamageCause::unknown);
            d.position = o.position;
            send.emplace_back(d);
        }
    }
}

void Sync::Apply(std::uint8_t self, std::uint64_t now_us, std::vector<proto::Message>& send,
                 std::vector<LifeWrite>& writes) {
    std::vector<proto::Message> inbox;
    bool forget = false;
    {
        std::lock_guard lock(inbox_mutex_);
        inbox.swap(inbox_);
        forget = std::exchange(forget_owners_, false);
    }
    if (forget) {
        std::erase_if(owners_, [](const auto& e) { return e.first.kind == proto::ObjectKind::placed; });
    }
    const auto find = [this](const proto::ObjectId& id) -> Object* {
        for (auto& [address, o] : objects_) {
            if (o.id == id) {
                return &o;
            }
        }
        return nullptr;
    };
    const auto write = [&writes](Object& o, std::int32_t life) {
        writes.push_back({o.address, life, 0, {}});
        o.life = life;
        o.hurt_left = 0;
        o.hurt_until = 0;
    };
    for (const proto::Message& m : inbox) {
        if (const auto* spawn = std::get_if<proto::ObjectSpawn>(&m)) {
            // A rolled enemy the decider made, or a placed one its owner
            // announced: created here by the director.
            const bool made = spawn->id.kind == proto::ObjectKind::made;
            if (!spawn->state.position || !Finite(*spawn->state.position)) {
                continue;
            }
            if (made && (spawn->id.maker == self || spawn->id.maker == 0 ||
                         !(IsRolled(spawn->actor) || IsHatchling(spawn->actor)))) {
                continue;
            }
            if (!made && (!IsAnnounced(spawn->actor) || find(spawn->id) != nullptr ||
                          spawn->player_number == self)) {
                continue;  // we have that placement ourselves
            }
            const bool known = std::any_of(decided_.begin(), decided_.end(),
                                           [&](const auto& e) { return e.second.id == spawn->id; });
            if (known) {
                continue;
            }
            Decided d;
            d.id = spawn->id;
            d.name = spawn->actor;
            d.position = *spawn->state.position;
            d.got_us = now_us;
            decided_.emplace(next_key_++, std::move(d));
            if (made) {
                owners_[spawn->id] = {spawn->id.maker, 0};
            } else if (!owners_.contains(spawn->id)) {
                owners_[spawn->id] = {spawn->player_number, 0};  // OBJECT_OWNER corrects the epoch
            }
        } else if (const auto* remove = std::get_if<proto::ObjectRemove>(&m)) {
            for (auto& [key, d] : decided_) {
                if (d.id == remove->id) {
                    d.removed = true;
                }
            }
            // A placed enemy its owner saw die (issue 133): ours dies too.
            // Its life goes to 0 through the game (it plays its own death);
            // still here kDeadGraceUs later, it is deleted.
            const auto had = owners_.find(remove->id);
            // gohma-larva: an egg that hatched in its owner's game. Ours is
            // held there and cannot hatch: it goes, and the hatchling comes
            // as the owner's OBJECT_SPAWN.
            if (remove->reason == proto::RemoveReason::gone && remove->id.kind == proto::ObjectKind::placed &&
                (had == owners_.end() || remove->epoch >= had->second.epoch)) {
                Object* o = find(remove->id);
                if (o != nullptr && IsHatchingEgg(o->name) && o->dead_at == 0 &&
                    !(had != owners_.end() && had->second.player == self)) {
                    o->dead_now = true;
                    o->dead_at = now_us;
                    o->hit_sum = 0;
                    notes_.emplace_back("hatched in its owner's game: " + o->name);
                    if (had != owners_.end()) {
                        owners_.erase(had);
                    }
                }
            }
            if (remove->reason == proto::RemoveReason::dead &&
                remove->id.kind == proto::ObjectKind::placed &&
                (had == owners_.end() || remove->epoch >= had->second.epoch)) {
                Object* o = find(remove->id);
                if (o != nullptr && o->fish && o->dead_at == 0) {
                    // Issue 151: caught in its owner's game. Ours goes now.
                    o->dead_now = true;
                    o->dead_at = now_us;
                    notes_.emplace_back("caught in its owner's game: " + o->name);
                } else if (o != nullptr && o->reacts && o->dead_at == 0) {
                    // Our game kills it itself (life 0 through the game), so
                    // its own kill counts: a room's enemy counter and shutter,
                    // the kill it keeps for a room reload. The delete after
                    // kDeadGraceUs is the fallback. A kill from before we
                    // came (no owner was ever named to us: a late entrant or
                    // a rejoin, issue 133) whose life we cannot read goes at
                    // once.
                    if (o->life && *o->life > 0) {
                        Lower(*o, 0, now_us, writes, remove->player_number);
                    } else if (had == owners_.end() && !o->life) {
                        o->dead_now = true;
                    }
                    o->dead_at = now_us;
                    notes_.emplace_back(had == owners_.end() ? "killed before we came: " + o->name
                                                             : "dead in its owner's game: " + o->name);
                    o->hit_sum = 0;
                }
                // Nobody's now: no samples, poses or plays for it.
                if (had != owners_.end()) {
                    owners_.erase(had);
                }
            }
        } else if (const auto* owner = std::get_if<proto::ObjectOwner>(&m)) {
            // A placed object's owner counts only while it is on our walk.
            // The server forgets an object nobody has loaded and starts its
            // epoch at 0 again; an owner kept from before would make every
            // later one look old (issue 133: in the Lixyas test each game
            // kept the other as owner of the Deku Tree's enemies for minutes,
            // so neither sent their state and the hits were dropped).
            if (owner->id.kind == proto::ObjectKind::placed && find(owner->id) == nullptr) {
                continue;
            }
            const auto had = owners_.find(owner->id);
            const std::uint8_t before = had != owners_.end() ? had->second.player : 0;
            if (had != owners_.end() && owner->epoch < had->second.epoch) {
                continue;  // an older change, late
            }
            owners_[owner->id] = {owner->owner, owner->epoch};
            Object* o = owner->owner != before ? find(owner->id) : nullptr;
            if (o == nullptr) {
                continue;
            }
            // Another owner: its samples alone place the enemy from now on,
            // none of the old owner's are blended in.
            o->received.clear();
            if (owner->owner == self) {
                // Ours now, where it stands here: nothing pulls it. A hit of
                // ours not yet sent is already in its life, and the others
                // get its state at once.
                o->hit_sum = 0;
                o->hit_at = 0;
                o->react_at = 0;
                o->sent_once = false;
                if (before != 0 && (o->reacts || o->guard) && ours_now_.size() < 64) {
                    ours_now_.push_back(o->address);  // was a copy: its hold ends
                }
            } else if (before == self) {
                // No longer ours: a hurt we were giving it as the owner is
                // the new owner's to give.
                o->late_left = 0;
            }
        } else if (const auto* sample = std::get_if<proto::ObjectSample>(&m)) {
            Object* o = find(sample->id);
            const auto own = owners_.find(sample->id);
            if (o == nullptr || (own != owners_.end() && own->second.player == self)) {
                continue;
            }
            if (own != owners_.end() && sample->epoch < own->second.epoch) {
                continue;  // from an owner before the last change
            }
            if (sample->state.position && Finite(*sample->state.position)) {
                const std::uint64_t t = sample->state.time.server_time_us;
                if (o->received.empty() || t > o->received.back().time) {
                    o->received.push_back({t, *sample->state.position, sample->state.rotation,
                                           sample->state.velocity, sample->state.animation,
                                           sample->state.slots.value_or(
                                               std::vector<proto::SlotAnimation>{})});
                    while (o->received.size() > kKeepSamples) {
                        o->received.pop_front();
                    }
                    if (const auto& a = o->received.back().animation; o->npc && a) {
                        o->npc_loop.See(a->name, a->frame);  // coop_npc_anim.h
                    }
                }
            }
            if (!sample->state.life || !o->life) {
                continue;
            }
            const std::int32_t life = sample->state.life->life;
            const std::int32_t have = o->hurt_until != 0 ? o->hurt_target : *o->life;
            const bool holding = o->hit_sum > 0 || now_us - o->hit_at < kHitHoldUs;
            if (life < have) {
                // Who hit it is not sent: its owner is the best guess.
                Lower(*o, life, now_us, writes, sample->player_number);
                o->fight_until = now_us + kFightUs;
            } else if (life > have && !holding && have <= 0 && o->killed_at != 0 &&
                       now_us - o->killed_at < kKilledKeepUs && o->dead_at == 0 &&
                       o->resent < kResendMax) {
                // We killed it here and the owner's enemy still lives (the
                // DAMAGE was lost, or crossed a change of owner): not healed
                // back, the hit is sent again (issue 133). After kResendMax
                // the owner's life is taken, as below.
                ++o->resent;
                o->hit_sum = life;
                o->hit_since = now_us > kDamageSumUs ? now_us - kDamageSumUs : 0;
                notes_.emplace_back("killed here, owner still shows life: hit sent again: " +
                                    o->name);
            } else if (life > have && !holding && o->dead_at == 0) {
                write(*o, life);
                o->fight_until = now_us + kFightUs;
            }
        } else if (const auto* damage = std::get_if<proto::Damage>(&m)) {
            // The server sends DAMAGE only to the owner: that is us.
            Object* o = find(damage->id);
            if (o == nullptr || !o->life || damage->amount <= 0) {
                continue;
            }
            if (damage->player_number == self) {
                continue;  // our own hit, sent before it became ours: already in its life
            }
            std::int32_t amount = damage->amount;
            if (o->blast_credit > 0 && now_us < o->blast_until) {
                // A copy bomb here took this hit already.
                const std::int32_t taken = std::min(o->blast_credit, amount);
                o->blast_credit -= taken;
                amount -= taken;
                if (amount <= 0) {
                    continue;
                }
            }
            if (now_us >= o->debit_until) {
                o->damage_debit = 0;
            }
            o->damage_debit += amount;
            o->debit_until = now_us + kBlastCreditUs;
            const std::int32_t have = o->hurt_until != 0 ? o->hurt_target : *o->life;
            Lower(*o, std::max(0, have - amount), now_us, writes, damage->player_number);
            o->fight_until = now_us + kFightUs;
            if (!hurt_ && have - amount <= 0) {
                // Hurt off: the life was written, not given as a hit.
                const auto own = owners_.find(o->id);
                DeadByWrite(*o, own != owners_.end() ? own->second.epoch : 0, now_us, send,
                            "life written to 0");
            }
        } else if (const auto* ask = std::get_if<proto::HandoverAsk>(&m)) {
            proto::HandoverState state;
            state.id = ask->id;
            state.epoch = ask->epoch;
            if (Object* o = find(ask->id)) {
                state.state = StateOf(*o, now_us);
            } else {
                state.state.time = {now_us, 0};
            }
            send.emplace_back(std::move(state));
        } else if (const auto* handed = std::get_if<proto::HandoverState>(&m)) {
            Object* o = find(handed->id);
            // The lower life of the two, so an enemy never heals across a
            // hand-over. Its place stays our own enemy's.
            if (o != nullptr && handed->state.life && o->life &&
                handed->state.life->life < *o->life) {
                write(*o, handed->state.life->life);
                if (handed->state.life->life <= 0) {
                    // Killed in the old owner's game as it came to us.
                    DeadByWrite(*o, handed->epoch, now_us, send, "dead in the hand-over");
                }
            }
        }
    }
}

void Sync::DeadByWrite(Object& o, std::uint16_t epoch, std::uint64_t now_us,
                       std::vector<proto::Message>& send, const char* why) {
    if (!o.reacts || o.dead_at != 0 || o.dead_said) {
        return;
    }
    o.dead_at = now_us;  // deleted kDeadGraceUs later if still here (Removes)
    if (o.id.kind == proto::ObjectKind::placed) {
        proto::ObjectRemove r;
        r.id = o.id;
        r.epoch = epoch;
        r.reason = proto::RemoveReason::dead;
        r.position = o.position;
        send.emplace_back(r);
        o.dead_said = true;
    }
    notes_.emplace_back(std::string("dead by ") + why + ", said to the others: " + o.name);
}

void Sync::Lower(Object& o, std::int32_t life, std::uint64_t now_us,
                 std::vector<LifeWrite>& writes, std::uint8_t hitter) {
    if (!o.life) {
        return;
    }
    // Only enemies take a hit through the game (they flinch, lose a head, are
    // knocked back); a pot, a crate or an animal gets its life written.
    if (!hurt_ || !o.reacts) {
        writes.push_back({o.address, life, 0, {}});
        o.life = life;
        return;
    }
    const std::int32_t from = o.hurt_until != 0 ? o.hurt_target : *o.life;
    if (life >= from) {
        return;
    }
    // The game takes it off itself; the walk sees the drop and does not
    // count it as our hit. o.life stays as read until then.
    LifeWrite w{o.address, life, from - life, std::nullopt};
    {
        std::lock_guard lock(inbox_mutex_);
        const auto place = places_.find(hitter);
        const std::uint64_t at = place != places_.end() ? place->second.second : 0;
        if (place != places_.end() &&
            (at <= now_us ? now_us - at : at - now_us) < kPlaceStaleUs) {
            w.from = place->second.first;
        }
    }
    writes.push_back(w);
    o.hurt_left += from - life;
    o.hurt_target = life;
    o.hurt_until = now_us + kHurtWaitUs;
}

void Sync::Poses(std::uint64_t now_us, std::vector<PoseWrite>& out) const {
    const std::uint64_t t = now_us > kBehindUs ? now_us - kBehindUs : 0;
    for (const auto& [address, o] : objects_) {
        if (o.received.empty() || now_us < o.received.back().time ||
            now_us - o.received.back().time > kPoseStaleUs) {
            continue;
        }
        const auto owner = owners_.find(o.id);
        if (owner == owners_.end() || owner->second.player == 0 || owner->second.player == self_) {
            continue;
        }
        PoseWrite w;
        w.address = address;
        const Received& last = o.received.back();
        std::optional<proto::Quat> rotation;
        if (t >= last.time) {
            const float ahead =
                static_cast<float>(std::min(t - last.time, kExtrapolateUs)) / 1e6f;
            w.position = last.position;
            if (last.velocity && Finite(*last.velocity)) {
                for (std::size_t i = 0; i < 3; ++i) {
                    w.position[i] += (*last.velocity)[i] * ahead;
                }
            }
            rotation = last.rotation;
        } else if (t <= o.received.front().time) {
            w.position = o.received.front().position;
            rotation = o.received.front().rotation;
        } else {
            std::size_t i = 1;
            while (o.received[i].time <= t) {
                ++i;
            }
            const Received& a = o.received[i - 1];
            const Received& b = o.received[i];
            const float f = static_cast<float>(t - a.time) / static_cast<float>(b.time - a.time);
            w.position = Lerp(a.position, b.position, f);
            if (a.rotation && b.rotation) {
                rotation = Nlerp(*a.rotation, *b.rotation, f);
            } else {
                rotation = b.rotation ? b.rotation : a.rotation;
            }
        }
        if (rotation) {
            w.rotation = MatOf(*rotation);
        }
        out.push_back(w);
    }
}

std::vector<std::uint64_t> Sync::Holds(std::uint64_t now_us) const {
    std::vector<PoseWrite> placed;
    Poses(now_us, placed);
    std::vector<std::uint64_t> out;
    for (const PoseWrite& w : placed) {
        const auto o = objects_.find(w.address);
        if (o != objects_.end() && o->second.reacts) {
            out.push_back(w.address);
        }
    }
    return out;
}

float FrameAfter(float frame, float rate, std::int64_t age_us) {
    if (!std::isfinite(frame) || !std::isfinite(rate)) {
        return 0.0f;
    }
    const float after = frame + rate * 30.0f * static_cast<float>(age_us) / 1e6f;
    return std::isfinite(after) && after > 0.0f ? after : 0.0f;
}

void Sync::Plays(std::uint64_t now_us, std::vector<PlayWish>& out) const {
    for (const auto& [address, since] : chest_shown_) {
        // Issue 195: a chest opened elsewhere shows its open idle; played
        // again only over its closed idle (sample 1: its frame is set once).
        out.push_back({address, kChestOpenIdle, 1.0f, 0.0f, 1, 0, kChestClosedIdle});
    }
    for (const auto& [address, o] : objects_) {
        if (o.received.empty() || now_us < o.received.back().time ||
            now_us - o.received.back().time > kPoseStaleUs) {
            continue;
        }
        const auto owner = owners_.find(o.id);
        if (owner == owners_.end() || owner->second.player == 0 || owner->second.player == self_) {
            continue;
        }
        const Received& last = o.received.back();
        const auto& wanted = last.animation;
        if (!wanted || wanted->name.empty()) {
            continue;
        }
        // Just hit here: the copy's own game shows the hit. The owner's
        // animation from before the hit waits; its next one (its own
        // reaction to our DAMAGE) is played as usual.
        if (o.react_at != 0 && now_us < o.react_at + kReactHoldUs &&
            wanted->name == o.react_over) {
            continue;
        }
        const float rate = std::isfinite(wanted->rate) && wanted->rate > 0.0f ? wanted->rate : 1.0f;
        // Shown kBehindUs late, as the pose; no further ahead than a pose is.
        const std::int64_t age = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(now_us - last.time) - static_cast<std::int64_t>(kBehindUs),
            -static_cast<std::int64_t>(kBehindUs), static_cast<std::int64_t>(kExtrapolateUs));
        out.push_back({address, wanted->name, rate, FrameAfter(wanted->frame, rate, age),
                       last.time, 0, {}});
        if (o.npc) {
            // Walking townspeople (coop_npc_anim.h): the playing frame, run
            // on as fast as the owner's last two samples show it running.
            PlayWish& wish = out.back();
            wish.npc = true;
            wish.alias = !o.guard;  // a guard's own reaction is a hand-over (issue 191)
            wish.loop = o.npc_loop.Length();
            wish.run = rate;
            if (o.received.size() >= 2) {
                const Received& before = o.received[o.received.size() - 2];
                if (before.animation && before.animation->name == wanted->name) {
                    wish.run = CoopNpcAnim::SeenRate(before.animation->frame, before.time,
                                                     wanted->frame, last.time, rate);
                }
            }
            wish.frame =
                CoopNpcAnim::Wrapped(FrameAfter(wanted->frame, wish.run, age), wish.loop);
        }
        // Issue 150: a boss's other slots (Gohma's eye guard and eyelids,
        // whose AS events open and close the eye's guard). Without them
        // each game's copy kept its own AI's guard: the eye stood open for
        // one player and guarded for the other.
        for (const proto::SlotAnimation& s : last.slots) {
            if (s.animation.name.empty()) {
                continue;
            }
            const float r = std::isfinite(s.animation.rate) && s.animation.rate > 0.0f
                                ? s.animation.rate
                                : 1.0f;
            out.push_back({address, s.animation.name, r, FrameAfter(s.animation.frame, r, age),
                           last.time, s.slot, {}});
        }
    }
}

void Sync::ChestPlayHeld(std::uint64_t address, std::uint64_t now_us) {
    if (const auto it = chest_shown_.find(address); it != chest_shown_.end() && !it->second.took) {
        it->second.since_us = now_us;
    }
}

std::vector<std::uint64_t> Sync::Removes() const {
    std::vector<std::uint64_t> out(hidden_.begin(), hidden_.end());
    out.insert(out.end(), refused_.begin(), refused_.end());
    out.insert(out.end(), dead_.begin(), dead_.end());
    return out;
}

} // namespace CoopObjects
