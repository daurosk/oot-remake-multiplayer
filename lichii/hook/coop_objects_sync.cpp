// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_objects_sync.h"
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

bool IsShared(std::string_view name) {
    if (name.starts_with("Npc_")) {
        // Each player's own: Navi (the fairy sync has her), the equipment
        // a player or NPC holds, and invisible helpers.
        return !name.starts_with("Npc_Navi") && !name.starts_with("Npc_Equipment") &&
               !name.starts_with("Npc_Invisible");
    }
    return name.starts_with("Enemy_") || name.starts_with("Animal_") ||
           CoopWorld::IsBreakable(name);
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

bool IsAnnounced(std::string_view name) {
    return name == "Enemy_Skulltula_Gold";
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
                   std::is_same_v<T, proto::Damage> || std::is_same_v<T, proto::FlagSet> ||
                   std::is_same_v<T, proto::FlagState> || std::is_same_v<T, proto::FlagSnapshot> ||
                   std::is_same_v<T, proto::ObjectSpawn> || std::is_same_v<T, proto::ObjectRemove>;
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

void Sync::Reset() {
    scene_.clear();
    objects_.clear();
    hidden_.clear();
    dead_.clear();
    order_.clear();
    owners_.clear();
    present_sent_.clear();
    present_at_ = 0;
    present_once_ = false;
    groups_sent_.clear();
    group_links_.clear();
    decided_.clear();
    bound_.clear();
    refused_.clear();
    decider_ = 0;
    // next_key_ and made_counter_ go on: the director never makes a key
    // twice, and the server keeps made ids until they are removed.
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
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
        out.push_back({key, d.name, d.position, d.removed});
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

void Sync::NoteDrift(std::uint64_t address, const proto::Vec3& put, const proto::Vec3& now,
                     std::uint64_t now_us) {
    if (!player_ || !Finite(put) || !Finite(now)) {
        return;
    }
    const auto it = objects_.find(address);
    if (it == objects_.end() || !it->second.reacts) {
        return;
    }
    Object& o = it->second;
    const auto owner = owners_.find(o.id);
    if (owner == owners_.end() || owner->second.player == 0 || owner->second.player == self_) {
        return;  // ours, or nobody's: the Mover does not place it
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
    for (const Seen& s : seen) {
        if (s.hash != 0 && s.hash != kNoHash && taken_.contains(s.hash)) {
            hidden.insert(s.address);
            continue;  // gone for everyone: not shared, not sent
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
        o.animation = s.animation;
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
                    // Its own game is playing the hit reaction now.
                    o.react_at = now_us;
                    o.react_over = !o.received.empty() && o.received.back().animation
                                       ? o.received.back().animation->name
                                       : std::string{};
                }
            }
            o.life = life->life;
            o.max = life->max;
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
        o.seen_at = now_us;
        o.name = s->name;
        o.breakable = CoopWorld::IsBreakable(s->name);
        o.reacts = s->name.starts_with("Enemy_");
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
            if (decider_ == self) {
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
        if (o.dead_at != 0 && now_us - o.dead_at >= kDeadGraceUs) {
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
            const bool changed = life != o.sent_life || Moved(o.position, o.sent_position) ||
                                 Turned(rotation, o.sent_rotation) ||
                                 animation != o.sent_animation;
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
            }
        }
    }

    std::vector<proto::ObjectPresence> present;
    present.reserve(objects_.size());
    for (auto& [address, o] : objects_) {
        std::uint8_t flags = proto::object_presence::awake;
        if (o.fight_until > now_us) {
            flags |= proto::object_presence::in_fight;
        }
        // An enemy of ours coming at our Link (NoteDrift): a fight to the
        // server, so it stays with us.
        if (o.reacts && player_) {
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
                if (d <= kChaseReachM && speed >= kChaseSpeed) {
                    ChaseStep(o, now_us);
                }
                // Woken by our Link (issue 132): out of the animation it
                // rested in, near us. Keeps it ours.
                const std::string now_playing = o.animation ? o.animation->name : std::string();
                if (wake_ && d <= kWakeM && !now_playing.empty() && !o.rest_animation.empty() &&
                    now_playing != o.rest_animation) {
                    ChaseStep(o, now_us);
                }
            } else if (wake_ && owner != owners_.end() && owner->second.player != 0 &&
                       now_seen.contains(address) && Distance(o.position, *player_) <= kWakeM &&
                       !o.received.empty() && now_us >= o.received.back().time &&
                       now_us - o.received.back().time <= kPoseStaleUs &&
                       !(o.react_at != 0 && now_us < o.react_at + kReactHoldUs)) {
                // A copy near our Link whose own AI plays something else than
                // the owner's enemy (issue 132): it wants to act on our Link.
                const auto& wanted = o.received.back().animation;
                if (o.animation && !o.animation->name.empty() && wanted &&
                    !wanted->name.empty() && o.animation->name != wanted->name) {
                    ChaseStep(o, now_us);
                }
            }
        }
        if (o.chase_until > now_us) {
            flags |= proto::object_presence::targeting;
        }
        present.push_back({o.id, flags});
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
            const bool died = (o.life && *o.life <= 0) ||
                              (o.animation && IsDeathAnimation(o.animation->name));
            if (o.reacts && died && o.id.kind == proto::ObjectKind::placed) {
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
    const auto flags = [this](const std::vector<proto::FlagEntry>& entries) {
        for (const proto::FlagEntry& f : entries) {
            if (f.key.kind == proto::FlagKeyKind::key64 && f.kind == proto::FlagKind::boolean &&
                f.value == 1) {
                taken_.insert(f.key.key);
            }
        }
    };
    for (const proto::Message& m : inbox) {
        if (const auto* set = std::get_if<proto::FlagSet>(&m)) {
            flags(set->flags);
        } else if (const auto* state = std::get_if<proto::FlagState>(&m)) {
            flags(state->flags);
        } else if (const auto* snapshot = std::get_if<proto::FlagSnapshot>(&m)) {
            flags(snapshot->flags);
        } else if (const auto* spawn = std::get_if<proto::ObjectSpawn>(&m)) {
            // A rolled enemy the decider made, or a placed one its owner
            // announced: created here by the director.
            const bool made = spawn->id.kind == proto::ObjectKind::made;
            if (!spawn->state.position || !Finite(*spawn->state.position)) {
                continue;
            }
            if (made && (spawn->id.maker == self || spawn->id.maker == 0 || !IsRolled(spawn->actor))) {
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
            if (remove->reason == proto::RemoveReason::dead &&
                remove->id.kind == proto::ObjectKind::placed &&
                (had == owners_.end() || remove->epoch >= had->second.epoch)) {
                if (Object* o = find(remove->id); o != nullptr && o->reacts && o->dead_at == 0) {
                    if (o->life && *o->life > 0) {
                        Lower(*o, 0, now_us, writes, remove->player_number);
                    }
                    o->dead_at = now_us;
                    o->hit_sum = 0;
                    notes_.emplace_back("dead in its owner's game: " + o->name);
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
                                           sample->state.velocity, sample->state.animation});
                    while (o->received.size() > kKeepSamples) {
                        o->received.pop_front();
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
            } else if (life > have && !holding && have <= 0 && o->hit_at != 0 &&
                       o->dead_at == 0 && o->resent < kResendMax) {
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
            }
        }
    }
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
                       last.time});
    }
}

std::vector<std::uint64_t> Sync::Removes() const {
    std::vector<std::uint64_t> out(hidden_.begin(), hidden_.end());
    out.insert(out.end(), refused_.begin(), refused_.end());
    out.insert(out.end(), dead_.begin(), dead_.end());
    return out;
}

} // namespace CoopObjects
