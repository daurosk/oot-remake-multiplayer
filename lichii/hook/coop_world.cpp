// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "coop/host/game_items.hpp"
#include "coop/protocol/codec.hpp"
#include "coop/protocol/messages.hpp"

namespace CoopWorld {

namespace {

// Every Obj_ pack of build e0f3ccfaf50065ae that has LifeRef and
// DestructionRef (read from the romfs, 2026-11-09), by name part. Left out:
// the chest Obj_TBox_A_04 (opening a chest is a flag, flags.md), shop
// targets and moving obstacles (minigames), icicles and hammer pillars
// (they fall or tilt rather than break), Obj_Well_CandleStick_A_03.
constexpr std::array<std::string_view, 6> kPrefixes{
    "Obj_Pot_",          // pots, the respawning and dream ones too
    "Obj_BrokenBox_",    // crates
    "Obj_LiftRock_",     // rocks Link lifts and throws
    "Obj_BrokenRock_",   // rocks a bomb or hammer breaks
    "Obj_Plant_Grass_",  // grass
    "Obj_RedIce_",       // red ice (melted with blue fire: it goes, no life)
};
constexpr std::array<std::string_view, 11> kParts{
    "SignBoard_",  "Signboard_",  "GuidePost_",  // cuttable signs, every region's
    "CrackWall_",  "CrackFloor_",     // bombable walls and floors
    "BrokenRock_",                    // Goron and Death Mountain rocks
    "BrokenIcePillar_", "SkullPot_", "ArcheryPot_", "Beehive_", "SpiderWeb_",
};

// The grass that is an actor without an Obj_Plant_Grass_ name: every pack
// with the life of Obj_Plant_DestructibleGrass_Base (a pot's: 1, then gone).
// It has no DestructionRef, so the list above missed it; the 13 tufts by
// the Kokiri side of Hyrule Field are the third. The rest of the field's
// grass is foliage, no actor at all, and is not shared (world.md).
constexpr std::array<std::string_view, 4> kGrass{
    "Obj_Plant_DestructibleGrass_Base",
    "Land_Kokiri_Plant_G_01",
    "Land_kokiri_Plant_A_02_DropFixed_03",
    "Land_Kokiri_Zenmai_A_02",
};

} // namespace

bool IsLock(std::string_view name) {
    return name.starts_with("Obj_") && name.find("DoorChainLock") != std::string_view::npos &&
           name.find("HrlCity") == std::string_view::npos;
}

std::uint32_t SmallKeyFlag(std::string_view scene) {
    // The dungeons with a DungeonInfo.<name>.SmallKeyNum (coop/host game_items).
    static constexpr std::array<std::string_view, 14> kKeyScenes{
        "InsideTheDekuTree", "DodongoCavern", "InsideJabuJabuBelly", "ForestTemple",
        "FireTemple", "WaterTemple", "ShadowTemple", "SpiritTemple", "BottomOfTheWell",
        "IceCavern", "GerudoTrainingGround", "InsideGanonCastle", "GerudoFortress",
        "FinalBattleField"};
    for (const auto s : kKeyScenes) {
        if (s == scene) {
            return coop::protocol::flag_hash("DungeonInfo." + std::string(scene) + ".SmallKeyNum");
        }
    }
    return 0;
}

bool IsBreakable(std::string_view name) {
    if (IsLock(name)) {
        return true;
    }
    for (const auto grass : kGrass) {
        if (name == grass) {
            return true;
        }
    }
    if (!name.starts_with("Obj_")) {
        return false;
    }
    for (const auto prefix : kPrefixes) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    for (const auto part : kParts) {
        if (name.find(part) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool IsMovable(std::string_view name) {
    if (!name.starts_with("Obj_") || name.find("BasePosAnchor") != std::string_view::npos) {
        return false;
    }
    return name.find("MovableBox_") != std::string_view::npos ||
           name.starts_with("Obj_SliderBox_") || name.starts_with("Obj_AmosStatue_") || IsRoller(name);
}

bool IsRoller(std::string_view name) {
    return name.starts_with("Obj_") && name.find("RollingRock_") != std::string_view::npos &&
           name.find("RollingRockArea") == std::string_view::npos;
}

bool IsBlockAnchor(std::string_view name) {
    return name == "Obj_MovableBox_BasePosAnchor";
}

bool IsPushBlock(std::string_view name) {
    return IsMovable(name) && (name.find("MovableBox_") != std::string_view::npos ||
                               name.starts_with("Obj_SliderBox_"));
}

bool IsSign(std::string_view name) {
    return name.starts_with("Obj_") &&
           (name.find("SignBoard_") != std::string_view::npos ||
            name.find("Signboard_") != std::string_view::npos ||
            name.find("GuidePost_") != std::string_view::npos);
}

bool IsFaller(std::string_view name) {
    // The Fire Temple's hammer pillars, and its hammer floor switches (life
    // SwitchHit: life 0 presses it once, its flag does the rest).
    return name.starts_with("Obj_FireTemple_HammerSlipPillar_") ||
           name.starts_with("Obj_HammerSwitch_");
}

bool IsLyingKey(std::string_view name) {
    return name.starts_with("Obj_Dungeon_SmallKey");
}

bool IsGoldToken(std::string_view name) {
    return name == "Obj_Token_Gold";
}

std::uint64_t TokenId(std::uint64_t skulltula_hash) {
    const std::uint64_t id = skulltula_hash ^ 0x746f6b656e5f6f6bULL;  // "token_ok"
    return id == 0 || id == ~std::uint64_t{0} ? id ^ 1u : id;  // never 0 or ~0 on the wire
}

bool IsShifter(std::string_view name) {
    // From the romfs (breakables-2): no LifeRef, moved or turned by their AI
    // when hit, and no 64-bit key, so before this nothing of them was shared.
    return name.starts_with("Obj_FireTemple_HammerDropFloor_") ||
           name.starts_with("Obj_FireTemple_HammerDropPillar_") ||
           name.starts_with("Obj_FireTemple_HammerStair_") ||
           name.starts_with("Obj_InsideTheDekuTree_FallingLadder_") ||
           // The Deku Tree's falling floors (dungeon-switches): the keyed ones
           // (_A_01, _A_02, with _A_03 the slingshot room's bridge) are shifters
           // too, so the other games see them fall before their key takes them
           // (IsFallTaken). The Shadow Temple's keyed one stays as ground.
           name.starts_with("Obj_InsideTheDekuTree_FallingFloor_") ||
           name == "Obj_ShadowTemple_FallingFloor_A_03";
}

bool IsGimmickMover(std::string_view name) {
    // deku-dungeon-2 (issue 196): the Deku Tree basement's moving floor, moved
    // by the room's GroundControlActor_ResidentMoveTo (its actor place
    // changes, unlike the Deku lifts, which only animate: OnOffAnim).
    return name.starts_with("Obj_Deku_MoveFloor_");
}

bool IsFallTaken(std::string_view name) {
    // A taken (keyed) piece whose fall or break plays in the game that did it:
    // the falling floors, and the visible parts of a web (the web itself is a
    // breakable and plays its own break on life 0).
    return (IsShifter(name) && name.find("FallingFloor_") != std::string_view::npos) ||
           (name.starts_with("Obj_") && name.find("SpiderWebParts_") != std::string_view::npos);
}

bool IsWorldObject(std::string_view name) {
    return IsBreakable(name) || IsMovable(name) || IsFaller(name) || IsLyingKey(name) ||
           IsGoldToken(name) || IsShifter(name) || IsGimmickMover(name);
}

// A shifter whose AI would put it back up (hammer drop floors, pillars and
// stair, the Deku Tree ladder): held at the place it was given. The
// falling floors `_A_03` have no AI in their packs: the normal settle.
bool HoldsPlace(std::string_view name) {
    return IsShifter(name) && name.find("FallingFloor_A_03") == std::string_view::npos;
}

std::optional<std::string> KeyCountOf(std::string_view scene) {
    // The scene, then the part before its first '_' (GerudoFortress_Adult).
    for (const std::string_view base : {scene, scene.substr(0, scene.find('_'))}) {
        const std::string name = "DungeonInfo." + std::string(base) + ".SmallKeyNum";
        for (const auto& item : coop::host::item_flags()) {
            if (item.name == name) {
                return name;
            }
        }
    }
    return std::nullopt;
}

namespace {
std::mutex give_back_mutex;
std::vector<std::string> give_backs;
std::vector<std::uint64_t> taken_keys;
} // namespace

void QueueGiveBack(std::string count) {
    std::lock_guard lock(give_back_mutex);
    if (give_backs.size() < 64) {
        give_backs.push_back(std::move(count));
    }
}

void QueueTakenKey(std::uint64_t key) {
    std::lock_guard lock(give_back_mutex);
    if (taken_keys.size() < 256) {
        taken_keys.push_back(key);
    }
}

std::vector<std::uint64_t> TakeTakenKeys() {
    std::lock_guard lock(give_back_mutex);
    return std::exchange(taken_keys, {});
}

std::vector<std::string> TakeGiveBacks() {
    std::lock_guard lock(give_back_mutex);
    return std::exchange(give_backs, {});
}

std::string SetTakenKey(coop::host::FlagStore& game, std::uint64_t key) {
    char hex[24];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(key));
    const auto flag = coop::protocol::FlagKey::key64_flag(key);
    if (const auto now = game.read(flag); now && now->value != 0) {
        return std::string("world: key64 ") + hex + " is 1 already";
    }
    if (!game.write(coop::protocol::FlagEntry(flag, coop::protocol::FlagKind::boolean, 1))) {
        return std::string("world: key64 ") + hex + ": write failed";
    }
    return std::string("world: key64 ") + hex + " set to 1 (a token taken elsewhere)";
}

std::string GiveBack(coop::host::FlagStore& game, std::string_view count) {
    const std::optional<std::string> name = count.empty() ? std::nullopt
                                                           : std::optional<std::string>(count);
    if (!name) {
        return "world: no count to give one back to";
    }
    for (const auto& item : coop::host::item_flags()) {
        if (item.name != *name) {
            continue;
        }
        const coop::host::FlagKey key(item.hash, 0);
        const auto now = game.read(key);
        if (!now) {
            return "world: " + *name + " not readable: nothing given back";
        }
        const auto was = static_cast<std::int32_t>(now->value);
        if (was <= 0) {
            return "world: " + *name + " is " + std::to_string(was) + ": nothing to give back";
        }
        if (!game.write(coop::protocol::FlagEntry(key, item.kind, static_cast<std::uint32_t>(was - 1)))) {
            return "world: " + *name + ": write failed";
        }
        return "world: one given back: " + *name + " " + std::to_string(was) + " -> " +
               std::to_string(was - 1);
    }
    return "world: " + *name + " is no item flag";
}

bool IsGrassActor(std::string_view name) {
    if (name.starts_with("Obj_Plant_Grass_")) {
        return true;
    }
    return std::find(kGrass.begin(), kGrass.end(), name) != kGrass.end();
}

namespace {

namespace proto = coop::protocol;

constexpr std::uint64_t kNoHash = ~std::uint64_t{0};
constexpr std::size_t kMaxInbox = 4096;

// The angle in radians between two turns (0 when either is unknown).
float Turn(const std::optional<CoopObjects::Mat3>& a, const std::optional<CoopObjects::Mat3>& b) {
    if (!a || !b) {
        return 0.0f;
    }
    float trace = 0.0f;  // of a transposed times b: the sum of the products
    for (std::size_t i = 0; i < 9; ++i) {
        trace += (*a)[i] * (*b)[i];
    }
    const float c = std::clamp((trace - 1.0f) / 2.0f, -1.0f, 1.0f);
    return std::acos(c);
}

float Distance(const proto::Vec3& a, const proto::Vec3& b) {
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    const float z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

bool Finite(const proto::Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// The rotation as a unit quaternion the codec takes; none: no turn.
proto::Quat UnitQuat(const std::optional<CoopObjects::Mat3>& m) {
    proto::Quat q{0.0f, 0.0f, 0.0f, 1.0f};
    if (!m) {
        return q;
    }
    const proto::Quat got = CoopObjects::QuatOf(*m);
    const float n =
        std::sqrt(got[0] * got[0] + got[1] * got[1] + got[2] * got[2] + got[3] * got[3]);
    if (!std::isfinite(n) || n < 0.5f || n > 2.0f) {
        return q;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        q[i] = got[i] / n;
    }
    return q;
}

bool HeapPointer(std::uint64_t v);  // below, with the sign probe

const std::string* SceneOfMessage(const proto::Message& m) {
    if (const auto* b = std::get_if<proto::WorldBreak>(&m)) {
        return &b->scene;
    }
    if (const auto* v = std::get_if<proto::WorldMove>(&m)) {
        return &v->scene;
    }
    if (const auto* p = std::get_if<proto::WorldPlace>(&m)) {
        return &p->scene;
    }
    if (const auto* h = std::get_if<proto::WorldHit>(&m)) {
        return &h->scene;
    }
    if (const auto* c = std::get_if<proto::SignCut>(&m)) {
        return &c->scene;
    }
    return nullptr;
}

// A log line: "%016llx" is the hash, the first "%s" the name, the second `more`.
std::string Line(std::string_view format, std::uint64_t hash, const std::string& name,
                 std::string_view more) {
    char hex[24];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    std::string out(format);
    if (const auto at = out.find("%016llx"); at != std::string::npos) {
        out.replace(at, 7, hex);
    }
    if (const auto at = out.find("%s"); at != std::string::npos) {
        out.replace(at, 2, name);
        if (const auto next = out.find("%s", at + name.size()); next != std::string::npos) {
            out.replace(next, 2, more);
        }
    }
    return out;
}

} // namespace

void WorldSync::Receive(const proto::Message& message) {
    if (SceneOfMessage(message) == nullptr) {
        return;
    }
    std::lock_guard lock(inbox_mutex_);
    if (inbox_.size() < kMaxInbox) {
        inbox_.push_back(message);
    }
}

void WorldSync::Reset() {
    scene_.clear();
    scene_since_ = 0;
    objects_.clear();
    broken_.clear();
    remote_.clear();
    gone_.clear();
    went_.clear();
    hits_.clear();
    cuts_.clear();
    skulltulas_.clear();
    skulltulas_seen_us_.clear();
    skulltulas_dead_.clear();
    token_spots_.clear();
    early_.clear();
    anchors_.clear();
    anchor_holds_.clear();
    refunds_.clear();
    refund_notes_.clear();
    own_drops_.clear();
    lent_hash_ = 0;
    lend_back_.clear();
    lend_quiet_until_ = 0;
    ignore_drops_ = 0;
    player_.reset();
    probe_.Reset();
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

std::uint8_t WorldSync::MoverOf(std::uint64_t hash) const {
    for (const auto& [address, o] : objects_) {
        if (o.hash == hash && o.ours) {
            return self_;
        }
    }
    const auto r = remote_.find(hash);
    return r != remote_.end() && !r->second.samples.empty() ? r->second.player : 0;
}

std::vector<std::uint64_t> WorldSync::Removes() const {
    std::vector<std::uint64_t> out;
    for (const auto& [address, o] : objects_) {
        if (o.remove) {
            out.push_back(address);
        }
    }
    return out;
}

std::optional<std::string> WorldSync::TookKey(std::uint64_t hash) const {
    for (const Gone& g : gone_) {
        if (g.object.key && g.object.hash == hash) {
            return g.object.count;
        }
    }
    for (const auto& [address, o] : objects_) {
        if (!o.key || o.hash != hash || !player_ || !Finite(o.position) ||
            Distance(*player_, o.position) > kBreakReachM) {
            continue;
        }
        // Off the walk next to our Link: taken. Still on it but lifted from
        // where it lay: Link is picking it up (the game raises it to his
        // hands before it goes), so the count rises here too.
        if (o.missed > 0 || Distance(o.position, o.first_position) >= kLiftedM) {
            return o.count;
        }
    }
    return std::nullopt;
}

std::uint64_t WorldSync::IdentifyToken(const proto::Vec3& at, std::uint64_t now_us) {
    if (!Finite(at)) {
        return 0;
    }
    auto& spots = token_spots_[scene_];
    std::uint64_t best = 0;
    float best_m = kTokenNearM;
    for (const auto& [hash, where] : skulltulas_) {
        if (const float m = Distance(where, at); m <= best_m) {
            best = TokenId(hash);
            best_m = m;
        }
    }
    if (best == 0) {
        best_m = kTokenSpotM;
        for (const auto& [id, where] : spots) {
            if (const float m = Distance(where, at); m <= best_m) {
                best = id;
                best_m = m;
            }
        }
    }
    if (best == 0) {
        // A Skulltula that died and left our walk a moment ago, farther off:
        // the one that left last, the nearer one on a tie. Never one whose
        // token is known already (a token of a Skulltula with no placement
        // must not take its id), nor one that left alive (its room unloaded).
        std::uint64_t latest = 0;
        best_m = kTokenFarM;
        for (const auto& [hash, where] : skulltulas_) {
            const auto seen = skulltulas_seen_us_.find(hash);
            if (seen == skulltulas_seen_us_.end() || seen->second >= now_us ||
                now_us - seen->second > kTokenAfterUs) {
                continue;  // still in this walk, or gone too long ago
            }
            if (!skulltulas_dead_.contains(hash) || spots.contains(TokenId(hash))) {
                continue;
            }
            const float m = Distance(where, at);
            if (m <= kTokenFarM && (seen->second > latest || (seen->second == latest && m < best_m))) {
                best = TokenId(hash);
                latest = seen->second;
                best_m = m;
            }
        }
    }
    if (best != 0 && spots.size() < 512) {
        spots[best] = at;
    }
    return best;
}

void WorldSync::Token(const proto::WorldBreak& b) {
    auto& spots = token_spots_[scene_];
    for (const auto& [hash, where] : skulltulas_) {
        if (TokenId(hash) == b.hash) {
            if (spots.size() < 512 && Finite(b.position)) {
                spots.emplace(b.hash, b.position);
            }
            taken_keys_.push_back(hash);
            return;
        }
    }
}

void WorldSync::Drain(std::uint64_t now_us) {
    std::vector<proto::Message> inbox;
    {
        std::lock_guard lock(inbox_mutex_);
        inbox.swap(inbox_);
    }
    for (proto::Message& m : inbox) {
        if (*SceneOfMessage(m) != scene_) {
            // The server's replay for a scene can come before our walk has
            // seen that scene: kept until the scene changes.
            if (!std::holds_alternative<proto::WorldMove>(m) && early_.size() < kMaxInbox) {
                early_.push_back(std::move(m));
            }
            continue;
        }
        if (const auto* b = std::get_if<proto::WorldBreak>(&m)) {
            if ((b->flags & proto::world_break::restored) != 0) {
                broken_.erase(b->hash);
                for (auto& [address, o] : objects_) {
                    if (o.hash == b->hash) {
                        o.remove = false;
                        o.life_written_us = 0;
                    }
                }
            } else if (const auto known = broken_.find(b->hash); known == broken_.end()) {
                Break k;
                k.got_us = now_us;
                k.kept = (b->flags & proto::world_break::kept) != 0;
                broken_.emplace(b->hash, k);
                // Our own take of the same small key that has not gone out
                // yet: the key is already off our walk next to our Link (or
                // waiting out kGoneHoldUs). Theirs was first: ours goes back.
                if (const auto count = TookKey(b->hash)) {
                    give_back_.push_back(*count);
                }
                Token(*b);
            } else if ((b->flags & proto::world_break::kept) != 0 && known->second.local) {
                // Both games opened this lock at once: two keys went for one
                // door. The higher player number gives its key back.
                if (b->player_number != self_ && self_ > b->player_number &&
                    !known->second.refunded) {
                    Refund(known->second, b->hash, now_us, "opened by both at once");
                }
            } else if (known->second.local && known->second.key) {
                // We took this small key too, and the server, which keeps the
                // first break only, relayed theirs: they were first. Ours goes
                // back, so the shared count rose once (issue 102).
                known->second.key = false;
                give_back_.push_back(known->second.count);
                Token(*b);
            } else if (known->second.local && now_us - known->second.got_us < kEchoUs) {
                // The life 0 we read a moment ago was this player's break
                // (it reached our copy through the enemy sharing first).
                known->second.local = false;
            }
        } else if (const auto* v = std::get_if<proto::WorldMove>(&m)) {
            Remote& r = remote_[v->hash];
            const bool fresh = !r.samples.empty() && now_us - r.got_us < kRemoteHoldUs;
            if (fresh && r.player != v->player_number) {
                if (v->player_number > r.player) {
                    continue;  // two movers: the lower number keeps it
                }
                r.samples.clear();
            }
            if (!fresh) {
                r.samples.clear();
            }
            r.player = v->player_number;
            r.got_us = now_us;
            if (r.samples.empty() || v->time_us > r.samples.back().time_us) {
                r.samples.push_back({v->time_us, v->position, v->rotation});
                if (r.samples.size() > kKeepSamples) {
                    r.samples.pop_front();
                }
            }
        } else if (const auto* h = std::get_if<proto::WorldHit>(&m)) {
            auto& waiting = hits_[h->hash];
            if (waiting.size() < proto::kMaxWorldHitsPerObject) {
                waiting.emplace_back(h->at, h->from);
            }
        } else if (const auto* c = std::get_if<proto::SignCut>(&m)) {
            if (cuts_.contains(c->hash) || cuts_.size() < proto::kMaxWorldHitObjects) {
                cuts_[c->hash] = c->words;
            }
        } else if (const auto* p = std::get_if<proto::WorldPlace>(&m)) {
            Remote& r = remote_[p->hash];
            r.place = Sample{now_us, p->position, p->rotation};
            ++r.place_serial;
            if (r.samples.empty()) {
                r.player = p->player_number;
            }
        }
    }
}

void WorldSync::LocalBreak(Object& o, std::uint64_t now_us, const char* how, Out& out) {
    if (o.sent_break || broken_.contains(o.hash)) {
        return;
    }
    o.sent_break = true;
    Break k;
    k.got_us = now_us;
    k.local = true;
    k.key = o.key;
    k.count = o.count;
    k.kept = o.kept;
    // Our own lock: the key our game spent on it is used up here, so it can
    // never also pay for another lock's give-back.
    k.paid = o.kept && TakeOwnDrop(now_us);
    broken_.emplace(o.hash, k);
    proto::WorldBreak b;
    b.scene = scene_;
    b.hash = o.hash;
    b.position = Finite(o.position) ? o.position : proto::Vec3{};
    if (o.kept) {
        b.flags = proto::world_break::kept;
        if (SmallKeyFlag(scene_) == 0 && !no_key_logged_) {
            no_key_logged_ = true;
            out.notes.push_back(Line("world: %016llx %s opened in a scene with no small key count%s",
                                     o.hash, o.name, " (a second key here cannot go back)"));
        }
    }
    out.send.emplace_back(std::move(b));
    out.notes.push_back(Line("world: %016llx %s broken here (%s): sent", o.hash, o.name, how));
}

void WorldSync::Refund(Break& b, std::uint64_t hash, std::uint64_t now_us, const char* why) {
    const std::uint32_t key = SmallKeyFlag(scene_);
    if (key == 0) {
        return;  // no key count here (logged once by LocalBreak)
    }
    // Both at once: our own break already took the key we spent on it (paid).
    // Else a drop of our count must be there, and is used up.
    if (!b.paid && !TakeOwnDrop(now_us)) {
        // Our count did not drop: our player spent nothing on it (the room
        // unloaded beside us, or another's spend came in): nothing back.
        refund_notes_.push_back(Line("world: %016llx lock %s, but no key of ours went: none back%s",
                                     hash, why, ""));
        return;
    }
    b.paid = false;
    b.refunded = true;
    refunds_.push_back(key);
    refund_notes_.push_back(Line("world: %016llx lock %s: one key back%s", hash, why, ""));
}

bool WorldSync::TakeOwnDrop(std::uint64_t now_us) {
    const std::uint32_t key = SmallKeyFlag(scene_);
    std::erase_if(own_drops_, [now_us](const auto& d) { return now_us - d.second > kOwnDropUs; });
    const auto drop = std::find_if(own_drops_.begin(), own_drops_.end(),
                                   [key](const auto& d) { return d.first == key; });
    if (key == 0 || drop == own_drops_.end()) {
        return false;
    }
    own_drops_.erase(drop);
    return true;
}

void WorldSync::OwnCountDrop(std::uint32_t key_flag, std::uint64_t now_us) {
    if (ignore_drops_ > 0) {
        if (now_us < ignore_drops_until_) {
            --ignore_drops_;  // our own take-back of a lent key, not a spend
            return;
        }
        ignore_drops_ = 0;
    }
    if (own_drops_.size() < 16) {
        own_drops_.emplace_back(key_flag, now_us);
    }
}

void WorldSync::Lend(Object& o, Break& b, std::uint64_t now_us, Out& out) {
    const std::uint32_t key = SmallKeyFlag(scene_);
    if (key == 0 || b.lent || b.refunded || b.lends >= kMaxLends || lent_hash_ != 0 ||
        now_us < lend_quiet_until_ || now_us - o.held_us < kLendWaitUs) {
        return;
    }
    auto count = KeyCountOf(scene_);
    if (!count) {
        return;
    }
    b.lent = true;
    ++b.lends;
    lent_hash_ = o.hash;
    lent_count_ = std::move(*count);
    out.key_refunds.push_back(key);
    out.notes.push_back(Line("world: %016llx %s was opened by another player and is shut here with our "
                             "Link at it: one key lent to open it%s",
                             o.hash, o.name, ""));
}

void WorldSync::TakeBack(Break& b, std::uint64_t hash, std::uint64_t now_us, const char* why) {
    b.lent = false;
    if (lent_hash_ != hash) {
        return;
    }
    lent_hash_ = 0;
    lend_back_.push_back(lent_count_);
    ++ignore_drops_;
    ignore_drops_until_ = now_us + kLendQuietUs;
    refund_notes_.push_back(Line("world: %016llx lent key not used (%s): taken back%s", hash, why, ""));
}

void WorldSync::LentGone(Break& b, std::uint64_t hash, std::uint64_t now_us) {
    if (!TakeOwnDrop(now_us)) {
        TakeBack(b, hash, now_us, "the lock went with no key of ours spent");
        return;
    }
    b.lent = false;
    b.refunded = true;  // settled: nothing more goes back for this lock
    lent_hash_ = 0;
    lend_quiet_until_ = now_us + kLendQuietUs;
    refund_notes_.push_back(Line("world: %016llx lock opened here with the lent key%s%s", hash, "", ""));
}

void WorldSync::ForgetKept() {
    lent_hash_ = 0;  // the save's key counts are written in: nothing to take back
    std::erase_if(broken_, [](const auto& b) { return b.second.kept; });
    for (auto& [address, o] : objects_) {
        if (o.kept) {
            o.remove = false;
            o.life_written_us = 0;
        }
    }
}

void WorldSync::MakeBreak(Object& o, Break& b, std::uint64_t now_us, Out& out) {
    if (o.remove || b.local) {
        return;  // our own break: the game is doing it
    }
    if (o.kept && player_ && Finite(o.position)) {
        const float away = Distance(*player_, o.position);
        if (away <= kLockHoldM) {
            // Our Link is at this door: never take the lock from under them (they
            // may be opening it). A player with no key would be stuck here (the
            // opener spent the group's key), so a key is lent for it.
            if (o.held_us == 0) {
                o.held_us = now_us;
            }
            Lend(o, b, now_us, out);
            return;
        }
        o.held_us = 0;
        if (b.lent) {
            if (away <= kLendAwayM) {
                return;  // a step back with the lent key: the lock stays
            }
            TakeBack(b, o.hash, now_us, "our Link left the lock");
        }
    }
    if (o.kept && !delete_locks_) {
        return;  // the lock stays: our player opens it with a lent key (SetDeleteLocks)
    }
    if (o.key) {
        // Taken in another game: ours goes at once (no life, no wait), also
        // in a game that loads the room later (the server's break).
        o.remove = true;
        out.notes.push_back(
            Line("world: %016llx %s taken by another player: deleted%s", o.hash, o.name, ""));
        return;
    }
    if (o.faller) {
        // It falls and stays (a bridge): never deleted. Life 0 makes the
        // game drop it; written again each second until the life reads 0
        // (a pillar that loaded fallen reads 0 already, nothing to write),
        // at most kFallerTries times: then one line, and it is left standing.
        if (o.life && *o.life > 0 && o.faller_tries <= kFallerTries &&
            (o.life_written_us == 0 || now_us - o.life_written_us >= kFallerRetryUs)) {
            if (o.faller_tries == kFallerTries) {
                ++o.faller_tries;
                out.notes.push_back(Line("world: %016llx %s did not fall on life 0 (%s): left standing",
                                         o.hash, o.name, std::to_string(kFallerTries) + " tries"));
                return;
            }
            ++o.faller_tries;
            out.lives.push_back({o.address, 0, 0, {}});
            out.notes.push_back(Line("world: %016llx %s %s", o.hash, o.name,
                                     o.life_written_us == 0 ? "felled by another player"
                                                            : "still standing: life 0 again"));
            o.life_written_us = now_us;
        }
        return;
    }
    if (b.got_us <= o.first_us + kStaleUs) {
        // Broken before we ever saw it (we came later, or it loaded later).
        o.remove = true;
        out.notes.push_back(
            Line("world: %016llx %s was broken before we saw it: deleted%s", o.hash, o.name, ""));
        return;
    }
    const bool alive = !o.life || *o.life > 0;
    if (o.life_written_us == 0) {
        if (o.life && alive) {
            out.lives.push_back({o.address, 0, 0, {}});
        }
        o.life_written_us = now_us;
        out.notes.push_back(Line("world: %016llx %s broken by another player%s", o.hash, o.name,
                                 o.life ? "" : " (no life: deleted after a wait)"));
        return;
    }
    // Still standing. Grass does not go when its life is 0; something with
    // no life has nothing to write; a life that is back above 0 did not take.
    const std::uint64_t wait = o.grass || !o.life ? kDeleteGrassUs : kDeleteUs;
    if (now_us - o.life_written_us >= wait && (o.grass || alive)) {
        o.remove = true;
        out.notes.push_back(
            Line("world: %016llx %s still stands after the break: deleted%s", o.hash, o.name, ""));
    }
}

std::optional<WorldSync::Sample> WorldSync::PoseAt(const Remote& r, std::uint64_t now_us) const {
    const std::uint64_t t = now_us > kBehindUs ? now_us - kBehindUs : 0;
    if (r.samples.empty() || (r.place && t >= r.samples.back().time_us)) {
        return r.place ? r.place : std::nullopt;
    }
    if (t <= r.samples.front().time_us) {
        return r.samples.front();
    }
    if (t >= r.samples.back().time_us) {
        return r.samples.back();
    }
    for (std::size_t i = 1; i < r.samples.size(); ++i) {
        const Sample& a = r.samples[i - 1];
        const Sample& b = r.samples[i];
        if (t > b.time_us) {
            continue;
        }
        const float f = static_cast<float>(t - a.time_us) / static_cast<float>(b.time_us - a.time_us);
        Sample s;
        s.time_us = t;
        float dot = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            dot += a.rotation[k] * b.rotation[k];
        }
        float n = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            s.rotation[k] = a.rotation[k] + f * ((dot < 0.0f ? -b.rotation[k] : b.rotation[k]) - a.rotation[k]);
            n += s.rotation[k] * s.rotation[k];
        }
        n = std::sqrt(n);
        for (std::size_t k = 0; k < 4; ++k) {
            s.rotation[k] = n > 1e-4f ? s.rotation[k] / n : (k == 3 ? 1.0f : 0.0f);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            s.position[k] = a.position[k] + f * (b.position[k] - a.position[k]);
        }
        return s;
    }
    return r.samples.back();
}

void WorldSync::Poses(std::uint64_t now_us, std::vector<CoopObjects::PoseWrite>& out) const {
    for (const auto& [address, hold] : anchor_holds_) {
        if (now_us < hold.until_us) {
            CoopObjects::PoseWrite w;
            w.address = address;
            w.position = hold.to;  // place only: the anchor keeps its turn
            out.push_back(w);
        }
    }
    for (const auto& [address, o] : objects_) {
        if (!o.following || o.remove) {
            continue;
        }
        const auto r = remote_.find(o.hash);
        if (r == remote_.end()) {
            continue;
        }
        if (const auto s = PoseAt(r->second, now_us)) {
            CoopObjects::PoseWrite w;
            w.address = address;
            w.position = s->position;
            w.rotation = CoopObjects::MatOf(s->rotation);
            out.push_back(w);
        }
    }
}

void WorldSync::Move(Object& o, std::uint8_t self, std::uint64_t now_us,
                     const std::optional<proto::Vec3>& player, Out& out) {
    if (o.faller || o.key || o.kept) {
        // Its fall (or its taking) is the break, not a move. A door's lock is
        // moved by its own game only: the two locks of a door part by 0.25 m
        // when a player comes near (run lock-open, 2026-11-12: sent as moves
        // and held in place in the other game).
        return;
    }
    const auto found = remote_.find(o.hash);
    Remote* r = found != remote_.end() ? &found->second : nullptr;
    bool remote = r != nullptr && !r->samples.empty() && now_us - r->got_us < kRemoteHoldUs;
    if (r != nullptr && !remote && r->place) {
        r->samples.clear();  // the mover stopped: only its rest place counts now
    }
    if (o.ours && remote) {
        if (r->player < self) {
            o.ours = false;  // two movers: the lower number keeps it
            out.notes.push_back(Line("world: %016llx %s: another player moves it too; theirs%s",
                                     o.hash, o.name, ""));
        } else {
            remote = false;
        }
    }
    if (!o.ours && r != nullptr && r->place && o.place_seen != r->place_serial) {
        o.place_seen = r->place_serial;
        o.settle_us = now_us;
        if (!o.following) {
            out.notes.push_back(
                Line("world: %016llx %s put where another player left it%s", o.hash, o.name, ""));
        }
    }
    // A shifter with an AI is held where it was put for good: its AI would
    // put it back up (it was not hit here).
    // A gimmick mover (issue 196) is held at another game's place while that
    // place is away from where the floor starts (the room's own control would
    // put ours back); a place back home lets go.
    const bool away = o.mover && r != nullptr && r->place &&
                      Distance(r->place->position, o.first_position) > kMoverAwayM;
    const bool settling =
        !o.ours && o.settle_us != 0 && (o.hold || away || now_us - o.settle_us < kSettleUs);
    if (!o.ours && (remote || settling)) {
        if (!o.following && !o.follow_from && !o.breakable && IsPushBlock(o.name)) {
            o.follow_from = o.rest;  // where our copy rested before (issue 147)
        }
        if (!o.following && remote) {
            out.notes.push_back(
                Line("world: %016llx %s moved by another player%s", o.hash, o.name, ""));
        }
        o.following = true;
        o.rest = o.position;
        o.rest_rotation = o.rotation;
        o.quiet_until = now_us + kQuietUs;
        return;
    }
    if (o.following) {
        o.following = false;
        o.rest = o.position;
        o.rest_rotation = o.rotation;
        o.quiet_until = now_us + kQuietUs;
        if (o.follow_from && Finite(o.position) &&
            Distance(*o.follow_from, o.position) > kBlockStartM) {
            // Issue 147: our block's own memory still says it rests at
            // follow_from; the glue rewrites that (RestWords), and its base
            // anchor, found there, is put where it rests now.
            Out::BlockFix fix;
            fix.address = o.address;
            fix.hash = o.hash;
            fix.name = o.name;
            fix.from = *o.follow_from;
            fix.to = o.position;
            float best = kAnchorNearM;
            for (const Anchor& a : anchors_) {
                const float d = Distance(a.position, fix.from);
                if (d <= best) {
                    best = d;
                    fix.anchor = a.address;
                }
            }
            if (fix.anchor != 0) {
                anchor_holds_[fix.anchor] = {fix.to, now_us + kAnchorHoldUs};
            }
            out.notes.push_back(Line("world: %016llx %s came to rest where another player left it; "
                                     "its base anchor%s",
                                     o.hash, o.name,
                                     fix.anchor != 0 ? " is put there too" : " was not found near its old place"));
            out.blocks.push_back(std::move(fix));
        }
        o.follow_from.reset();
    }
    if (now_us < o.quiet_until) {
        o.rest = o.position;  // settling after another game's move: no claim
        o.rest_rotation = o.rotation;
        return;
    }
    if (!o.ours) {
        // A shifter moves only by its own AI, hit here: the game that shows it
        // moved sends it, from across the room too (the slingshot room's eye
        // is shot from the chest ledge, 17 m from the ladder).
        const bool near = player && Distance(*player, o.position) <=
                                        (o.shifter || o.mover ? kShifterReachM : kMoverReachM);
        bool shifted = Distance(o.position, o.rest) > (o.breakable ? kLooseStartM : kBlockStartM);
        if (o.mover && Distance(o.position, o.first_position) <= kMoverAwayM &&
            Distance(o.rest, o.first_position) <= kMoverAwayM) {
            shifted = false;  // bobbing near its start (its wave loop): nobody's move
        }
        // A shifter may only turn (the Deku Tree's ladder tips over on its foot).
        const bool turned = o.shifter && Turn(o.rest_rotation, o.rotation) > kShiftTurnRad;
        if (!near || !Finite(o.position) || (!shifted && !turned)) {
            return;
        }
        o.ours = true;
        o.moved_us = now_us;
        o.sent_us = 0;
        out.notes.push_back(Line("world: %016llx %s moved here: sending%s", o.hash, o.name, ""));
    }
    if (o.stirred) {
        o.moved_us = now_us;
    }
    if (!Finite(o.position)) {
        o.ours = false;
        return;
    }
    if (now_us - o.moved_us >= kRestUs) {
        proto::WorldPlace p;
        p.scene = scene_;
        p.hash = o.hash;
        p.position = o.position;
        p.rotation = UnitQuat(o.rotation);
        out.send.emplace_back(std::move(p));
        o.ours = false;
        o.rest = o.position;
        o.rest_rotation = o.rotation;
        out.notes.push_back(Line("world: %016llx %s at rest: place sent%s", o.hash, o.name, ""));
        return;
    }
    if (o.sent_us == 0 || (now_us - o.sent_us >= kMoveEveryUs &&
                           Distance(o.position, o.sent_position) > 0.003f)) {
        proto::WorldMove v;
        v.scene = scene_;
        v.hash = o.hash;
        v.time_us = now_us;
        v.position = o.position;
        v.rotation = UnitQuat(o.rotation);
        if (!o.shifter && !o.roller && !o.mover && player && Distance(*player, o.position) <= 1.5f) {
            v.flags |= proto::world_move::carried;
        }
        out.send.emplace_back(std::move(v));
        o.sent_us = now_us;
        o.sent_position = o.position;
    }
}

void WorldSync::Swing(const proto::GrassCut& cut, const std::optional<proto::Vec3>& player,
                      std::uint64_t now_us, Out& out) {
    if (cut.scene != scene_ || std::isnan(cut.sweep[0][0])) {
        return;  // a sphere cut (an AI's), no sword
    }
    std::vector<proto::Vec3> points{cut.position};
    for (const proto::Vec3& p : cut.sweep) {
        if (Finite(p)) {
            points.push_back(p);
        }
    }
    for (auto& [address, o] : objects_) {
        if (!o.sign || now_us - o.hit_sent_us < kHitEveryUs) {
            continue;
        }
        const proto::Vec3* best = nullptr;
        float best_level = kSignReachM;
        for (const proto::Vec3& p : points) {
            const float dx = p[0] - o.position[0];
            const float dz = p[2] - o.position[2];
            const float level = std::sqrt(dx * dx + dz * dz);
            if (level <= best_level && std::fabs(p[1] - o.position[1]) <= kSignHighM) {
                best_level = level;
                best = &p;
            }
        }
        if (best == nullptr) {
            continue;
        }
        o.hit_sent_us = now_us;
        proto::WorldHit h;
        h.scene = scene_;
        h.hash = o.hash;
        h.at = *best;
        h.from = player && Finite(*player) ? *player : *best;
        out.send.emplace_back(std::move(h));
        out.notes.push_back(Line("world: %016llx %s: our sword passed it: hit sent%s", o.hash, o.name, ""));
        probe_.Hit({address, o.hash, o.name, o.position}, true, now_us);
    }
}

void WorldSync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
                     const std::vector<CoopObjects::Seen>& seen,
                     const std::optional<proto::Vec3>& player, Out& out) {
    self_ = self;
    player_ = player;
    if (scene != scene_) {
        if (lent_hash_ != 0) {  // left the dungeon with a lent key: it goes back
            lent_hash_ = 0;
            lend_back_.push_back(lent_count_);
            refund_notes_.push_back("world: lent key not used (our Link left the scene): taken back");
            ++ignore_drops_;
            ignore_drops_until_ = now_us + kLendQuietUs;
        }
        probe_.Reset();
        scene_ = scene;
        scene_since_ = now_us;
        objects_.clear();
        broken_.clear();
        remote_.clear();
        gone_.clear();
        went_.clear();
        hits_.clear();
        cuts_.clear();
        skulltulas_.clear();  // token_spots_ stay: a room loaded again
        skulltulas_seen_us_.clear();
        skulltulas_dead_.clear();
        anchor_holds_.clear();
        std::lock_guard lock(inbox_mutex_);
        for (proto::Message& m : early_) {
            if (*SceneOfMessage(m) == scene_) {
                inbox_.insert(inbox_.begin(), std::move(m));
            }
        }
        early_.clear();
    }
    for (const CoopObjects::Seen& s : seen) {
        if (s.name == "Enemy_Skulltula_Gold" && s.hash != 0 && s.hash != kNoHash &&
            Finite(s.position)) {
            skulltulas_[s.hash] = s.position;
            skulltulas_seen_us_[s.hash] = now_us;
            if (!s.life || s.life->life <= 0) {
                skulltulas_dead_.insert(s.hash);  // life 0, or its life can no longer be read
            } else {
                skulltulas_dead_.erase(s.hash);
            }
        }
    }
    Drain(now_us);
    for (std::string& count : give_back_) {
        out.notes.push_back("world: taken here and by another player at once (" + count +
                            "): theirs came first, ours goes back");
        out.give_back.push_back(std::move(count));
    }
    give_back_.clear();
    out.taken_keys.insert(out.taken_keys.end(), taken_keys_.begin(), taken_keys_.end());
    taken_keys_.clear();

    // A token has no placement: it gets its Skulltula's id (TokenId).
    std::vector<CoopObjects::Seen> with_tokens;
    const std::vector<CoopObjects::Seen>* walk = &seen;
    if (std::any_of(seen.begin(), seen.end(),
                    [](const CoopObjects::Seen& s) { return IsGoldToken(s.name); })) {
        with_tokens = seen;
        for (CoopObjects::Seen& s : with_tokens) {
            if (IsGoldToken(s.name) && (s.hash == 0 || s.hash == kNoHash)) {
                s.hash = IdentifyToken(s.position, now_us);
            }
        }
        walk = &with_tokens;
    }

    anchors_.clear();
    std::erase_if(anchor_holds_, [now_us](const auto& h) { return now_us >= h.second.until_us; });
    for (const CoopObjects::Seen& s : seen) {
        if (IsBlockAnchor(s.name)) {
            anchors_.push_back({s.address, s.position});
        }
    }
    std::set<std::uint64_t> now_seen;
    for (const CoopObjects::Seen& s : *walk) {
        if (s.hash == 0 || s.hash == kNoHash) {
            continue;
        }
        const bool faller = IsFaller(s.name);
        const bool token = IsGoldToken(s.name);
        const bool key = IsLyingKey(s.name) || token;
        const bool shifter = IsShifter(s.name);
        const bool mover = IsGimmickMover(s.name);
        const bool breakable = IsBreakable(s.name) || faller || key;
        if (!breakable && !shifter && !mover && !IsMovable(s.name)) {
            continue;
        }
        now_seen.insert(s.address);
        auto it = objects_.find(s.address);
        if (it != objects_.end() && it->second.hash != s.hash) {
            objects_.erase(it);  // the address has another actor now
            it = objects_.end();
        }
        std::optional<std::int32_t> life;
        if (s.life) {
            life = s.life->life;
        }
        if (it == objects_.end()) {
            Object o;
            o.address = s.address;
            o.hash = s.hash;
            o.name = s.name;
            o.breakable = breakable;
            o.grass = IsGrassActor(s.name);
            o.sign = IsSign(s.name);
            o.faller = faller;
            o.key = key;
            if (token) {
                o.count = kTokenCount;
            } else if (key) {
                o.count = KeyCountOf(scene_).value_or("");
            }
            o.shifter = shifter;
            o.mover = mover;
            o.roller = IsRoller(s.name);
            o.hold = shifter && HoldsPlace(s.name);
            o.life = life;
            o.position = s.position;
            o.rotation = s.rotation;
            o.first_position = s.position;
            o.first_us = now_us;
            o.rest = s.position;
            o.rest_rotation = s.rotation;
            o.sent_position = s.position;
            o.kept = IsLock(s.name);
            if (const auto b = broken_.find(s.hash);
                !o.kept && b != broken_.end() && b->second.seen_go && now_us - b->second.gone_us >= kRespawnUs &&
                (!life || *life > 0)) {
                // We saw it go and here it is again: the game put it back.
                broken_.erase(b);
                proto::WorldBreak back;
                back.scene = scene_;
                back.hash = o.hash;
                back.position = Finite(o.position) ? o.position : proto::Vec3{};
                back.flags = proto::world_break::restored;
                out.send.emplace_back(std::move(back));
                out.notes.push_back(
                    Line("world: %016llx %s is back: break forgotten%s", o.hash, o.name, ""));
            }
            objects_.emplace(s.address, std::move(o));
            continue;
        }
        Object& o = it->second;
        o.missed = 0;
        o.stirred = Finite(s.position) && Distance(s.position, o.position) > 0.003f;
        if (o.shifter && Turn(o.rotation, s.rotation) > 0.005f) {
            o.stirred = true;  // still turning
        }
        o.position = s.position;
        o.rotation = s.rotation;
        // As the enemy sharing reads it: a breakable whose life can no longer
        // be read has died (a crate's life object goes a frame after 0).
        if (!life && o.breakable && o.life && *o.life > 0) {
            life = 0;
        }
        if (life) {
            const bool died = o.life && *o.life > 0 && *life <= 0;
            o.life = life;
            if (died && o.breakable) {
                LocalBreak(o, now_us, "life 0", out);
            }
        }
    }

    for (auto it = objects_.begin(); it != objects_.end();) {
        Object& o = it->second;
        if (now_seen.contains(it->first) || ++o.missed < kMissedWalks) {
            ++it;
            continue;
        }
        const float from_link = player ? Distance(*player, o.position) : -1.0f;
        const bool far = !player || from_link > kBreakNearM;
        went_.emplace_back(now_us, far);
        if (const auto b = broken_.find(o.hash); b != broken_.end()) {
            b->second.seen_go = true;
            b->second.gone_us = now_us;
            if (o.kept && !b->second.local && !b->second.refunded && !o.remove && player &&
                from_link <= kLockHoldM + kBreakReachM) {
                // Another game's lock that we did not delete went next to our
                // Link: opened here too (a second key), or the room unloaded.
                // Decided after the same wait and unload test as a break.
                gone_.push_back({o, now_us, true});
            } else if (b->second.lent) {
                TakeBack(b->second, o.hash, now_us, "the lock left the walk");
            }
        } else if (o.breakable && player && (!o.life || *o.life > 0)) {
            const bool moved = Distance(o.position, o.first_position) >= 0.5f;
            if (from_link <= (moved ? kBreakNearM : kBreakReachM)) {
                gone_.push_back({o, now_us});
            }
        }
        it = objects_.erase(it);
    }
    if (!player) {
        for (const Gone& g : gone_) {
            if (const auto b = broken_.find(g.object.hash); b != broken_.end() && b->second.lent) {
                TakeBack(b->second, g.object.hash, now_us, "no Link in the walk");
            }
        }
        gone_.clear();  // no Link in the walk: the scene is loading or going
    }
    for (auto it = gone_.begin(); it != gone_.end();) {
        if (now_us - it->at_us < kGoneHoldUs) {
            ++it;
            continue;
        }
        // Left the walk next to our Link. Broken, unless a crowd went with
        // it and some of that crowd was far away: then the area unloaded.
        int together = 0;
        bool any_far = false;
        for (const auto& [at, far] : went_) {
            const std::uint64_t apart = at > it->at_us ? at - it->at_us : it->at_us - at;
            if (apart <= kGoneHoldUs) {
                ++together;
                any_far = any_far || far;
            }
        }
        if (together >= kUnloadCount && any_far) {
            out.notes.push_back(Line("world: %016llx %s left with the area: no break%s",
                                     it->object.hash, it->object.name, ""));
            if (const auto b = broken_.find(it->object.hash); b != broken_.end() && b->second.lent) {
                TakeBack(b->second, it->object.hash, now_us, "the room unloaded");
            }
        } else if (it->refund) {
            if (const auto b = broken_.find(it->object.hash); b != broken_.end() && !b->second.refunded) {
                if (b->second.lent) {
                    LentGone(b->second, it->object.hash, now_us);
                } else if (lent_hash_ != 0 || now_us < lend_quiet_until_) {
                    // The door's other lock: the lent key opens (opened) the pair.
                    refund_notes_.push_back(Line("world: %016llx lock went with the one a lent key "
                                                 "opens: none back%s%s", it->object.hash, "", ""));
                } else {
                    Refund(b->second, it->object.hash, now_us, "opened here after another player");
                }
            }
        } else {
            LocalBreak(it->object, now_us, "gone next to Link", out);
            if (const auto b = broken_.find(it->object.hash); b != broken_.end()) {
                b->second.seen_go = true;
                b->second.gone_us = it->at_us;
            }
        }
        it = gone_.erase(it);
    }
    std::erase_if(went_, [now_us](const auto& w) { return now_us - w.first > 4 * kGoneHoldUs; });
    // Keys given back for locks opened twice (Drain and the walk above).
    out.key_refunds.insert(out.key_refunds.end(), refunds_.begin(), refunds_.end());
    refunds_.clear();
    for (auto& note : refund_notes_) {
        out.notes.push_back(std::move(note));
    }
    refund_notes_.clear();
    for (std::string& count : lend_back_) {
        out.give_back.push_back(std::move(count));
    }
    lend_back_.clear();

    for (auto& [address, o] : objects_) {
        // Another player's swings at this sign, once our copy has settled in.
        if (const auto waiting = hits_.find(o.hash);
            waiting != hits_.end() && o.sign && now_us - o.first_us >= kHitWaitUs) {
            for (const auto& [at, from] : waiting->second) {
                out.hits.push_back({address, at, from});
            }
            out.notes.push_back(Line("world: %016llx %s hit by another player's sword%s", o.hash,
                                     o.name, ""));
            probe_.Hit({address, o.hash, o.name, o.position}, false, now_us);
            hits_.erase(waiting);
            o.hit_given_us = now_us;
        }
        // Another player's cut of this sign: its words, once the hits are in.
        // Written only into components the probe has read whole (it copies
        // a sign once it is near): until then it waits.
        if (const auto cut = cuts_.find(o.hash);
            cut != cuts_.end() && o.sign && now_us - o.first_us >= kHitWaitUs &&
            !hits_.contains(o.hash) && now_us - o.hit_given_us >= kCutAfterHitUs &&
            probe_.Copied(address, o.hash)) {
            std::size_t taken = 0;
            for (const proto::SignWord& w : cut->second) {
                const auto part = probe_.PartOf(address, o.hash, (w.key >> 32) - 2);
                if (!SignCutKeyAllowed(w.key) || HeapPointer(w.value) || !part) {
                    continue;
                }
                out.words.push_back({address, o.hash, w.key, w.value, *part});
                ++taken;
            }
            char more[64];
            std::snprintf(more, sizeof more, ": %zu of %zu words to write", taken,
                          cut->second.size());
            out.notes.push_back(Line("world: %016llx %s cut by another player%s", o.hash, o.name, more));
            cuts_.erase(cut);
        }
        if (const auto b = broken_.find(o.hash); b != broken_.end()) {
            MakeBreak(o, b->second, now_us, out);
            o.ours = false;
            o.following = false;
            continue;
        }
        Move(o, self, now_us, player, out);
    }
    // A mover that stopped without a rest place (it left, or the place was
    // lost): forgotten once nothing follows it.
    std::erase_if(remote_, [now_us](const auto& e) {
        const Remote& r = e.second;
        return !r.place && (r.samples.empty() || now_us - r.got_us > 4 * kRemoteHoldUs);
    });
}

namespace {

constexpr int kBlockLines = 60;                      // per block and scene
constexpr int kNoisy = 6;  // a word that changed this often is a counter: no more lines for it
constexpr int kAllLines = 1500;                      // per session

bool LooksLikePointer(std::uint64_t v) {
    return (v >> 32) != 0 && v < (1ull << 40) && v % 4 == 0;
}

} // namespace

void FoliageProbe::Compare(const ReadWord& read, const std::string& key, std::uint64_t address,
                           std::size_t count, const std::string& where,
                           std::vector<std::string>& out) {
    std::vector<std::uint64_t> now(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto w = read(address + 8 * i);
        if (!w) {
            return;  // not readable: not a buffer
        }
        now[i] = *w;
    }
    Block& b = blocks_[key];
    if (b.lines >= kBlockLines || lines_ >= kAllLines) {
        b.address = address;
        b.words = std::move(now);
        b.changes.assign(count, 0);
        return;
    }
    char text[64];
    std::string line;
    if (b.address != address || b.words.size() != count) {
        std::snprintf(text, sizeof text, " at %#llx:", static_cast<unsigned long long>(address));
        line = "foliage: " + key + text;
        b.changes.assign(count, 0);
        for (const std::uint64_t w : now) {
            std::snprintf(text, sizeof text, " %llx", static_cast<unsigned long long>(w));
            line += text;
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            if (now[i] == b.words[i] || ++b.changes[i] > kNoisy + 1) {
                continue;
            }
            if (b.changes[i] == kNoisy + 1) {
                std::snprintf(text, sizeof text, " +%#zx:noisy", 8 * i);
            } else {
                std::snprintf(text, sizeof text, " +%#zx:%llx>%llx", 8 * i,
                              static_cast<unsigned long long>(b.words[i]),
                              static_cast<unsigned long long>(now[i]));
            }
            line += text;
        }
        if (line.empty()) {
            return;
        }
        line = "foliage: " + key + " changed" + line;
    }
    b.address = address;
    b.words = std::move(now);
    ++b.lines;
    ++lines_;
    out.push_back(line + where);
}

std::vector<std::string> FoliageProbe::Look(const ReadWord& read, std::uint64_t main,
                                            const std::optional<std::array<float, 3>>& link) {
    std::vector<std::string> out;
    if (++calls_ % 4 != 0 || main == 0) {
        return out;
    }
    const auto module = read(main + kFoliageModule);
    const std::uint64_t flags = module && *module ? read(*module + 0x30).value_or(0) & 0xff : 0;
    // As the game's own 0x2c013c0: with bit 0 of the module's byte +0x30 set there is no scene.
    const bool off = (flags & 1) != 0;
    const auto holder = module && *module ? read(*module + 0xf0) : std::nullopt;
    const auto scene = !off && holder && *holder ? read(*holder + 0x338) : std::nullopt;
    const std::uint64_t s = scene.value_or(0);
    char text[160];
    if (s != scene_) {
        scene_ = s;
        blocks_.clear();
        std::snprintf(text, sizeof text,
                      "foliage: module %#llx flags %#llx holder %#llx scene %#llx%s",
                      static_cast<unsigned long long>(module.value_or(0)),
                      static_cast<unsigned long long>(flags),
                      static_cast<unsigned long long>(holder.value_or(0)),
                      static_cast<unsigned long long>(s), off ? " (off)" : "");
        out.emplace_back(text);
    }
    if (s == 0) {
        return out;
    }
    std::string where;
    if (link) {
        std::snprintf(text, sizeof text, " | Link %.2f %.2f %.2f", (*link)[0], (*link)[1],
                      (*link)[2]);
        where = text;
    }
    // The scene's first 0x400 bytes: SsboGrassCut is +0x58, SsboGather +0x88. A cut that
    // lands in a field this walk does not follow still shows here.
    Compare(read, "scene", s, 128, where, out);
    for (std::uint64_t field = 0x58; field < 0x88; field += 8) {
        const auto p = read(s + field);
        if (!p || !LooksLikePointer(*p)) {
            continue;
        }
        std::snprintf(text, sizeof text, "cut+%#llx", static_cast<unsigned long long>(field));
        const std::string key = text;
        Compare(read, key, *p, 64, where, out);
        for (std::uint64_t inner = 0; inner < 0x40; inner += 8) {
            const auto q = read(*p + inner);
            if (!q || !LooksLikePointer(*q) || *q == *p) {
                continue;
            }
            std::snprintf(text, sizeof text, "->+%#llx", static_cast<unsigned long long>(inner));
            Compare(read, key + text, *q, 64, where, out);
        }
    }
    return out;
}

namespace {

constexpr std::uint64_t kMalloc = 0x14c6c90;  // main + this (coop_puppet.h)
constexpr float kRequestDefault = 0.3f;       // what the game puts at +0x10

float FloatAt(const GrassSync::Slot& s, std::size_t offset) {
    float f = 0.0f;
    std::memcpy(&f, s.data() + offset, 4);
    return f;
}

void PutFloat(GrassSync::Slot& s, std::size_t offset, float f) {
    std::memcpy(s.data() + offset, &f, 4);
}

// The same cut: position and radius, bit for bit.
bool SameCut(const GrassSync::Slot& a, const GrassSync::Slot& b) {
    return std::memcmp(a.data(), b.data(), 16) == 0;
}

std::optional<std::uint64_t> SceneOf(const GrassSync::ReadWord& read, std::uint64_t main) {
    const auto module = read(main + kFoliageModule);
    if (!module || *module == 0) {
        return std::nullopt;
    }
    if ((read(*module + 0x30).value_or(1) & 1) != 0) {
        return std::nullopt;  // off, as the game's 0x2c013c0 reads it
    }
    const auto holder = read(*module + 0xf0);
    if (!holder || *holder == 0) {
        return std::nullopt;
    }
    const auto scene = read(*holder + 0x338);
    if (!scene || *scene == 0) {
        return std::nullopt;
    }
    return scene;
}

} // namespace

coop::protocol::GrassCut GrassSync::ToMessage(const Slot& slot, const std::string& scene) {
    coop::protocol::GrassCut c;
    c.scene = scene;
    for (std::size_t i = 0; i < 3; ++i) {
        c.position[i] = FloatAt(slot, 4 * i);
        for (std::size_t j = 0; j < 3; ++j) {
            c.sweep[i][j] = FloatAt(slot, 0x18 + 0xc * i + 4 * j);
        }
    }
    c.radius = FloatAt(slot, 0xc);
    c.flags = slot[0x14];
    return c;
}

GrassSync::Slot GrassSync::ToSlot(const coop::protocol::GrassCut& cut) {
    Slot s{};
    for (std::size_t i = 0; i < 3; ++i) {
        PutFloat(s, 4 * i, cut.position[i]);
        for (std::size_t j = 0; j < 3; ++j) {
            PutFloat(s, 0x18 + 0xc * i + 4 * j, cut.sweep[i][j]);
        }
    }
    PutFloat(s, 0xc, cut.radius);
    PutFloat(s, 0x10, kRequestDefault);
    s[0x14] = cut.flags;
    return s;
}

std::vector<coop::protocol::GrassCut> GrassSync::Look(const ReadWord& read, std::uint64_t main,
                                                      const std::string& scene,
                                                      std::uint64_t now_ms) {
    std::vector<coop::protocol::GrassCut> out;
    const auto s = main != 0 ? SceneOf(read, main) : std::nullopt;
    {
        std::lock_guard lock(mutex_);
        main_ = main;
        if (scene != scene_) {
            scene_ = scene;
            // Cuts of the scene we left go; ones for this scene that came
            // before we knew we were here (the server's replay) stay.
            std::erase_if(waiting_, [&scene](const Waiting& w) { return w.scene != scene; });
        }
        while (!made_.empty() && now_ms - made_.front().first > kEchoMs) {
            made_.pop_front();
        }
    }
    if (!s) {
        scene_address_ = 0;
        last_index_.reset();
        SetReady(false);
        return out;
    }
    if (*s != scene_address_ || scene != settle_scene_) {
        scene_address_ = *s;
        settle_scene_ = scene;
        settle_ms_ = now_ms;
        last_index_.reset();
    }
    const auto capacity = read(*s + 0x20);
    const auto entries = read(*s + 0x28);
    const auto written = read(*s + 0x38);
    const std::uint64_t cap = capacity.value_or(0) & 0xffffffff;
    if (!entries || !written || cap == 0 || cap > 4096 || (cap & (cap - 1)) != 0) {
        return out;
    }
    // Ready once the scene and its foliage scene stayed the same a while: on
    // a scene change the name can change before the foliage is the new one.
    SetReady(!scene.empty() && now_ms - settle_ms_ >= kSettleMs);
    if (!last_index_ || *written < *last_index_) {
        last_index_ = *written;  // the queue is new to us: only cuts from now on
        return out;
    }
    std::uint64_t from = *last_index_;
    if (*written - from > cap) {
        from = *written - cap;
    }
    last_index_ = *written;
    std::optional<Slot> previous;
    for (std::uint64_t i = from; i < *written; ++i) {
        const std::uint64_t entry = *entries + 16 * (i & (cap - 1));
        const auto pointer = read(entry);
        const auto sequence = read(entry + 8);
        // i + 1 while queued, i + capacity once taken out (and not reused).
        if (!pointer || *pointer == 0 || !sequence ||
            (*sequence != i + 1 && *sequence != i + cap)) {
            continue;
        }
        Slot slot{};
        bool ok = true;
        for (std::size_t k = 0; k < kCutSlotSize && ok; k += 8) {
            const auto w = read(*pointer + k);
            ok = w.has_value();
            if (ok) {
                std::memcpy(slot.data() + k, &*w, std::min<std::size_t>(8, kCutSlotSize - k));
            }
        }
        if (!ok || (previous && SameCut(*previous, slot))) {
            continue;
        }
        previous = slot;
        coop::protocol::GrassCut cut = ToMessage(slot, scene);
        bool echo = false;
        {
            std::lock_guard lock(mutex_);
            for (auto it = made_.begin(); it != made_.end(); ++it) {
                if (SameCut(it->second, slot)) {
                    made_.erase(it);
                    echo = true;
                    break;
                }
            }
        }
        const coop::protocol::Message check = cut;
        std::vector<std::uint8_t> bytes;
        if (echo || scene.empty() ||
            coop::protocol::encode(check, bytes) != coop::protocol::Error::none) {
            continue;  // made here for another game, or not a cut we can send
        }
        out.push_back(std::move(cut));
    }
    return out;
}

void GrassSync::SetReady(bool ready) {
    std::lock_guard lock(mutex_);
    ready_ = ready;
}

void GrassSync::Receive(const coop::protocol::GrassCut& cut) {
    std::lock_guard lock(mutex_);
    // A cut for a scene we are not in is kept only while no cut of ours says
    // where we are: the server replays a scene's cuts when our first sample
    // there reaches it, which can be before our objects walk saw the scene.
    if (waiting_.size() >= kMaxWaiting || (cut.scene != scene_ && ready_)) {
        return;
    }
    waiting_.push_back({cut.scene, ToSlot(cut)});
}

std::optional<CoopPuppet::Call> GrassSync::Next(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    // Only once the walk has seen this scene's foliage queue: a cut made
    // while the scene loads would go to the one we left.
    if (waiting_.empty() || main_ == 0 || !ready_ || this_frame_ >= kCutsPerFrame) {
        return std::nullopt;
    }
    while (!waiting_.empty() && waiting_.front().scene != scene_) {
        waiting_.pop_front();
    }
    if (waiting_.empty()) {
        return std::nullopt;
    }
    if (block_ == 0) {
        if (allocating_ || now_ms < retry_ms_) {
            return std::nullopt;
        }
        allocating_ = true;
        CoopPuppet::Call c;
        c.function = main_ + kMalloc;
        c.x[0] = 0x40;
        return c;
    }
    std::uint64_t module = 0;
    if (!guest.Read(main_ + kFoliageModule, &module, sizeof module) || module == 0) {
        waiting_.clear();
        return std::nullopt;
    }
    const Slot slot = waiting_.front().slot;
    waiting_.pop_front();
    if (!guest.Write(block_, slot.data(), slot.size())) {
        return std::nullopt;
    }
    made_.emplace_back(now_ms, slot);
    ++made_count_;
    ++this_frame_;
    CoopPuppet::Call c;
    c.function = main_ + kCutFoliage;
    c.x[0] = module;
    c.x[1] = block_;
    return c;
}

std::optional<CoopPuppet::Call> GrassSync::Step(CoopPuppet::Guest& guest, bool after_call,
                                                std::uint64_t result, std::uint64_t now_ms) {
    {
        std::lock_guard lock(mutex_);
        if (!after_call) {
            this_frame_ = 0;
        } else if (allocating_) {
            allocating_ = false;
            if (result == 0) {
                retry_ms_ = now_ms + 5000;
                notes_.emplace_back("grass: malloc gave nothing; again in 5 s");
            } else {
                block_ = result;
                char text[64];
                std::snprintf(text, sizeof text, "grass: request at %#llx",
                              static_cast<unsigned long long>(result));
                notes_.emplace_back(text);
            }
        }
    }
    return Next(guest, now_ms);
}

std::uint64_t GrassSync::Made() const {
    std::lock_guard lock(mutex_);
    return made_count_;
}

std::vector<std::string> GrassSync::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

// ---- SignProbe (docs/world.md, "Signs: the probe") ----

namespace {

// Snapshot keys: region in the high half, byte offset in the low half.
// Region 0 is the actor, 1 its component table, 2 + i the component in
// table entry i.
constexpr std::uint64_t kRegionActor = 0;
constexpr std::uint64_t kRegionTable = 1;
constexpr std::uint64_t kRegionComponent = 2;

std::uint64_t Key(std::uint64_t region, std::uint64_t offset) {
    return (region << 32) | offset;
}

bool ProbePointer(std::uint64_t v) {
    return v >= 0x1000 && v < (1ull << 48) && v % 8 == 0;
}

} // namespace

std::string SignProbe::Where(std::uint64_t key) {
    const std::uint64_t region = key >> 32;
    const auto offset = static_cast<unsigned long long>(key & 0xffffffffu);
    char text[48];
    if (region == kRegionActor) {
        std::snprintf(text, sizeof text, "actor+0x%llx", offset);
    } else if (region == kRegionTable) {
        std::snprintf(text, sizeof text, "table+0x%llx", offset);
    } else {
        std::snprintf(text, sizeof text, "c+0x%llx+0x%llx",
                      static_cast<unsigned long long>((region - kRegionComponent) * 8), offset);
    }
    return text;
}

SignProbe::Snapshot SignProbe::Read(const ReadWord& read, std::uint64_t address) {
    Snapshot out;
    for (std::size_t i = 0; i < kActorWords; ++i) {
        const auto w = read(address + 8 * i);
        if (!w) {
            return {};  // the actor is not there
        }
        out[Key(kRegionActor, 8 * i)] = *w;
    }
    const std::uint64_t table = out[Key(kRegionActor, kActorTable)];
    if (!ProbePointer(table)) {
        return out;
    }
    std::vector<std::uint64_t> entries;
    for (std::size_t i = 0; i < kTableWords; ++i) {
        const auto w = read(table + 8 * i);
        if (!w) {
            break;
        }
        out[Key(kRegionTable, 8 * i)] = *w;
        entries.push_back(*w);
    }
    std::set<std::uint64_t> done;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::uint64_t c = entries[i];
        if (!ProbePointer(c) || !done.insert(c).second) {
            continue;  // empty, not a pointer, or read under an earlier entry
        }
        Snapshot part;
        bool ok = true;
        for (std::size_t j = 0; j < kComponentWords && ok; ++j) {
            const auto w = read(c + 8 * j);
            ok = w.has_value();
            if (ok) {
                part[Key(kRegionComponent + i, 8 * j)] = *w;
            }
        }
        if (ok) {
            out.insert(part.begin(), part.end());
        }
    }
    return out;
}

void SignProbe::Hit(const Sign& sign, bool ours, std::uint64_t now_us) {
    State& s = signs_[sign.address];
    if (s.hash != sign.hash) {
        s = State{};
        s.hash = sign.hash;
        s.name = sign.name;
    }
    if (s.event) {
        return;  // the first hit's lines are still to come
    }
    Event e;
    e.ours = ours;
    e.time_us = now_us;
    // Our swing is read from the grass queue a little after it was made:
    // take a copy from before that. A given hit lands after this call.
    const std::uint64_t age = ours ? kBaseAgeUs : 0;
    for (auto it = s.bases.rbegin(); it != s.bases.rend(); ++it) {
        if (it->time_us + age <= now_us) {
            e.base = *it;
            break;
        }
    }
    s.event = std::move(e);
}

std::string SignProbe::Report(const State& s, const Event& e, const Snapshot& now,
                              std::uint64_t after_us) {
    const std::uint64_t tenths = (after_us + 50'000) / 100'000;
    std::string line = Line("sign probe: %016llx %s", s.hash, s.name, "") +
                       (e.ours ? " our cut +" : " given hit +") + std::to_string(tenths / 10) +
                       "." + std::to_string(tenths % 10) + " s:";
    if (!e.base) {
        return line + " no copy from before the hit";
    }
    if (now.empty()) {
        return line + " not readable";
    }
    std::size_t changed = 0;
    std::string shown;
    auto note = [&](std::uint64_t key, std::optional<std::uint64_t> was,
                    std::optional<std::uint64_t> is) {
        if (s.noisy.contains(key) || was == is) {
            return;
        }
        if (++changed > kMaxShown) {
            return;
        }
        char text[64];
        std::snprintf(text, sizeof text, " %s:", Where(key).c_str());
        shown += text;
        if (was) {
            std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(*was));
            shown += text;
        } else {
            shown += "-";
        }
        shown += ">";
        if (is) {
            std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(*is));
            shown += text;
        } else {
            shown += "-";
        }
    };
    const Snapshot& before = e.base->words;
    for (const auto& [key, was] : before) {
        const auto it = now.find(key);
        note(key, was, it == now.end() ? std::nullopt : std::optional(it->second));
    }
    for (const auto& [key, is] : now) {
        if (!before.contains(key)) {
            note(key, std::nullopt, is);
        }
    }
    std::string tail = " " + std::to_string(changed) + " words changed";
    if (changed > kMaxShown) {
        tail += " (first ones shown)";
    }
    return line + tail + (changed > 0 ? ":" : "") + shown;
}

namespace {

// A guest heap pointer (as the logs show them: 0x21_xxxxxxxx): not portable.
bool HeapPointer(std::uint64_t v) {
    return v >= 0x10'0000'0000ull && v < 0x100'0000'0000ull && v % 8 == 0;
}

// Two floats that are a place or a turn (a body's transform): the pieces
// fly, a copy of where they were is no cut.
bool PoseWord(std::uint64_t v) {
    auto real = [](std::uint32_t bits) {
        float f;
        std::memcpy(&f, &bits, sizeof f);
        const float a = std::fabs(f);
        return std::isfinite(f) && a >= 1e-4f && a <= 1e6f;
    };
    return real(static_cast<std::uint32_t>(v)) && real(static_cast<std::uint32_t>(v >> 32));
}

} // namespace

bool SignCutKeyAllowed(std::uint64_t key) {
    // (component table entry offset, byte offset in the component)
    static constexpr std::array<std::pair<std::uint64_t, std::uint64_t>, 11> kCutWords{{
        {0x58, 0x120}, {0x60, 0xc8},                  // NaN -> a float
        {0x70, 0x18},                                 // flag 0x80000 (high half)
        {0x130, 0xb0}, {0x130, 0xc0},                 // contacts, their 0x400 bit
        {0x160, 0x60}, {0x160, 0x70},                 // the same, second component
        {0x268, 0x98}, {0x268, 0xa0}, {0x268, 0xa8},  // the cut point
        {0x268, 0x1e8},                               // flag 0x80000
    }};
    const std::uint64_t region = key >> 32;
    if (region < 2) {
        return false;
    }
    const std::pair<std::uint64_t, std::uint64_t> word{(region - 2) * 8, key & 0xffffffffu};
    return std::find(kCutWords.begin(), kCutWords.end(), word) != kCutWords.end();
}

bool SignWordWritable(std::uint64_t current, std::uint64_t value) {
    if (HeapPointer(value) || HeapPointer(current) || PoseWord(current)) {
        return false;
    }
    return !PoseWord(value) || current == 0;
}

std::vector<proto::SignWord> SignProbe::CutWords(const Snapshot& before, const Snapshot& now,
                                                  const std::set<std::uint64_t>& noisy) {
    constexpr std::uint64_t kLifeRegion = 2 + 0x2b8 / 8;  // the hit record: the replay's own
    std::vector<proto::SignWord> out;
    for (const auto& [key, is] : now) {
        const std::uint64_t region = key >> 32;
        if (region < 2 || region == kLifeRegion || region - 2 >= proto::kSignCutEntries ||
            (key & 0xffffffffu) >= proto::kSignCutBytes || noisy.contains(key)) {
            continue;
        }
        const auto was = before.find(key);
        if (!SignCutKeyAllowed(key) || was == before.end() || was->second == is || HeapPointer(is) ||
            HeapPointer(was->second) || (PoseWord(is) && PoseWord(was->second))) {
            continue;
        }
        out.push_back({key, is});
        if (out.size() == proto::kMaxSignCutWords) {
            break;
        }
    }
    return out;
}

std::vector<std::string> SignProbe::Look(const ReadWord& read, const std::vector<Sign>& signs,
                                         const std::optional<proto::Vec3>& player,
                                         std::uint64_t now_us, std::vector<Cut>* cuts) {
    std::vector<std::string> out;
    // Forget signs the walk no longer sees. One that went while a hit was
    // still to report is logged: a cut that deletes or replaces the actor.
    for (auto it = signs_.begin(); it != signs_.end();) {
        const bool seen = std::any_of(signs.begin(), signs.end(), [&it](const Sign& s) {
            return s.address == it->first && s.hash == it->second.hash;
        });
        if (seen) {
            ++it;
            continue;
        }
        const State& s = it->second;
        if (s.event && lines_ < kMaxLines) {
            ++lines_;
            const std::uint64_t after = now_us - s.event->time_us;
            const std::uint64_t tenths = (after + 50'000) / 100'000;
            out.push_back(Line("sign probe: %016llx %s", s.hash, s.name, "") +
                          (s.event->ours ? " our cut" : " given hit") + ": gone after the hit (+" +
                          std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s)");
        }
        it = signs_.erase(it);
    }
    // The nearest few, a sign with a hit to report first.
    std::vector<std::pair<float, const Sign*>> pick;
    for (const Sign& sign : signs) {
        const auto st = signs_.find(sign.address);
        const bool hit = st != signs_.end() && st->second.event.has_value();
        float d = 1e9f;
        if (player) {
            const float dx = sign.position[0] - (*player)[0];
            const float dy = sign.position[1] - (*player)[1];
            const float dz = sign.position[2] - (*player)[2];
            d = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        if (hit) {
            pick.emplace_back(-1.0f, &sign);
        } else if (d <= kNearM) {
            pick.emplace_back(d, &sign);
        }
    }
    std::sort(pick.begin(), pick.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (pick.size() > kMaxSigns) {
        pick.resize(kMaxSigns);
    }
    for (const auto& [distance, sign] : pick) {
        State& s = signs_[sign->address];
        if (s.hash != sign->hash || s.name.empty()) {
            s = State{};
            s.hash = sign->hash;
            s.name = sign->name;
        }
        if (s.event) {
            Event& e = *s.event;
            const std::uint64_t after = now_us - e.time_us;
            const std::uint64_t due = e.reported == 0 ? kFirstReportUs : kLastReportUs;
            if (after < due) {
                continue;
            }
            const Snapshot now = Read(read, sign->address);
            if (lines_ < kMaxLines) {
                ++lines_;
                out.push_back(Report(s, e, now, after));
            }
            if (cuts != nullptr && e.ours && e.reported == 0 && e.base && !now.empty()) {
                Cut c{sign->address, s.hash, CutWords(e.base->words, now, s.noisy)};
                if (!c.words.empty()) {
                    cuts->push_back(std::move(c));
                }
            }
            if (++e.reported >= 2) {
                // After a cut the sign is not what it was: start again.
                s.event.reset();
                s.bases.clear();
            }
            continue;
        }
        if (!s.bases.empty() && now_us - s.bases.back().time_us < kBaseEveryUs) {
            continue;
        }
        Base b;
        b.time_us = now_us;
        b.words = Read(read, sign->address);
        if (b.words.empty()) {
            continue;
        }
        if (!s.bases.empty()) {
            const Snapshot& last = s.bases.back().words;
            for (const auto& [key, word] : b.words) {
                const auto it = last.find(key);
                if (it == last.end() || it->second != word) {
                    s.noisy.insert(key);
                }
            }
        }
        s.bases.push_back(std::move(b));
        while (s.bases.size() > 3) {
            s.bases.pop_front();
        }
    }
    return out;
}

const SignProbe::Snapshot* SignProbe::CopyOf(std::uint64_t address, std::uint64_t hash) const {
    const auto it = signs_.find(address);
    if (it == signs_.end() || it->second.hash != hash) {
        return nullptr;
    }
    const State& s = it->second;
    if (!s.bases.empty()) {
        return &s.bases.back().words;
    }
    return s.event && s.event->base ? &s.event->base->words : nullptr;
}

bool SignProbe::Copied(std::uint64_t address, std::uint64_t hash) const {
    return CopyOf(address, hash) != nullptr;
}

std::optional<SignProbe::Part> SignProbe::PartOf(std::uint64_t address, std::uint64_t hash,
                                                 std::uint64_t entry) const {
    const Snapshot* copy = CopyOf(address, hash);
    if (copy == nullptr || entry >= kTableWords) {
        return std::nullopt;
    }
    const auto table = copy->find(Key(kRegionActor, kActorTable));
    const auto part = copy->find(Key(kRegionTable, 8 * entry));
    if (table == copy->end() || part == copy->end() || !ProbePointer(table->second) ||
        !ProbePointer(part->second)) {
        return std::nullopt;
    }
    if (!copy->contains(Key(kRegionComponent + entry, 8 * (kComponentWords - 1)))) {
        return std::nullopt;  // not read whole (or read under another entry)
    }
    return Part{table->second, part->second};
}

std::optional<std::uint64_t> WorldSync::SignWordTarget(const SignProbe::ReadWord& read,
                                                       const Out::Word& w,
                                                       std::string* why) const {
    auto fail = [why](const std::string& reason) -> std::optional<std::uint64_t> {
        if (why) {
            *why = reason;
        }
        return std::nullopt;
    };
    auto hex = [](std::uint64_t v) {
        char text[24];
        std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(v));
        return std::string(text);
    };
    const auto o = objects_.find(w.address);
    if (o == objects_.end() || o->second.hash != w.hash || !o->second.sign) {
        return fail("the sign left the walk");
    }
    if (!SignCutKeyAllowed(w.key)) {
        return fail("word off the list");
    }
    if (!ProbePointer(w.part.table) || !ProbePointer(w.part.component)) {
        return fail("no component pointer from the probe");
    }
    const auto table = read(w.address + SignProbe::kActorTable);
    if (!table || *table != w.part.table) {
        return fail("component table moved (" + hex(table.value_or(0)) + ", probe saw " +
                    hex(w.part.table) + ")");
    }
    const std::uint64_t entry = (w.key >> 32) - 2;
    const auto part = read(*table + 8 * entry);
    const std::uint64_t offset = w.key & 0xffffffffu;
    if (!part || *part != w.part.component) {
        return fail("component moved (" + hex(part.value_or(0)) + ", probe saw " +
                    hex(w.part.component) + ")");
    }
    if (offset >= 8 * SignProbe::kComponentWords) {
        return fail("offset past the copy");
    }
    const auto now = read(*part + offset);
    if (!now) {
        return fail("not readable");
    }
    if (!SignWordWritable(*now, w.value)) {
        return fail("holds " + hex(*now) + " here (a pointer or two floats), " + hex(w.value) +
                    " not written over it");
    }
    return *part + offset;
}

void SignProbe::Reset() {
    signs_.clear();
}

void WorldSync::ProbeSigns(const SignProbe::ReadWord& read, std::uint64_t now_us, Out& out) {
    std::vector<SignProbe::Sign> signs;
    for (const auto& [address, o] : objects_) {
        if (o.sign) {
            signs.push_back({address, o.hash, o.name, o.position});
        }
    }
    std::vector<SignProbe::Cut> cuts;
    for (std::string& line : probe_.Look(read, signs, player_, now_us, &cuts)) {
        out.notes.push_back(std::move(line));
    }
    for (SignProbe::Cut& c : cuts) {
        proto::SignCut m;
        m.scene = scene_;
        m.hash = c.hash;
        m.words = std::move(c.words);
        char more[48];
        std::snprintf(more, sizeof more, ": %zu words sent", m.words.size());
        const auto it = objects_.find(c.address);
        out.notes.push_back(Line("world: %016llx %s cut here%s", c.hash,
                                 it != objects_.end() ? it->second.name : std::string("?"), more));
        out.send.emplace_back(std::move(m));
    }
}

bool IsGimmick(std::string_view name) {
    // dungeon-switches (the Scrub Brothers 2-3-1, docs/dungeon.md "Order
    // puzzles"): the brothers and their manager, so a run logs which words a
    // hit and the manager's count step.
    if (name == "Enemy_Scrub_Deku_Brothers" || name == "Scrub_Deku_Brothers_Manager") {
        return true;
    }
    if (!name.starts_with("Obj_")) {
        return false;
    }
    constexpr std::string_view kParts[] = {
        "StepSwitch_", "EyeSwitch_", "SwitchHit_", "SolarSwitch_", "SwitchCandleStick_",
        "WaterPlane", "SwitchShutter_", "CandleStick_", "ElevatorLift", "MoveFloor_",
        // world-objects: the push blocks and their base anchor (issue 147: which words a
        // real push steps), the Deku Tree ladder, its pillar and the trap floors (issue 129).
        "MovableBox_", "FallingLadder_", "Stopper_", "FallingFloor_",
    };
    for (const std::string_view p : kParts) {
        if (name.find(p) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

void GimmickProbe::Reset() {
    states_.clear();
    last_us_ = 0;
}

std::vector<std::string> GimmickProbe::Look(const SignProbe::ReadWord& read,
                                            const std::vector<Gimmick>& gimmicks,
                                            const std::optional<proto::Vec3>& player, std::uint64_t now_us) {
    std::vector<std::string> lines;
    if (!player || (last_us_ != 0 && now_us >= last_us_ && now_us - last_us_ < kEveryUs)) {
        return lines;
    }
    last_us_ = now_us;
    std::vector<const Gimmick*> near;
    for (const Gimmick& g : gimmicks) {
        if (Distance(*player, g.position) <= kNearM) {
            near.push_back(&g);
        }
    }
    std::sort(near.begin(), near.end(), [&](const Gimmick* a, const Gimmick* b) {
        return Distance(*player, a->position) < Distance(*player, b->position);
    });
    if (near.size() > kMaxGimmicks) {
        near.resize(kMaxGimmicks);
    }
    std::set<std::uint64_t> looked;
    for (const Gimmick* g : near) {
        looked.insert(g->address);
        State& st = states_[g->address];
        if (st.hash != g->hash || st.name != g->name) {
            st = State{};
            st.hash = g->hash;
            st.name = g->name;
            if (lines_ < kMaxLines) {
                ++lines_;
                lines.push_back(Line("gimmick probe: watching %016llx %s", g->hash, g->name, ""));
            }
        }
        SignProbe::Snapshot now = SignProbe::Read(read, g->address);
        if (now.empty()) {
            continue;
        }
        if (st.copies.size() == 2) {
            const SignProbe::Snapshot& a = st.copies[0];
            const SignProbe::Snapshot& b = st.copies[1];
            std::string shown;
            std::size_t count = 0;
            for (const auto& [key, value] : now) {
                const auto ia = a.find(key);
                const auto ib = b.find(key);
                if (ia == a.end() || ib == b.end() || ia->second != ib->second || ib->second == value) {
                    continue;  // not stable before, or no step
                }
                int& n = st.steps[key];
                if (++n > kNoisySteps) {
                    continue;  // muted: it steps too often
                }
                ++count;
                if (count <= kMaxShown) {
                    char word[96];
                    std::snprintf(word, sizeof word, " %s:%llx>%llx", SignProbe::Where(key).c_str(),
                                  static_cast<unsigned long long>(ib->second),
                                  static_cast<unsigned long long>(value));
                    shown += word;
                }
            }
            if (count != 0 && lines_ < kMaxLines) {
                ++lines_;
                const float d = Distance(*player, g->position);
                char more[80];
                std::snprintf(more, sizeof more, "%.1f m from our Link: %zu words stepped:", d, count);
                lines.push_back(Line("gimmick probe: %016llx %s %s", g->hash, g->name, more) + shown);
            }
        }
        st.copies.push_back(std::move(now));
        while (st.copies.size() > 2) {
            st.copies.pop_front();
        }
    }
    // Forget what is out of reach (unloaded, or far): a fresh start when back.
    for (auto it = states_.begin(); it != states_.end();) {
        it = looked.contains(it->first) ? std::next(it) : states_.erase(it);
    }
    return lines;
}

RestScan RestWords(const std::function<std::optional<std::uint64_t>(std::uint64_t)>& read,
                   std::uint64_t actor, const proto::Vec3& from, const proto::Vec3& to) {
    RestScan out;
    if (!Finite(from) || !Finite(to) || Distance(from, to) < WorldSync::kBlockStartM ||
        std::max({std::fabs(from[0]), std::fabs(from[1]), std::fabs(from[2])}) < 1.0f) {
        return out;
    }
    struct Region {
        std::uint64_t base = 0;
        std::size_t bytes = 0;
        std::string name;
    };
    std::vector<Region> regions{{actor, 0x600, "actor"}};
    std::set<std::uint64_t> taken{actor};
    char text[48];
    if (const auto table = read(actor + SignProbe::kActorTable); table && HeapPointer(*table)) {
        for (std::uint64_t at = 0; at < SignProbe::kTableWords * 8; at += 8) {
            const auto c = read(*table + at);
            if (c && HeapPointer(*c) && taken.insert(*c).second) {
                std::snprintf(text, sizeof text, "c+%#llx", static_cast<unsigned long long>(at));
                regions.push_back({*c, SignProbe::kComponentWords * 8, text});
            }
        }
        if (const auto physics = read(*table + 0x50); physics && HeapPointer(*physics)) {
            if (const auto root = read(*physics + 0x20); root && HeapPointer(*root) &&
                                                         taken.insert(*root).second) {
                regions.push_back({*root, 0x400, "physics root"});
            }
        }
    }
    const auto matches = [&](const std::optional<float>& v, int axis) {
        return v && std::isfinite(*v) && *v != 0.0f && std::fabs(*v - from[axis]) <= kRestMatchM;
    };
    const auto bits = [](float f) {
        std::uint32_t b = 0;
        std::memcpy(&b, &f, 4);
        return b;
    };
    std::size_t triplets = 0;
    for (const Region& region : regions) {
        std::vector<std::optional<float>> f(region.bytes / 4);
        for (std::size_t w = 0; w < region.bytes / 8; ++w) {
            if (const auto v = read(region.base + 8 * w)) {
                float lo = 0.0f, hi = 0.0f;
                const auto lo_bits = static_cast<std::uint32_t>(*v);
                const auto hi_bits = static_cast<std::uint32_t>(*v >> 32);
                std::memcpy(&lo, &lo_bits, 4);
                std::memcpy(&hi, &hi_bits, 4);
                f[2 * w] = lo;
                f[2 * w + 1] = hi;
            }
        }
        std::set<std::size_t> used;
        for (std::size_t i = 0; i < f.size(); ++i) {
            const std::uint64_t offset = 4 * i;
            if (region.name == "actor" && offset >= 0x31c && offset < 0x34c) {
                continue;  // its own place and turn: the setter wrote them
            }
            for (const std::size_t stride : {std::size_t{1}, std::size_t{4}}) {
                if (i + 2 * stride >= f.size() || used.contains(i) || used.contains(i + stride) ||
                    used.contains(i + 2 * stride) || !matches(f[i], 0) || !matches(f[i + stride], 1) ||
                    !matches(f[i + 2 * stride], 2)) {
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    const std::size_t k = i + axis * stride;
                    used.insert(k);
                    out.writes.push_back({region.base + 4 * k, bits(to[axis])});
                }
                std::snprintf(text, sizeof text, "+%#llx", static_cast<unsigned long long>(offset));
                out.where.push_back(region.name + text + (stride == 4 ? " (matrix)" : ""));
                ++triplets;
                break;
            }
        }
    }
    if (triplets > kMaxRestTriplets) {
        out.too_many = true;
        out.writes.clear();
    }
    return out;
}

std::optional<std::uint64_t> FindBlockNode(
    const std::function<std::optional<std::uint64_t>(std::uint64_t)>& read, std::uint64_t actor,
    std::uint64_t main) {
    using namespace BlockNode;
    if (!HeapPointer(actor) || main == 0) {
        return std::nullopt;
    }
    const auto is_node = [&](std::uint64_t p) {
        const auto v1 = read(p);
        if (!v1 || *v1 != main + kVtable) return false;
        const auto v2 = read(p + 0x20);
        if (!v2 || *v2 != main + kVtable2) return false;
        const auto a = read(p + kOwner);
        if (!a || !HeapPointer(*a)) return false;
        const auto b = read(*a + 8);
        if (!b || !HeapPointer(*b)) return false;
        const auto owner = read(*b + kOwnerActor);
        return owner && *owner == actor;
    };
    // Breadth first from the actor's components, so the nearest match wins.
    std::vector<std::uint64_t> level;
    std::set<std::uint64_t> seen{actor};
    if (const auto table = read(actor + SignProbe::kActorTable); table && HeapPointer(*table)) {
        for (std::uint64_t at = 0; at < SignProbe::kTableWords * 8; at += 8) {
            const auto c = read(*table + at);
            if (c && HeapPointer(*c) && seen.insert(*c).second) {
                if (is_node(*c)) return *c;
                level.push_back(*c);
            }
        }
    }
    std::size_t looks = 0;
    for (int depth = 0; depth < kNodeDepth && !level.empty(); ++depth) {
        std::vector<std::uint64_t> next;
        for (const std::uint64_t object : level) {
            if (++looks > kNodeLooks) {
                return std::nullopt;
            }
            for (std::uint64_t at = 0; at < kNodeBytes; at += 8) {
                const auto p = read(object + at);
                if (!p || !HeapPointer(*p) || !seen.insert(*p).second) {
                    continue;
                }
                if (is_node(*p)) return *p;
                next.push_back(*p);
            }
        }
        level = std::move(next);
    }
    return std::nullopt;
}

RestScan NodeRestWords(const std::function<std::optional<std::uint64_t>(std::uint64_t)>& read,
                       std::uint64_t node, const proto::Vec3& from, const proto::Vec3& to) {
    RestScan out;
    if (!Finite(from) || !Finite(to) || Distance(from, to) < WorldSync::kBlockStartM) {
        return out;
    }
    const auto float_at = [&](std::uint64_t address) -> std::optional<float> {
        const auto word = read(address & ~std::uint64_t{7});
        if (!word) return std::nullopt;
        const auto b = static_cast<std::uint32_t>((address & 4) != 0 ? *word >> 32 : *word);
        float f = 0.0f;
        std::memcpy(&f, &b, 4);
        return f;
    };
    const auto holds_from = [&](std::uint64_t offset) {
        for (int axis = 0; axis < 3; ++axis) {
            const auto v = float_at(node + offset + 4 * axis);
            if (!v || !std::isfinite(*v) || std::fabs(*v - from[axis]) > kRestMatchM) return false;
        }
        return true;
    };
    // Both or none (review): a node with only one of them at the old place is
    // mid-push here (its target is the next cell); writing just one would make
    // it run from the new place toward a cell figured from the old one.
    const bool target = holds_from(BlockNode::kTarget), start = holds_from(BlockNode::kStart);
    if (target != start) {
        out.busy = true;
        return out;
    }
    if (!target) {
        return out;
    }
    char text[32];
    for (const std::uint64_t offset : {BlockNode::kTarget, BlockNode::kStart}) {
        for (int axis = 0; axis < 3; ++axis) {
            std::uint32_t b = 0;
            std::memcpy(&b, &to[axis], 4);
            out.writes.push_back({node + offset + 4 * axis, b});
        }
        std::snprintf(text, sizeof text, "node+%#llx", static_cast<unsigned long long>(offset));
        out.where.push_back(text);
    }
    return out;
}

} // namespace CoopWorld
