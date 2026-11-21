// Puppets (coop_puppet.h). No emulator code.
#include "coop_puppet.h"

#include "coop_camera.h"
#include "coop_char_colours.h"
#include "coop_colors.h"
#include "coop_local_game.h"
#include "coop_look_share.h"
#include "coop_pose.h"
#include "coop_lift_player.h"
#include "coop_pvp_native.h"
#include "coop_puppet_own.h"
#include "coop_show_delay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

#include "coop/host/scene.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/presence/presence.hpp"

namespace CoopPuppet {

namespace {
// Native puppets phase 3 (coop_lift_player.h, WatchGamePlaces): off unless the lift switch is on.
std::atomic<bool> g_watch_places{false};
std::atomic<std::uint8_t> g_game_holds{0};
struct KeptPlace {
    Vec3 drawn{};
    bool drawn_known = false;
    std::optional<Vec3> game;
    std::uint64_t at_ms = 0;
};
std::mutex g_places_mutex;
std::map<std::uint8_t, KeptPlace> g_places;
} // namespace
// The head show call's cell (#91) between the gear pointers and the Ocarina notes' cells.
static_assert(block::kHeadOwnerEu >= block::kGearPtr + 8 * kGearCount);
static_assert(block::kHeadOwnerEu + 8 <= CoopPuppetSound::block::kHandle);
namespace {
// The head show call (#91, addr::kShowHeadArmor). LICHII_COOP_MASK_SHOW=0
// leaves it out (a mask is then worn and not drawn, as before 0.11.6).
bool ShowHeadArmor() {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_MASK_SHOW");
        return v == nullptr || std::string_view(v) != "0";
    }();
    return on;
}

// The puppet plays the other player's Ocarina notes (issue 152) on the game's own
// "Ocarina" SLink user, the one the game emits its own notes on (coop_puppet_sound.h,
// OcarinaModuleUser). On by default since ocarina-notes (2026-11-14);
// LICHII_COOP_OCARINA_SOUND=0 turns it off.
bool OcarinaSoundOn() {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_OCARINA_SOUND");
        return v == nullptr || std::string_view(v) != "0";
    }();
    return on;
}

// LICHII_COOP_PUPPET_HOOKSHOT (issue 232): "1" wears a GearIsUnsafe piece on
// a puppet whatever the null page, "0" leaves it off; unset (the default) it
// follows Director::SetNullReadsZero.
std::optional<bool> UnsafeSwitch() {
    static const std::optional<bool> on = []() -> std::optional<bool> {
        const char* v = std::getenv("LICHII_COOP_PUPPET_HOOKSHOT");
        if (v == nullptr || v[0] == '\0') {
            return std::nullopt;
        }
        return std::string_view(v) != "0";
    }();
    return on;
}

template <typename T>
bool ReadValue(Guest& guest, std::uint64_t address, T& out) {
    return guest.Read(address, &out, sizeof(T));
}

template <typename T>
bool WriteValue(Guest& guest, std::uint64_t address, const T& value) {
    return guest.Write(address, &value, sizeof(T));
}

std::string Hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string VecText(const Vec3& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v[0], v[1], v[2]);
    return buf;
}

bool Gone(std::uint32_t state) {
    return state >= 7 && state <= 11;  // as 0x37cca0
}

// The actor's name (actor + 0x260 -> a C string, as coop_fairy.h's
// addr::kActorName) is `name`, or not readable yet (then the vtable decides).
bool NameFits(Guest& guest, std::uint64_t actor, const std::string& name) {
    constexpr std::uint64_t kActorName = 0x260;
    std::uint64_t text = 0;
    if (!ReadValue(guest, actor + kActorName, text) || text == 0) {
        return true;
    }
    std::string read(name.size() + 1, '\0');
    if (!guest.Read(text, read.data(), read.size())) {
        return true;
    }
    return read.compare(0, name.size(), name) == 0 && read[name.size()] == '\0';
}

} // namespace

Mat3 QuatToMatrix(const Quat& q) {
    const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] +
                               double(q[3]) * q[3]);
    if (!(n > 1e-6) || !std::isfinite(n)) {
        return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    }
    const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    return {
        float(1 - 2 * (y * y + z * z)), float(2 * (x * y - z * w)),     float(2 * (x * z + y * w)),
        float(2 * (x * y + z * w)),     float(1 - 2 * (x * x + z * z)), float(2 * (y * z - x * w)),
        float(2 * (x * z - y * w)),     float(2 * (y * z + x * w)),     float(1 - 2 * (x * x + y * y)),
    };
}

const char* PhaseText(Phase phase) {
    switch (phase) {
    case Phase::no_block:
        return "no block";
    case Phase::allocating:
        return "allocating";
    case Phase::idle:
        return "idle";
    case Phase::creating:
        return "creating";
    case Phase::live:
        return "live";
    case Phase::deleting:
        return "deleting";
    }
    return "?";
}

const char* ShowText(Show show) {
    switch (show) {
    case Show::show:
        return "show";
    case Show::hold:
        return "hold";
    case Show::hide:
        return "hide";
    }
    return "?";
}

Director::Director(Log log, bool verbose, bool anim)
    : log_(std::move(log)), verbose_(verbose), anim_(anim) {}

void Director::SetInput(Input input) {
    std::lock_guard lock(input_mutex_);
    next_input_ = std::move(input);
}

void Director::Say(const std::string& text) const {
    if (log_) {
        log_("coop: puppet: " + text);
    }
}

std::vector<Director::SlotView> Director::Slots() const {
    std::vector<SlotView> out;
    for (const auto& s : slots_) {
        out.push_back({s.number, s.phase, s.block, s.actor, s.actor_name, s.anim_name, s.strays.size()});
    }
    return out;
}

std::vector<Director::DressView> Director::DressTargets() const {
    std::vector<DressView> out;
    for (const auto& s : slots_) {
        if (s.phase != Phase::live || s.actor == 0 || s.block == 0) continue;
        const Target* t = Wanted(s.number);
        const bool character = IsCharacterActor(s.actor_name);
        std::optional<std::uint32_t> eyes, hair;   // (0.11.9: the eyes and hair parts of their look, as received)
        if (const auto parts = CoopLookShare::Of(s.number)) {
            for (const auto& p : *parts) {
                if (p.part == kEyesPart) eyes = p.colour;
                if (p.part == kHairPart) hair = p.colour;
            }
        }
        const auto link = CoopCharColours::LinkOnly(!character, eyes, hair);   // (a Link puppet's only)
        eyes = link.eyes, hair = link.hair;
        out.push_back({s.number, s.actor, s.block + block::kDress,
                       t != nullptr && character ? static_cast<std::uint32_t>(t->custom & 0xFFFFFFu) : 0u, character,
                       character && character_apply_ != 0 && character_apply_state_ == 1, s.actor_name, eyes, hair});
    }
    return out;
}

const Target* Director::Wanted(std::uint8_t number) const {
    for (const auto& t : input_.targets) {
        if (t.number == number) {
            return &t;
        }
    }
    return nullptr;
}

Director::Slot& Director::SlotFor(std::uint8_t number) {
    for (auto& s : slots_) {
        if (s.number == number) {
            return s;
        }
    }
    Slot s;
    s.number = number;
    slots_.push_back(s);
    return slots_.back();
}

std::optional<Call> Director::Step(Guest& guest, bool after_call, std::uint64_t result,
                                   std::uint64_t now_ms) {
    if (!after_call) {
        NewFrame(guest, now_ms);
        PoseFrame(guest, now_ms);
    } else if (pending_ != Pending::none) {
        Finish(guest, result, now_ms);
    }
    if (input_.main_base == 0 || calls_ >= kMaxCallsPerFrame) {
        return std::nullopt;
    }
    if (!own_asked_) {
        own_asked_ = true;
        if (auto call = OwnState(guest)) {
            ++calls_;
            return call;
        }
    }
    for (const auto& t : input_.targets) {
        SlotFor(t.number);
    }
    // The frame's call budget goes round the puppets: each frame starts one
    // slot further on, so with many puppets (up to 15) the last ones are not
    // starved by the first ones' calls.
    const std::size_t n = slots_.size();
    for (std::size_t i = 0; i < n; ++i) {
        auto& slot = slots_[(first_slot_ + i) % n];
        if (auto call = Next(slot, guest, now_ms)) {
            pending_slot_ = slot.number;
            ++calls_;
            return call;
        }
    }
    return std::nullopt;
}

// sword-3: the game's own state of the local Link's sword or shield
// (kEquipState, 0xce7ab0(eu, slot)), one slot a frame in turn. The sender's
// request-byte reading misses changes made inside one game frame
// (coop_puppet_own.h); this answer is what it sends instead. No local Link
// (a load, the title): nothing asked, and the stored answers go stale.
std::optional<Call> Director::OwnState(Guest& guest) {
    const std::uint64_t player = CoopPose::FindPlayer(guest, input_.main_base);
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    if (player == 0 || !ReadValue(guest, player + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
        return std::nullopt;
    }
    own_index_ = own_next_;
    own_next_ = (own_next_ + 1) % CoopPuppetOwn::kSlots.size();
    Call c;
    c.function = input_.main_base + addr::kEquipState;
    c.x[0] = eu;
    c.x[1] = CoopPuppetOwn::kSlots[own_index_];
    pending_ = Pending::own_state;
    return c;
}

// Once a frame, the full-body pose copy (coop_pose.cpp): our skeleton is
// read and each live puppet's is written.
void Director::PoseFrame(Guest& guest, std::uint64_t now_ms) {
    if (!pose_copy_) {
        return;
    }
    std::vector<std::pair<std::uint8_t, std::uint64_t>> puppets;
    for (const auto& slot : slots_) {
        // A Character Select character's puppet takes no pose: copied onto one, the sender's pose left it
        // stiff (seen 2026-11-18: Sheik stiff with the copy, moving with the animation commands), so it plays
        // the player's animation commands on its own model, as a puppet without a pose does.
        if (slot.phase == Phase::live && slot.actor != 0 && !slot.native && !IsCharacterActor(slot.actor_name)) {
            puppets.emplace_back(slot.number, slot.actor);
        }
    }
    CoopPose::OnCalcFrame(guest, input_.main_base, puppets, now_ms);
}

void Director::NewFrame(Guest& guest, std::uint64_t now_ms) {
    ++frames_;
    calls_ = 0;
    own_asked_ = false;
    if (pending_ == Pending::own_state) {
        pending_ = Pending::none;  // not made: asked again this frame
    }
    first_slot_ = slots_.empty() ? 0 : (first_slot_ + 1) % slots_.size();
    if (pending_ == Pending::dress && ++dress_not_made_ >= kDressNotMade && character_apply_state_ == 1) {
        character_apply_state_ = 2;
        Say("characters: the Character Select mod's colour routine at main+" + Hex(character_apply_) +
            " could not be called (not a game function of this build): co-op dresses the puppets itself");
    }
    if (pending_ != Pending::none) {
        // The cave did not make the call (its own limit): undo the phase.
        Slot& slot = SlotFor(pending_slot_);
        if (pending_ == Pending::malloc) {
            slot.phase = Phase::no_block;
        } else if (pending_ == Pending::create) {
            slot.phase = Phase::idle;
        } else if (pending_ == Pending::take || pending_ == Pending::drop) {
            // Still creating with the flag set: asked again.
        } else if (pending_ == Pending::stray) {
            // Its next delete is due in kStrayDeleteMs anyway.
        } else {
            slot.phase = Phase::live;
        }
        // A sound call not made: its step is asked again (note) or the sound is lost
        // (link sound), never left waiting for a Finish that cannot come.
        if (pending_ == Pending::sound) {
            slot.sound.pending = CoopPuppetSound::State::Step::none;
        } else if (pending_ == Pending::link_sound) {
            CoopLinkSound::Dropped(slot.link_sound);
        } else if (pending_ == Pending::flame) {
            CoopPuppetFlame::Dropped(slot.flame);
        }
        pending_ = Pending::none;
    }
    {
        std::lock_guard lock(input_mutex_);
        input_ = next_input_;
        for (auto& t : next_input_.targets) {
            t.sounds.clear();  // each received sound is taken once
        }
    }
    for (auto& t : input_.targets) {
        for (auto& slot : slots_) {
            if (slot.number == t.number && !t.sounds.empty()) {
                for (auto& s : t.sounds) {
                    if (OcarinaSoundOn() && s.name.starts_with("Ocarina_")) {
                        continue;  // the OCARINA_NOTE path plays the notes: not twice
                    }
                    if (CoopLinkSound::OwnOnly(s.name)) {
                        continue;  // the sender's own (its low-health beep): an older sender sends it (row 163)
                    }
                    slot.link_sound.heard.Push(std::move(s.name), s.at_ms);
                }
            }
        }
        t.sounds.clear();
    }
    if (input_.scene != scene_) {
        scene_ = input_.scene;
        scene_since_ms_ = now_ms;
        // A load deletes every actor itself: what was a stray is gone or freed
        // memory now, never to be called on.
        for (auto& slot : slots_) {
            if (!slot.strays.empty()) {
                Say("P" + std::to_string(slot.number) + ": " + std::to_string(slot.strays.size()) +
                    " stray actor(s) let go: the scene changed (the load deletes them)");
                slot.strays.clear();
            }
        }
    }
    std::uint32_t live = 0;  // LiveMask(): rebuilt each frame
    std::array<std::uint64_t, coop::protocol::kMaxPlayers + 1> pvp_actors{};  // LivePvpActor(): the same
    std::array<std::uint64_t, coop::protocol::kMaxPlayers + 1> seen_actors{};  // LiveSeenActor(): the same
    if (input_.main_base == 0) {
        live_mask_.store(0, std::memory_order_relaxed);
        for (auto& a : live_pvp_actor_) a.store(0, std::memory_order_relaxed);
        for (auto& a : live_seen_actor_) a.store(0, std::memory_order_relaxed);
        return;
    }
    for (auto& slot : slots_) {
        KeepStrays(slot, guest);
        if (slot.phase != Phase::creating && slot.phase != Phase::live &&
            slot.phase != Phase::deleting) {
            continue;
        }
        if (slot.phase == Phase::creating) {
            Poll(slot, guest, now_ms);
            if (slot.phase != Phase::creating) {
                continue;
            }
        }
        slot.actor = Resolve(slot, guest);
        std::uint32_t state = 0;
        const bool have_state =
            slot.actor != 0 && ReadValue(guest, slot.actor + addr::kActorState, state);
        switch (slot.phase) {
        case Phase::creating:
            if (have_state && state == addr::kStatePresent) {
                Vec3 at{};
                ReadValue(guest, slot.actor + addr::kActorPosition, at);
                Say("P" + std::to_string(slot.number) + " " + slot.actor_name + " spawned after " +
                    std::to_string(now_ms - slot.since_ms) + " ms: request " + Hex(slot.request) +
                    ", actor " + Hex(slot.actor) + ", state 4, at " + VecText(at));
                slot.phase = Phase::live;
                slot.since_ms = now_ms;
                slot.shown = Show::show;  // a new actor stands where it was made
                slot.model_checked = !IsTunicActor(slot.actor_name);
            } else if (now_ms - slot.since_ms > kCreateTimeoutMs) {
                Say("P" + std::to_string(slot.number) + " " + slot.actor_name +
                    " not spawned after " + std::to_string(kCreateTimeoutMs) + " ms (actor " +
                    Hex(slot.actor) + ", state " + (have_state ? std::to_string(state) : "-") +
                    "); trying again in 5 s");
                // Twice for the same actor, and only while no tunic puppet has
                // spawned yet: slow loads in a heavy scene must not cost a
                // working pack its colours.
                if (IsTunicActor(slot.actor_name) && !tunic_proven_ &&
                    ++tunic_timeouts_[slot.actor_name] == 2) {
                    TunicFails(slot.actor_name, "not made twice: the puppet pack has no tunic "
                                                "colours (an older pack? run make-puppet-pack again)");
                }
                // Its actor may still be loading: made later, it would be a
                // second puppet of this player beside the next one. Deleted.
                if (slot.taken != 0) {
                    Abandon(slot, guest, now_ms, "its create timed out");
                } else if (slot.request != 0) {
                    Say("P" + std::to_string(slot.number) + ": request " + Hex(slot.request) +
                        " timed out before it gave an actor (not taken: left to the game)");
                }
                slot.phase = Phase::idle;
                slot.retry_ms = now_ms + 5000;
                slot.take = slot.drop = false;
            }
            break;
        case Phase::live:
            if (!slot.model_checked && have_state && state == addr::kStatePresent &&
                now_ms - slot.since_ms >= kTunicModelCheckMs) {
                // A tunic puppet is the player's model under another project
                // name (tools/puppet_pack/tunic.py): if the engine did not load
                // it, the actor stands without a Player_ model. Plain then.
                slot.model_checked = true;
                // The name comes through the skeleton path (checked on the child),
                // the model object through +0x260 (seen on the adult too): only
                // neither, or a name other than Player_, counts as a failure.
                const auto model = CoopPose::ModelNameOf(guest, slot.actor);
                const std::uint64_t object = CoopColors::ModelOf(guest, slot.actor);
                if (model ? model->rfind("Player_", 0) == 0 : object != 0) {
                    tunic_proven_ = true;
                    Say("P" + std::to_string(slot.number) + " " + slot.actor_name + ": model " +
                        (model ? *model : "object " + Hex(object) + " (name not read)"));
                } else {
                    TunicFails(slot.actor_name, "spawned with " +
                                                    (model ? "model " + *model : std::string("no model")));
                }
            }
            if (!have_state || Gone(state)) {
                Say("P" + std::to_string(slot.number) + " gone (" +
                    (have_state ? "state " + std::to_string(state) : std::string("no actor")) +
                    ")");
                slot.phase = Phase::idle;
                slot.actor = 0;
                slot.ocarina_hand = {};  // its bind block is freed with it
                slot.taken = 0;
                slot.lost_ms = now_ms;  // a load deletes it before the camera goes
            } else if (slot.native) {
                // A real Player (coop_native_player.h): the game runs it; nothing of
                // ours is written into it, it is only watched.
                if (state == addr::kStatePresent) {
                    const auto read = [&guest](std::uint64_t a, void* out, std::size_t size) {
                        return guest.Read(a, out, size);
                    };
                    if (auto line = slot.probe.Step(read, input_.main_base, slot.actor,
                                                    CoopPose::FindPlayer(guest, input_.main_base),
                                                    now_ms)) {
                        Say("P" + std::to_string(slot.number) + " " + *line);
                    }
                }
            } else if (const Target* t = Wanted(slot.number);
                       t != nullptr && state == addr::kStatePresent && t->actor == slot.actor_name &&
                       input_.scene == slot.scene) {
                if (t->show != slot.shown) {
                    if (slot.shown == Show::hide) {
                        slot.meter_skip = true;  // issue 194: it was parked, not drawn
                    }
                    Say("P" + std::to_string(slot.number) + " " + ShowText(slot.shown) + " -> " +
                        ShowText(t->show) + (*t->why ? std::string(" (") + t->why + ")" : std::string()));
                    slot.shown = t->show;
                }
                if (t->show != Show::hide && slot.number < 32) {
                    live |= 1u << slot.number;
                }
                if (t->show != Show::hide && slot.number < seen_actors.size() &&
                    SeenPuppetActor(slot.actor_name, perceived_.load())) {
                    seen_actors[slot.number] = slot.actor;  // row 177: LiveSeenActor
                }
                if (slot.number < pvp_actors.size() && CoopPvpNative::IsPvpActor(slot.actor_name)) {
                    pvp_actors[slot.number] = slot.actor;  // phase 4: LivePvpActor (shown or held, any show)
                }
                // Issue 194 (coop_snap.h): a held puppet (their cutscene) follows their
                // place with its frame frozen, so it is not left behind and does not jump
                // when the cutscene ends; any jump is eased, a snap is logged.
                if (t->color && tint_ > 0.0f) {
                    Tint(slot, guest, *t->color);  // LICHII_COOP_PUPPET_TINT only
                }
                if (t->show == Show::show || t->show == Show::hold) {
                    DrawAt(slot, guest, *t, now_ms);
                } else if (t->show == Show::hide) {
                    slot.smooth.Set(t->position);  // out of sight: comes back where they are
                    slot.follow.Reset();
                    slot.meter.Reset();
                    Vec3 parked = t->position;
                    parked[1] -= kHideDrop;
                    Park(slot, guest, parked, now_ms);
                }
                // Issue 152: the Ocarina in hand while it plays one (coop_ocarina.h),
                // by their own command (free play is DemoOcarina: shown, not held,
                // coop/host/scene.hpp activity_of).
                const std::string their = t->anim ? t->anim->name : std::string();
                if (anim_ && their != slot.their_anim) {
                    Say("P" + std::to_string(slot.number) + " their command '" + their + "'");
                }
                slot.their_anim = their;
                const std::string hand = slot.ocarina_hand.Step(
                    [&guest](std::uint64_t a) -> std::optional<std::uint64_t> {
                        std::uint64_t w = 0;
                        return guest.Read(a, &w, 8) ? std::optional<std::uint64_t>(w) : std::nullopt;
                    },
                    [&guest](std::uint64_t a, std::uint64_t w) { return guest.Write(a, &w, 8); },
                    slot.actor,
                    t->show != Show::hide &&
                        CoopOcarina::HandBind::Playing(their.empty() ? slot.anim_name : their));
                if (!hand.empty()) {
                    Say("P" + std::to_string(slot.number) + " " + hand + " ('" + their + "')");
                }
            }
            break;
        case Phase::deleting:
            if (!have_state || Gone(state) || now_ms - slot.since_ms > 5000) {
                Say("P" + std::to_string(slot.number) + " deleted (" +
                    (have_state ? "state " + std::to_string(state) : std::string("no actor")) +
                    ")");
                if (have_state && !Gone(state)) {
                    Abandon(slot, guest, now_ms, "its delete did not finish");  // asked again until gone
                }
                slot.phase = Phase::idle;
                slot.actor = 0;
                slot.ocarina_hand = {};  // its bind block is freed with it
                slot.taken = 0;
            }
            break;
        default:
            break;
        }
    }
    live_mask_.store(live, std::memory_order_relaxed);
    for (std::size_t n = 0; n < pvp_actors.size(); ++n) live_pvp_actor_[n].store(pvp_actors[n], std::memory_order_relaxed);
    for (std::size_t n = 0; n < seen_actors.size(); ++n) live_seen_actor_[n].store(seen_actors[n], std::memory_order_relaxed);
    Status(guest, now_ms);
}

void Director::Finish(Guest& guest, std::uint64_t result, std::uint64_t now_ms) {
    if (pending_ == Pending::own_state) {
        // Its own clock: the hook's reader compares it with its own (coop_puppet_own.h).
        CoopPuppetOwn::TheDrawn().Set(own_index_, static_cast<std::uint32_t>(result),
                                      CoopPuppetOwn::NowMs());
        pending_ = Pending::none;
        return;
    }
    Slot& slot = SlotFor(pending_slot_);
    const std::string who = "P" + std::to_string(slot.number);
    switch (pending_) {
    case Pending::malloc:
        if (result == 0) {
            Say(who + ": malloc gave nothing; trying again in 5 s");
            slot.phase = Phase::no_block;
            slot.retry_ms = now_ms + 5000;
        } else {
            const std::array<std::uint8_t, block::kSize> zero{};
            guest.Write(result, zero.data(), zero.size());
            slot.block = result;
            slot.phase = Phase::idle;
            Say(who + ": block at " + Hex(result));
        }
        break;
    case Pending::create:
        Say(who + ": create " + slot.actor_name + " requested (returned " +
            Hex(result & 0xffffffff) + "), handle slot " + Hex(slot.handle));
        if ((result & 0xffffffff) != 0) {
            slot.refused = 0;
            break;
        }
        // Refused (seen right after a load deleted the puppet, while the old
        // scene goes): the handle stayed empty and nothing was made, so ask
        // again soon. Should the game have queued a request after all, wait
        // for it as for any create, so there are never two.
        if (std::uint64_t queued = 0; ReadValue(guest, slot.handle, queued) && queued != 0) {
            break;
        }
        ++slot.refused;
        slot.phase = Phase::idle;
        // A tunic actor the game keeps refusing is not in its ActorInfo: an
        // older puppet pack without the colours (Hookshot proof 3: the pairs
        // had the pack from before the tunics, CoopPuppet_Child_Blue refused
        // for good, so no puppet at all). The plain puppet then, as after two
        // create timeouts. Only while no tunic puppet has spawned: refusals
        // during a load must not cost a working pack its colours.
        if (IsTunicActor(slot.actor_name) && !tunic_proven_ && slot.refused == kRefusedTunicFallback) {
            TunicFails(slot.actor_name, "refused " + std::to_string(kRefusedTunicFallback) +
                                            " times: the puppet pack has no tunic colours (an older pack? run "
                                            "make-puppet-pack again)");
            slot.refused = 0;
            slot.retry_ms = now_ms;  // the plain puppet at once
            break;
        }
        if (slot.refused < kRefusedFast) {
            slot.retry_ms = now_ms + kRefusedRetryMs;
            if (slot.refused == 1) {
                Say(who + ": create refused (most often the game does not know " + slot.actor_name +
                    ": no row in the actor table it reads, e.g. a mod with its own RSDB/ActorInfo, or an older "
                    "puppet pack); trying again each second");
            }
        } else {
            slot.retry_ms = now_ms + kRefusedSlowMs;
            if (slot.refused == kRefusedFast) {
                Say(who + ": create refused " + std::to_string(kRefusedFast) +
                    " times; trying again every 5 s");
            }
        }
        break;
    case Pending::take:
        slot.take = false;
        WriteValue(guest, slot.handle, std::uint64_t{0});  // as the game does
        if (result == 0 || !ReadValue(guest, result, slot.vtable)) {
            Say(who + ": request " + Hex(slot.request) + " gave no actor; trying again in 5 s");
            slot.phase = Phase::idle;
            slot.retry_ms = now_ms + 5000;
        } else {
            slot.taken = result;
            Say(who + ": took actor " + Hex(result) + " from request " + Hex(slot.request));
        }
        break;
    case Pending::drop:
        slot.drop = false;
        WriteValue(guest, slot.handle, std::uint64_t{0});
        Say(who + ": request " + Hex(slot.request) + " dropped (state 4); trying again in 5 s");
        slot.phase = Phase::idle;
        slot.retry_ms = now_ms + 5000;
        break;
    case Pending::remove:
        Say(who + ": delete requested (returned " + Hex(result & 0xff) + ")");
        break;
    case Pending::stray:
        for (const auto& s : slot.strays) {
            if (s.actor == stray_actor_ && (s.deletes == 1 || s.deletes % 30 == 0)) {
                Say(who + ": stray actor " + Hex(s.actor) + " delete requested (returned " +
                    Hex(result & 0xff) + ", call " + std::to_string(s.deletes) + ")");
            }
        }
        break;
    case Pending::play:
        slot.anim_name = slot.play_name;
        slot.anim_serial = slot.play_serial;
        slot.frame_to_set = slot.play_frame;
        slot.sync_next = 1;  // then the partial slots, after the frame
        if (anim_) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " from frame %.1f", slot.play_frame);
            Say(who + " plays '" + slot.play_name + "'" + buf + " (returned " +
                Hex(result & 0xff) + ")");
        }
        break;
    case Pending::frame:
        slot.frame_to_set.reset();
        break;
    case Pending::sync:
        if (anim_) {
            Say(who + " slot " + std::to_string(slot.sync_next) + " synced to slot 0 ('" +
                slot.anim_name + "')");
        }
        ++slot.sync_next;
        break;
    case Pending::equip:
        slot.worn[slot.equip_index] = slot.equip_name;
        if (const int s = kGearSlots[slot.equip_index]; s >= 0) {
            // A new piece is worn put away, whatever the old one was.
            slot.in_hand = static_cast<std::uint8_t>(slot.in_hand & ~(1u << s));
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known & ~(1u << s));
        }
        Say(who + " wears " + kGearNames[slot.equip_index] + " '" + slot.equip_name +
            "' (returned " + Hex(result & 0xff) + ")");
        if (slot.equip_index == kGearHead) {
            slot.head_show = (result & 0xff) != 0 && ShowHeadArmor();   // then shown (#91)
            slot.head_hair_back = false;   // (the show call moves the hair for the new piece)
        }
        break;
    case Pending::unequip: {
        const std::size_t i = slot.equip_index;
        if ((result & 0xff) != 0) {
            Say(who + " takes off " + kGearNames[i] + " '" + slot.worn[i] + "' (returned " +
                Hex(result & 0xff) + ")");
            slot.worn[i].clear();
            slot.off_since_ms[i] = 0;
            if (i == kGearHead) {
                slot.head_show = false;
                slot.head_hair_back = slot.head_shown;   // the hair as without a mask (#91)
                slot.head_shown = false;
            }
        } else {
            // Refused: the next Equip makes the puppet again without it.
            Say(who + " takes off " + kGearNames[i] + " '" + slot.worn[i] +
                "': the game's call returned 0");
            slot.unequip_refused[i] = true;
        }
        break;
    }
    case Pending::hold: {
        // Taken when the slot's byte shows the state asked for.
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        std::uint8_t bits = 0;
        const bool read = ReadValue(guest, slot.actor + addr::kActorParts, parts) && parts != 0 &&
                          ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) && eu != 0 &&
                          ReadValue(guest, eu + addr::kEquipSlotBits + slot.hold_slot, bits);
        const std::uint8_t want = slot.hold_in_hand ? addr::kEquipBitInHand : addr::kEquipBitAway;
        const auto bit = static_cast<std::uint8_t>(1u << slot.hold_slot);
        if (read && (bits & want) != 0) {
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known | bit);
            slot.in_hand = static_cast<std::uint8_t>(slot.hold_in_hand ? (slot.in_hand | bit)
                                                                       : (slot.in_hand & ~bit));
            Say(who + " equipment slot " + std::to_string(slot.hold_slot) +
                (slot.hold_in_hand ? " in hand" : " put away"));
        } else {
            slot.hold_retry_ms = now_ms + 250;  // the piece is not ready: ask again
            if (anim_) {
                Say(who + " equipment slot " + std::to_string(slot.hold_slot) +
                    (slot.hold_in_hand ? " in hand" : " put away") + " not taken (byte " +
                    Hex(bits) + ")");
            }
        }
        break;
    }
    case Pending::hold_check: {
        // The game's state of the slot against what was last set: when the
        // puppet's own animation drew or put the piece away since, flip what
        // we hold as set, so the next Hold asks the change again.
        const auto state = static_cast<std::uint32_t>(result);
        const auto bit = static_cast<std::uint8_t>(1u << slot.hold_slot);
        if ((state != addr::kEquipStateInHand && state != addr::kEquipStateAway) ||
            (slot.hold_known & bit) == 0) {
            break;
        }
        if ((state == addr::kEquipStateInHand) == ((slot.in_hand & bit) != 0)) {
            slot.hold_said = static_cast<std::uint8_t>(slot.hold_said & ~bit);  // holds again
            break;
        }
        slot.in_hand = static_cast<std::uint8_t>(slot.in_hand ^ bit);
        // Said once until a check finds it holding: an animation that keeps
        // undoing it would otherwise log twice a second.
        if ((slot.hold_said & bit) == 0) {
            slot.hold_said = static_cast<std::uint8_t>(slot.hold_said | bit);
            Say(who + " equipment slot " + std::to_string(slot.hold_slot) + " is " +
                (state == addr::kEquipStateInHand ? "in hand" : "put away") +
                " in the game, not as set: set again (said once until it holds)");
        }
        break;
    }
    case Pending::move:
        break;
    case Pending::sound:
        CoopPuppetSound::Finish(slot.sound, result, who, [this](const std::string& line) { Say(line); });
        break;
    case Pending::flame:
        CoopPuppetFlame::Finish(slot.flame, result, who, [this](const std::string& line) { Say(line); });
        break;
    case Pending::dress:
        dress_not_made_ = 0;
        slot.dressed = slot.dress_asked;
        slot.dress_ms = now_ms;
        break;
    case Pending::link_sound:
        CoopLinkSound::Finish(slot.link_sound, result, guest, slot.block, who,
                              [this](const std::string& line) { Say(line); }, anim_);
        break;
    case Pending::head_show:
        slot.head_show = false;
        slot.head_shown = true;
        Say(who + " shows head '" + slot.worn[kGearHead] + "' (the game's own show call, " +
            Hex(addr::kShowHeadArmor) + ")");
        break;
    case Pending::head_hair:
        slot.head_hair_back = false;
        Say(who + ": hair put back after the head piece (the game's own take-off, " +
            Hex(addr::kHeadArmorHairBack) + ")");
        break;
    case Pending::own_state:  // taken above
    case Pending::none:
        break;
    }
    pending_ = Pending::none;
}

// Phase 4: player `number`'s copy is lifted by our Link (GameHolds), or its player lifts or carries something
// (a bomb, a pot, another player: its main animation has "lift" or "carry", as phase 3 reads our own): its
// actor is not re-made for a PvP mode change now.
static bool CopyBusy(std::uint8_t number, const Target& t) {
    if (GameHolds() == number) return true;
    // Its player holds something, or is carried (lift-young: the carried pose on every copy of it).
    return t.anim && (CoopLiftPlayer::OurLinkLifts(std::string_view(t.anim->name)) ||
                      t.anim->name == CoopLiftPlayer::kCarriedPose);
}

// The live copy keeps its actor: the wanted one differs only by the _Pvp copy and the copy is busy.
template <class Slot>
static bool PvpHold(const Slot& slot, const Target* t, const std::string& scene) {
    return t != nullptr && scene == slot.scene && !slot.remake &&
           CoopPvpNative::OnlyPvpDiffers(t->actor, slot.actor_name) &&
           !CoopPvpNative::RemakeNow(CoopPvpNative::CopyActor::plain, CoopPvpNative::CopyActor::pvp,
                                     CopyBusy(slot.number, *t));
}

std::optional<Call> Director::Next(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    const Target* t = Wanted(slot.number);
    if (t == nullptr) {
        slot.wanted_actor.clear();
    } else if (t->actor != slot.wanted_actor) {
        slot.wanted_actor = t->actor;
        slot.wanted_since_ms = now_ms;
    }
    if (slot.phase != Phase::allocating) {
        if (auto c = DeleteStray(slot, guest, now_ms)) {
            return c;  // before anything else: never two of one player
        }
    }
    switch (slot.phase) {
    case Phase::no_block:
        if (t != nullptr && now_ms >= slot.retry_ms) {
            Call c;
            c.function = input_.main_base + addr::kMalloc;
            c.x[0] = block::kSize;
            slot.phase = Phase::allocating;
            pending_ = Pending::malloc;
            return c;
        }
        return std::nullopt;
    case Phase::idle:
        // A failed create waits 5 s before the same actor is tried again; a
        // different one (the player's age was read wrong while loading) at once.
        if (t != nullptr && slot.block != 0 && !input_.scene.empty() &&
            (now_ms >= slot.retry_ms || t->actor != slot.actor_name)) {
            if (now_ms - slot.wanted_since_ms < input_.settle_ms) {
                return std::nullopt;  // the age may still change (Input::settle_ms)
            }
            if (!Drawn(slot, now_ms)) {
                if (!slot.wait_said) {
                    slot.wait_said = true;
                    Say("P" + std::to_string(slot.number) + ": waiting for the game to draw");
                }
                return std::nullopt;
            }
            auto c = Create(slot, *t, guest);
            if (c) {
                slot.since_ms = now_ms;
                slot.wait_said = false;
            }
            return c;
        }
        return std::nullopt;
    case Phase::creating:
        if (slot.take || slot.drop) {
            Call c;
            c.x[0] = slot.request;
            if (slot.take) {
                c.function = input_.main_base + addr::kTakeCreated;
                c.x[1] = 1;
                pending_ = Pending::take;
            } else {
                c.function = input_.main_base + addr::kDropCreate;
                pending_ = Pending::drop;
            }
            return c;
        }
        return std::nullopt;
    case Phase::live:
        if (input_.scene.empty() || slot.actor == 0) {
            return std::nullopt;  // loading: wait and see what is left
        }
        if (t != nullptr) {
            slot.missing_ms = 0;
        } else if (input_.scene == slot.scene) {
            if (slot.missing_ms == 0) {
                slot.missing_ms = now_ms;
            }
            if (now_ms - slot.missing_ms < kLeaveGraceMs) {
                return std::nullopt;  // maybe a hiccup: keep it a moment
            }
        }
        // Phase 4: a copy lifted by our Link or holding something keeps its plain or _Pvp actor until that
        // ends; the re-make would drop what it carries (CoopPvpNative::RemakeNow).
        if (!PvpHold(slot, t, input_.scene) && (t == nullptr || t->actor != slot.actor_name ||
                                                 input_.scene != slot.scene || slot.remake)) {
            slot.missing_ms = 0;
            Call c;
            c.function = input_.main_base + addr::kRequestDelete;
            c.x[0] = slot.actor;
            c.x[1] = 0;
            slot.phase = Phase::deleting;
            slot.since_ms = now_ms;
            pending_ = Pending::remove;
            return c;
        }
        if (slot.native) {
            return std::nullopt;  // a real Player: no call of ours drives it
        }
        // The player's Ocarina notes (issue 152), and with LICHII_COOP_DEBUG_ANIM
        // what the puppet's sound user plays (issue 131).
        // Not only in show: A playing is an event on A, so B holds the puppet (still,
        // seen) the whole song; the notes must sound then too.
        if (t->show != Show::hide) {
            const std::string who = "P" + std::to_string(slot.number);
            const auto say = [this](const std::string& line) { Say(line); };
            if (anim_) {
                CoopPuppetSound::LogPlaying(slot.sound, guest, slot.actor, now_ms, who, say);
            }
            if (!OcarinaSoundOn()) {
                // notes are received and logged by the receiver, not played
            } else if (auto c = CoopPuppetSound::Next(slot.sound, t->ocarina, guest,
                                                      input_.main_base, slot.actor, slot.block,
                                                      now_ms, who, say)) {
                pending_ = Pending::sound;
                return c;
            }
            // The player's own sounds (coop_link_sound.h), while no note is being started:
            // the two share the block's asset and text cells.
            if (slot.sound.step == CoopPuppetSound::State::Step::none &&
                slot.sound.pending == CoopPuppetSound::State::Step::none) {
                const std::uint64_t user = CoopPuppetSound::SoundUser(guest, slot.actor);
                if (auto c = CoopLinkSound::Next(
                        slot.link_sound, sound_budget_, guest, input_.main_base, slot.actor, user,
                        slot.block, now_ms,
                        [&guest, user] { return CoopPuppetSound::Playing(guest, user); }, who, say, anim_)) {
                    pending_ = Pending::link_sound;
                    return c;
                }
            }
        }
        if (auto c = Dress(slot, *t, guest, now_ms)) {
            return c;
        }
        if (auto c = Equip(slot, *t, guest, now_ms)) {
            return c;
        }
        if (auto c = Hold(slot, *t, guest, now_ms)) {
            return c;
        }
        // Issue 248: their lit stick's flame, the game's own effect on the puppet's piece
        // (a picture only; coop_puppet_flame.h). Only for a player who sent lit bits.
        if (t->lit || slot.flame.on || slot.flame.next != CoopPuppetFlame::State::Step::none) {
            if (auto c = CoopPuppetFlame::Next(slot.flame, t->lit.value_or(0), t->drawn.value_or(0),
                                               t->show != Show::hide, guest, input_.main_base, slot.actor,
                                               slot.block, "P" + std::to_string(slot.number),
                                               [this](const std::string& line) { Say(line); })) {
                pending_ = Pending::flame;
                return c;
            }
        }
        if (matrix_call_ && t->show != Show::hold &&
            !(g_watch_places.load(std::memory_order_relaxed) && slot.number != 0 &&
              g_game_holds.load(std::memory_order_relaxed) == slot.number)) {
            if (auto c = Move(slot, guest)) {
                return c;
            }
        }
        if (pose_copy_ && !IsCharacterActor(slot.actor_name) && CoopPose::Drives(slot.number, now_ms) &&
            CoopPose::Writes(slot.number, now_ms)) {
            // The pose copy moves it. puppet-anim (#36/#41): what the slot last played is
            // forgotten, so when the copy stops (poses stop, or come for another skeleton)
            // their current command is played again at once, not an old one left on.
            slot.anim_name.clear();
            return std::nullopt;
        }
        return Animate(slot, *t, guest, now_ms);
    default:
        return std::nullopt;
    }
}

// Whether a create now would be drawn. Made while the game loads a scene
// (the scene is readable, the camera is gone), a puppet reaches state 4 but
// never shows until it is made again (Alex's two-player run, 2026-11-09:
// B created P1 at 37.1 s, its loading screen ran 36.8 to 40.2 s).
bool Director::Drawn(const Slot& slot, std::uint64_t now_ms) const {
    if (!input_.camera) {
        return true;  // nothing to go by
    }
    const std::uint64_t since = input_.drawn_since_ms;
    if (since == 0 || now_ms < since || now_ms - since < kDrawnMs) {
        return false;
    }
    const std::uint64_t change = std::max(scene_since_ms_, slot.lost_ms);
    return since > change || now_ms - change >= kSteadyMs;
}

std::optional<Call> Director::Create(Slot& slot, const Target& target, Guest& guest) {
    const std::uint64_t base = input_.main_base;
    std::uint64_t manager = 0;
    std::uint64_t create_default = 0;
    if (!ReadValue(guest, base + addr::kActorManagerPtr, manager) || manager == 0 ||
        !ReadValue(guest, base + addr::kGotCreateDefault, create_default)) {
        return std::nullopt;  // not set up yet
    }
    const std::uint64_t b = slot.block;

    // The create argument as SeqExecuteCreateActor (0x32bca10) fills it in
    // the middle of play, with our position and rotation (docs/puppet.md).
    // The preload walker's form (+0x48 = 0x10001, +0x98 = 0) is only taken
    // up while a scene loads: made later, its request stays in state 2.
    std::array<std::uint8_t, 0x100> arg{};
    const Vec3 scale{1.0f, 1.0f, 1.0f};
    std::memcpy(arg.data() + 0x00, target.position.data(), 12);
    std::memcpy(arg.data() + 0x0c, target.rotation.data(), 36);
    std::memcpy(arg.data() + 0x30, scale.data(), 12);
    std::memcpy(arg.data() + 0x40, &create_default, 8);
    arg[0x98] = 1;
    if (!guest.Write(b + block::kCreateArg, arg.data(), arg.size())) {
        return std::nullopt;
    }

    std::array<char, block::kNameCapacity> text{};
    std::memcpy(text.data(), target.actor.data(),
                std::min<std::size_t>(target.actor.size(), text.size() - 1));
    guest.Write(b + block::kNameText, text.data(), text.size());
    WriteValue(guest, b + block::kName, b + block::kNameText);
    WriteValue(guest, b + block::kName + 8, std::uint64_t{block::kNameCapacity});

    // A fresh handle slot each time, in case the manager still holds the last one.
    slot.handle = b + block::kHandles + 8 * slot.handle_index;
    slot.handle_index = (slot.handle_index + 1) % block::kHandleCount;
    WriteValue(guest, slot.handle, std::uint64_t{0});

    Call c;
    c.function = base + addr::kCreate;
    c.x[0] = manager;
    c.x[1] = b + block::kName;
    c.x[2] = b + block::kCreateArg;
    c.x[3] = slot.handle;
    c.x[4] = 1;
    Abandon(slot, guest, 0, "a new one is made");  // never kept beside the new one
    slot.phase = Phase::creating;
    slot.actor = 0;
    slot.actor_name = target.actor;
    slot.native = CoopNativePlayer::IsPlayerActor(target.actor);
    slot.probe.Reset();
    slot.scene = input_.scene;
    slot.request = 0;
    slot.taken = 0;
    slot.vtable = 0;
    slot.take = slot.drop = false;
    slot.anim_name.clear();
    slot.ocarina_hand = {};  // a new puppet may reuse the old address
    slot.their_anim.clear();
    slot.anim_serial = 0;
    slot.frame_to_set.reset();
    slot.held_frame.reset();
    slot.smooth.Reset();
    slot.meter.Reset();
    slot.follow.Reset();
    slot.meter_skip = false;
    slot.sync_next = 0;
    slot.worn = {};
    slot.in_hand = 0;
    slot.hold_known = 0;
    slot.hold_retry_ms = 0;
    slot.hold_id = {};
    slot.far_since_ms = {};
    slot.rewear_tries = {};
    slot.remake = false;
    slot.off_since_ms = {};
    slot.unequip_refused = {};
    slot.head_show = false;
    slot.head_shown = false;
    slot.head_hair_back = false;
    slot.dressed.reset();   // (a new actor's model has its own colours)
    slot.dress_ms = 0;
    CoopPuppetFlame::Forget(slot.flame);  // its pieces go with the old actor
    pending_ = Pending::create;
    return c;
}

namespace {
// The state the equip call is given (issue 91, puppet-looks): 2 (worn put
// away, as the GameData path wears a sword or a tunic) for every piece but
// the mask, which gets 1. Through the GameData path (state 2) the game drew
// no mask even on the player's own Link (wear-masks 20261110-155536), and the
// call keeps the state per kind (eu + 0x12d4 + 8 * kind: {1, state}) for
// when the piece is made. LICHII_COOP_HEAD_STATE=2 goes back to 2.
std::uint64_t EquipState(std::uint32_t kind) {
    static const std::uint64_t head = [] {
        const char* v = std::getenv("LICHII_COOP_HEAD_STATE");
        return v != nullptr && std::string_view(v) == "2" ? std::uint64_t{2} : std::uint64_t{1};
    }();
    return kind == addr::kArmorKindFirst ? head : std::uint64_t{2};
}

// The eight equipment slots of an actor's EquipmentUser, for the debug log:
// for each slot that holds a piece its id, actor, state, place (height over
// the wearer and distance on the ground), the request byte, and the slot
// record's in-hand byte and flags (u32 at +0xa0: "put away" 0xe34c70 sets
// 0x800, "in hand" 0x6b6820 clears it; read from the dump, not run).
// A piece's turn in its wearer's frame (wearer rotation transposed times the
// piece's), its three rows to one decimal, for the debug log: a held piece
// at the same place but turned another way than on the player's own Link
// shows here (held-sword: a sword in the hand not seen, an Ocarina upside down).
std::string TurnText(Guest& guest, std::uint64_t wearer, std::uint64_t piece) {
    std::array<float, 9> w{};
    std::array<float, 9> p{};
    if (!ReadValue(guest, wearer + addr::kActorRotation, w) ||
        !ReadValue(guest, piece + addr::kActorRotation, p)) {
        return " turn ?";
    }
    std::string text = " turn";
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += w[k * 3 + r] * p[k * 3 + c];
            }
            char n[16];
            std::snprintf(n, sizeof(n), "%s%.1f", c == 0 ? (r == 0 ? " " : "/") : ",",
                          static_cast<double>(v));
            text += n;
        }
    }
    return text;
}

std::string SlotsText(Guest& guest, std::uint64_t actor, std::optional<std::uint8_t> wanted) {
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    if (!ReadValue(guest, actor + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
        return "no equipment user";
    }
    Vec3 own{};
    ReadValue(guest, actor + addr::kActorPosition, own);
    std::string text = "eu " + Hex(eu);
    for (std::uint32_t s = 0; s < 8; ++s) {
        const std::uint64_t link = eu + addr::kEquipSlots + addr::kEquipSlotStride * s;
        std::uint32_t id = 0xFFFFFFFFu;
        std::uint64_t holder = 0;
        std::uint64_t piece = 0;
        std::uint32_t state = 0;
        std::uint8_t request = 0xff;
        std::uint8_t hand = 0xff;
        std::uint32_t flags = 0;
        Vec3 at{};
        ReadValue(guest, link + addr::kEquipSlotId, id);
        ReadValue(guest, eu + addr::kEquipSlotBits + s, request);
        if (id == 0xFFFFFFFFu && (request == 0 || request == 0xff)) {
            continue;  // nothing in this slot
        }
        ReadValue(guest, link + addr::kEquipSlotHolder, holder);
        if (holder != 0) {
            ReadValue(guest, holder + addr::kHolderActor, piece);
        }
        if (piece != 0) {
            ReadValue(guest, piece + addr::kActorState, state);
            ReadValue(guest, piece + addr::kActorPosition, at);
        }
        const std::uint64_t record = eu + addr::kEquipRecords + addr::kEquipRecordStride * s;
        ReadValue(guest, record + addr::kEquipRecordInHand, hand);
        ReadValue(guest, record + addr::kEquipRecordFlags, flags);
        char place[64];
        std::snprintf(place, sizeof(place), "up %.2f off %.2f", static_cast<double>(at[1] - own[1]),
                      static_cast<double>(std::hypot(at[0] - own[0], at[2] - own[2])));
        text += "; slot " + std::to_string(s) + ": id " + Hex(id) + " actor " + Hex(piece) +
                " state " + std::to_string(state) + " " + place + " request " + Hex(request) +
                " record " + Hex(hand) + " flags " + Hex(flags) +
                (piece != 0 ? TurnText(guest, actor, piece) : std::string());
        if (wanted) {
            text += std::string(" want ") + (((*wanted >> s) & 1u) != 0 ? "hand" : "away");
        }
    }
    // The armor links (mask, tunic, boots, gauntlets and the belt pieces):
    // index, id, actor, its state and place.
    for (std::uint32_t a = 0; a < addr::kArmorLinkCount; ++a) {
        const std::uint64_t link = eu + addr::kArmorLinks + addr::kEquipSlotStride * a;
        std::uint32_t id = 0xFFFFFFFFu;
        std::uint64_t holder = 0;
        std::uint64_t piece = 0;
        std::uint32_t state = 0;
        Vec3 at{};
        if (!ReadValue(guest, link + addr::kEquipSlotId, id) || id == 0xFFFFFFFFu) {
            continue;
        }
        ReadValue(guest, link + addr::kEquipSlotHolder, holder);
        if (holder != 0) {
            ReadValue(guest, holder + addr::kHolderActor, piece);
        }
        if (piece != 0) {
            ReadValue(guest, piece + addr::kActorState, state);
            ReadValue(guest, piece + addr::kActorPosition, at);
        }
        char place[64];
        std::snprintf(place, sizeof(place), "up %.2f off %.2f", static_cast<double>(at[1] - own[1]),
                      static_cast<double>(std::hypot(at[0] - own[0], at[2] - own[2])));
        text += "; armor " + std::to_string(a) + ": id " + Hex(id) + " actor " + Hex(piece) +
                " state " + std::to_string(state) + " " + place +
                (piece != 0 && a == 8 ? TurnText(guest, actor, piece) : std::string());
    }
    // Issue 91 (puppet-looks): the equip call's per-kind words (eu + 0x12d4 +
    // 8 * kind, two u32), to tell how the game wears a real mask from the
    // puppet's (the GameData path asks state 1 or 2 per piece).
    text += "; kinds";
    for (std::uint32_t k = 0; k < 18; ++k) {
        std::array<std::uint32_t, 2> words{};
        if (ReadValue(guest, eu + 0x12d4 + 8 * k, words) && (words[0] != 0 || words[1] != 0)) {
            text += " " + std::to_string(k) + ":" + Hex(words[0]) + "/" + Hex(words[1]);
        }
    }
    return text;
}
}  // namespace

bool Director::LeftOff(const std::string& name) const {
    return GearIsUnsafe(name) && !UnsafeSwitch().value_or(null_reads_zero_.load());
}

// Character Select: the mod's colour routine for a character's puppet (its player's Customize options), once
// per change and every kDressAgainMs while any is set; only once its first 16 bytes are the pack's.
std::optional<Call> Director::Dress(Slot& slot, const Target& target, Guest& guest, std::uint64_t now_ms) {
    if (character_apply_ == 0 || character_apply_state_ == 2 || !IsCharacterActor(slot.actor_name)) {
        return std::nullopt;
    }
    const std::uint64_t word = target.custom;
    const bool plain = (word & 0xFFFFFFu) == 0;
    if (word == 0 || (plain && !slot.dressed)) {
        return std::nullopt;   // nothing set and nothing ever put on: the model's own
    }
    if (slot.dressed && *slot.dressed == word && (plain || now_ms - slot.dress_ms < kDressAgainMs)) {
        return std::nullopt;
    }
    if (character_apply_state_ == 0) {
        std::array<std::uint8_t, 16> there{};
        const bool same = guest.Read(input_.main_base + character_apply_, there.data(), there.size()) &&
                          there == character_apply_head_;
        character_apply_state_ = same ? 1 : 2;
        Say(same ? "characters: the Character Select mod's colour routine is at main+" + Hex(character_apply_) +
                       ": a player's Customize options show on their puppet"
                 : "characters: the Character Select mod's colour routine is not at main+" + Hex(character_apply_) +
                       " (another build of the mod than the pack was made with, or its code is not in the game): "
                       "no Customize options on puppets");
        if (!same) {
            return std::nullopt;
        }
    }
    if (!slot.dressed || *slot.dressed != word) {
        Say("P" + std::to_string(slot.number) + " " + slot.actor_name + ": Customize options " +
            Hex(word & 0xFFFFFFu) + " of character " + std::to_string(word >> 32));
    }
    Call c;
    c.function = input_.main_base + character_apply_;
    c.x[0] = slot.actor;
    c.x[1] = word;
    slot.dress_asked = word;
    pending_ = Pending::dress;
    return c;
}

// One equip call for a live puppet whose player's gear differs from what it
// wears, or none. An empty name (nothing in that slot): a carried piece (the
// held item, the tool slots) is only put away, as its player's is; a worn one
// that stays empty for kTakeOffMs is taken off: armor (mask, tunic, boots,
// gauntlets) with the game's own call (addr::kUnequipArmor); a sword or a
// shield, which has no known call, or armor the call refused, by making the
// puppet again without it (slot.remake; issue 91: a mask taken off stayed on).
std::optional<Call> Director::Equip(Slot& slot, const Target& target, Guest& guest,
                                    std::uint64_t now_ms) {
    if (anim_ && now_ms - slot.gear_log_ms >= 2000) {
        slot.gear_log_ms = now_ms;
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        ReadValue(guest, slot.actor + addr::kActorParts, parts);
        if (parts != 0) {
            ReadValue(guest, parts + addr::kPartsEquipmentUser, eu);
        }
        std::string line = "P" + std::to_string(slot.number) + " gear: eu " + Hex(eu);
        for (std::size_t i = 0; i < target.gear.size(); ++i) {
            line += std::string(", ") + kGearNames[i] + " " +
                    (target.gear[i] ? "'" + *target.gear[i] + "'" : std::string("?")) +
                    (slot.worn[i].empty() ? "" : " (worn)");
        }
        Say(line);
        // Every equipment slot as the game has it, and the local player's
        // beside it: what differs between the two is what does not show.
        Say("P" + std::to_string(slot.number) + " slots (puppet " + Hex(slot.actor) +
            "): " + SlotsText(guest, slot.actor, target.drawn));
        if (const std::uint64_t player = CoopPose::FindPlayer(guest, input_.main_base)) {
            Say("P" + std::to_string(slot.number) + " slots (local player " + Hex(player) +
                "): " + SlotsText(guest, player, std::nullopt));
        }
    }
    if (target.show == Show::hide) {
        return std::nullopt;  // parked: a piece made now would stay down there
    }
    if (target.show == Show::show && now_ms >= slot.rewear_ms) {
        LeftBehind(slot, guest, now_ms);
    }
    // A head piece worn is shown, a head piece taken off gives the hair back,
    // with the game's own calls (#91, addr::kShowHeadArmor).
    if (slot.head_show && slot.worn[kGearHead].empty()) {
        slot.head_show = false;
    }
    if (slot.head_show || slot.head_hair_back) {
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        if (ReadValue(guest, slot.actor + addr::kActorParts, parts) && parts != 0 &&
            ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) && eu != 0) {
            Call c;
            if (slot.head_show) {
                if (!WriteValue(guest, slot.block + block::kHeadOwnerEu, eu)) {
                    return std::nullopt;
                }
                c.function = input_.main_base + addr::kShowHeadArmor;
                c.x[0] = slot.block + block::kHeadOwnerEu - addr::kShowHeadArmorEu;
                pending_ = Pending::head_show;
            } else {
                c.function = input_.main_base + addr::kHeadArmorHairBack;
                c.x[0] = eu;
                pending_ = Pending::head_hair;
            }
            return c;
        }
    }
    for (std::size_t i = 0; i < target.gear.size(); ++i) {
        const auto& want = target.gear[i];
        if (want && !want->empty() && LeftOff(*want)) {
            // Issue 232: the game crashed with this piece on a puppet (the
            // Hookshot, in the held slot) where null reads cannot give 0. It stays
            // off there; its player's own Link still shows it.
            //
            // The piece that was in this slot before it was worn stays where
            // the game put it: the slingshot in slot 3 outlived a switch to the
            // Hookshot (daurosk, on PR 233). A slot holds one piece, and the
            // game made that one for us, so the puppet is made again without
            // it: the same way a mask taken off goes (issue 91), since no call
            // is known to take a held piece away.
            if (slot.worn[i] != *want) {
                Say("P" + std::to_string(slot.number) + " " + kGearNames[i] + " '" + *want +
                    "' stays off the puppet: null reads cannot give 0 here, and the game "
                    "crashed with it worn so (issue 232; LICHII_COOP_PUPPET_HOOKSHOT=1 wears it)");
                const bool had_piece = !slot.worn[i].empty();
                const std::string was = slot.worn[i];
                slot.worn[i] = *want;  // (kept as theirs, so the slot is not asked again)
                if (had_piece) {
                    Say("P" + std::to_string(slot.number) + " is made again: '" + was +
                        "' cannot be taken off the slot it keeps");
                    slot.remake = true;
                    return std::nullopt;
                }
            }
            continue;
        }
        if (want && want->empty() && !slot.worn[i].empty() && !GearIsCarried(i) &&
            !LeftOff(slot.worn[i])) {
            // Empty for a moment (a load) is not taken off: kTakeOffMs first.
            if (slot.off_since_ms[i] == 0) {
                slot.off_since_ms[i] = std::max<std::uint64_t>(now_ms, 1);
                continue;
            }
            if (now_ms - slot.off_since_ms[i] < kTakeOffMs) {
                continue;
            }
            if (GearIsArmor(i, slot.worn[i]) && !slot.unequip_refused[i]) {
                std::uint64_t parts = 0;
                std::uint64_t eu = 0;
                if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
                    !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
                    continue;  // not set up (yet): try again next frame, other pieces go on
                }
                slot.equip_index = i;
                Call c;
                c.function = input_.main_base + addr::kUnequipArmor;
                c.x[0] = eu;
                c.x[1] = GearKind(i, slot.worn[i]) - addr::kArmorKindFirst;
                c.x[2] = 0;
                pending_ = Pending::unequip;
                return c;
            }
            Say("P" + std::to_string(slot.number) + " takes off " + kGearNames[i] + " '" +
                slot.worn[i] + "': puppet made again without it");
            slot.remake = true;
            return std::nullopt;
        }
        slot.off_since_ms[i] = 0;
        if (!want || want->empty() || *want == slot.worn[i] ||
            want->size() >= block::kGearCapacity) {
            continue;
        }
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
            !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
            return std::nullopt;  // not set up (yet)
        }
        const std::uint64_t text = slot.block + block::kGearText + block::kGearCapacity * i;
        const std::uint64_t cell = slot.block + block::kGearPtr + 8 * i;
        std::array<char, block::kGearCapacity> name{};
        std::memcpy(name.data(), want->data(), want->size());
        if (!guest.Write(text, name.data(), name.size()) || !WriteValue(guest, cell, text)) {
            return std::nullopt;
        }
        slot.equip_index = i;
        slot.equip_name = *want;
        Call c;
        c.function = input_.main_base + addr::kEquip;
        c.x[0] = eu;
        c.x[1] = GearKind(i, *want);
        c.x[2] = cell;
        c.x[3] = EquipState(GearKind(i, *want));
        c.x[4] = 0xFFFFFFFF;
        c.x[5] = 1;
        c.x[6] = 0;
        pending_ = Pending::equip;
        return c;
    }
    return std::nullopt;
}

// Row 198: a worn copy the game did not make (CopyRefused): said once per copy,
// after kRefusedForMs (a piece just worn may still be the old one).
void Director::RefusedCopy(Slot& slot, Guest& guest, std::size_t i, std::uint64_t piece, std::uint64_t now_ms) {
    std::uint64_t text = 0;
    std::array<char, 96> name{};   // (zeros: the last one ends the name, only size - 1 read)
    if (!ReadValue(guest, piece + addr::kActorName, text) || text == 0 ||
        !guest.Read(text, name.data(), name.size() - 1)) {
        return;
    }
    const std::string made(name.data());
    if (!CopyRefused(slot.worn[i], made)) {
        slot.refused_since_ms[i] = 0;
        return;
    }
    if (slot.refused_since_ms[i] == 0) {
        slot.refused_since_ms[i] = std::max<std::uint64_t>(now_ms, 1);
        return;
    }
    if (now_ms - slot.refused_since_ms[i] < kRefusedForMs || slot.refused_said[i] == slot.worn[i]) {
        return;
    }
    slot.refused_said[i] = slot.worn[i];
    Say("P" + std::to_string(slot.number) + " " + kGearNames[i] + " copy '" + slot.worn[i] +
        "' was not made by the game: the puppet still wears '" + made +
        "' (row 198: its colours are the viewer's own; make the puppet pack again)");
}

// A worn sword or shield far from its puppet does not follow it: the game
// made it while the puppet was parked under the floor (Show::hide), or it
// stayed behind when the puppet came back up. It is worn again, so the game
// makes it anew at the puppet (and the in-hand state is set again for the
// new piece, Hold).
void Director::LeftBehind(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    Vec3 own{};
    if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0 ||
        !ReadValue(guest, slot.actor + addr::kActorPosition, own)) {
        return;
    }
    for (std::size_t i = 0; i < kGearSlots.size(); ++i) {
        const int s = kGearSlots[i];
        if (s < 0 || slot.worn[i].empty() || LeftOff(slot.worn[i])) {
            continue;  // (a piece left off the puppet, issue 232: its slot is not ours)
        }
        if (GearIsCarried(i) && (slot.in_hand & (1u << s)) == 0) {
            continue;  // put away, the game keeps a held item elsewhere (state 6)
        }
        const std::uint64_t link = eu + addr::kEquipSlots + addr::kEquipSlotStride * s;
        std::uint32_t id = 0xFFFFFFFFu;
        std::uint64_t holder = 0;
        std::uint64_t piece = 0;
        std::uint32_t state = 0;
        Vec3 at{};
        if (!ReadValue(guest, link + addr::kEquipSlotId, id) || id == 0xFFFFFFFFu ||
            !ReadValue(guest, link + addr::kEquipSlotHolder, holder) || holder == 0 ||
            !ReadValue(guest, holder + addr::kHolderActor, piece) || piece == 0 ||
            !ReadValue(guest, piece + addr::kActorState, state) || state != addr::kStatePresent ||
            !ReadValue(guest, piece + addr::kActorPosition, at)) {
            continue;
        }
        RefusedCopy(slot, guest, i, piece, now_ms);
        const float far = std::sqrt((at[0] - own[0]) * (at[0] - own[0]) +
                                    (at[1] - own[1]) * (at[1] - own[1]) +
                                    (at[2] - own[2]) * (at[2] - own[2]));
        if (!(far > kGearFar)) {
            slot.far_since_ms[i] = 0;
            slot.rewear_tries[i] = 0;  // where it belongs: a later miss may try again
            continue;
        }
        // Far on one reading may be a piece one frame behind a puppet that
        // jumped: it must still be far a moment later.
        if (slot.far_since_ms[i] == 0) {
            slot.far_since_ms[i] = now_ms;
            continue;
        }
        if (now_ms - slot.far_since_ms[i] < kFarForMs) {
            continue;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(far));
        const std::string what =
            "P" + std::to_string(slot.number) + " " + kGearNames[i] + " '" + slot.worn[i] + "'";
        if (slot.rewear_tries[i] >= kRewearTries) {
            if (slot.rewear_tries[i] == kRewearTries) {
                Say(what + " still left behind (" + text + " away) after " +
                    std::to_string(kRewearTries) + " tries: given up");
                ++slot.rewear_tries[i];
            }
            continue;
        }
        ++slot.rewear_tries[i];
        Say(what + " left behind (" + text + " away): worn again");
        slot.worn[i].clear();
        slot.far_since_ms[i] = 0;
        slot.rewear_ms = now_ms + kRewearMs;
    }
}

// One change of an equipment slot's state for a live puppet: a worn piece
// its player holds in the hand while the puppet's is put away, or the other
// way round. A piece just worn is put away, so only "in hand" is asked then.
// While the puppet plays the Ocarina (their command, as CoopOcarina::HandBind
// follows it) both hands hold the Ocarina, as on the player's own Link: every
// hand-slot piece is put away then, whatever their drawn bits say
// (held-ocarina-b: players saw a shield in the hand of an Ocarina player).
std::optional<Call> Director::Hold(Slot& slot, const Target& target, Guest& guest,
                                   std::uint64_t now_ms) {
    if (!target.drawn || now_ms < slot.hold_retry_ms || target.show == Show::hide) {
        return std::nullopt;
    }
    // The same rule as the hand bind in the live step: their command, else the
    // puppet's last play.
    const std::string their = target.anim ? target.anim->name : std::string();
    const std::string& playing = their.empty() ? slot.anim_name : their;
    const bool ocarina = CoopOcarina::HandBind::Playing(playing);
    std::uint64_t check_eu = 0;
    for (std::size_t i = 0; i < kGearSlots.size(); ++i) {
        const int s = kGearSlots[i];
        if (s < 0 || slot.worn[i].empty() || LeftOff(slot.worn[i])) {
            continue;  // (a piece left off the puppet, issue 232: its slot is not ours)
        }
        const auto bit = static_cast<std::uint8_t>(1u << s);
        const bool drawn = (*target.drawn & bit) != 0;
        const bool want = drawn && !ocarina;
        std::uint64_t parts = 0;
        std::uint64_t eu = 0;
        std::uint32_t id = 0;
        if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
            !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0 ||
            !ReadValue(guest,
                       eu + addr::kEquipSlots + addr::kEquipSlotStride * s + addr::kEquipSlotId,
                       id) ||
            id == 0xFFFFFFFFu) {
            continue;  // the piece is not there (yet)
        }
        check_eu = eu;
        if (id != slot.hold_id[i]) {
            // Another piece than the one last set: the game makes a worn
            // piece again a moment after the equip call (seen 2026-11-09),
            // and the new one is put away whatever the old one was.
            slot.hold_id[i] = id;
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known & ~bit);
            slot.in_hand = static_cast<std::uint8_t>(slot.in_hand & ~bit);
        }
        if ((slot.hold_known & bit) != 0 && want == ((slot.in_hand & bit) != 0)) {
            continue;
        }
        if ((slot.hold_known & bit) == 0 && !want) {
            // Worn put away, as wanted: nothing to ask.
            slot.hold_known = static_cast<std::uint8_t>(slot.hold_known | bit);
            slot.hold_check_ms = now_ms + kHoldCheckMs;
            continue;
        }
        if (drawn && ocarina) {
            Say("P" + std::to_string(slot.number) + " " + kGearNames[i] + " '" + slot.worn[i] +
                "' put away while the Ocarina plays ('" + playing + "')");
        }
        slot.hold_slot = s;
        slot.hold_in_hand = want;
        slot.hold_check_ms = now_ms + kHoldCheckMs;  // let the change settle first
        Call c;
        c.function = input_.main_base + addr::kEquipChangeState;
        c.x[0] = eu;
        c.x[1] = static_cast<std::uint64_t>(s);
        c.x[2] = want ? addr::kEquipStateInHand : addr::kEquipStateAway;
        pending_ = Pending::hold;
        return c;
    }
    // Nothing to change: now and then, one piece set before is checked
    // against the game's own state of its slot (kEquipState), in turn.
    if (check_eu == 0 || slot.hold_check_ms == 0 || now_ms < slot.hold_check_ms) {
        return std::nullopt;
    }
    slot.hold_check_ms = now_ms + kHoldCheckMs;
    for (std::size_t n = 0; n < kGearSlots.size(); ++n) {
        const std::size_t i = (slot.hold_check_next + n) % kGearSlots.size();
        const int s = kGearSlots[i];
        if (s < 0 || slot.worn[i].empty() || (slot.hold_known & (1u << s)) == 0) {
            continue;
        }
        slot.hold_check_next = i + 1;
        slot.hold_slot = s;
        Call c;
        c.function = input_.main_base + addr::kEquipState;
        c.x[0] = check_eu;
        c.x[1] = static_cast<std::uint64_t>(s);
        pending_ = Pending::hold_check;
        return c;
    }
    return std::nullopt;
}

// The game's own setter, once a frame, after Pose wrote the transform.
std::optional<Call> Director::Move(Slot& slot, Guest& guest) {
    (void)guest;
    if (slot.moved_frame == frames_) {
        return std::nullopt;
    }
    slot.moved_frame = frames_;
    if (slot.vtable != input_.main_base + addr::kActorVtable) {
        return std::nullopt;  // not the actor class the setter is known for
    }
    Call c;
    c.function = input_.main_base + addr::kSetMatrix;
    c.x[0] = slot.actor;
    c.x[1] = 0;
    pending_ = Pending::move;
    return c;
}

// The AS controller of an actor; 0 when the chain is not there.
std::uint64_t Director::Controller(Guest& guest, std::uint64_t actor) {
    std::uint64_t p = actor;
    for (const std::uint64_t offset : addr::kControllerPath) {
        if (p == 0 || !ReadValue(guest, p + offset, p)) {
            return 0;
        }
    }
    return p;
}

// Slot 0's frame, when the controller has a slot 0 and its frame is a number.
std::optional<float> Director::Frame(Guest& guest, std::uint64_t controller) {
    std::int32_t count = 0;
    std::uint64_t slots = 0;
    std::uint64_t slot0 = 0;
    float frame = 0.0f;
    if (!ReadValue(guest, controller + addr::kSlotCount, count) || count <= 0 ||
        !ReadValue(guest, controller + addr::kSlots, slots) || slots == 0 ||
        !ReadValue(guest, slots, slot0) || slot0 == 0 ||
        !ReadValue(guest, slot0 + addr::kSlotFrame, frame) || !std::isfinite(frame)) {
        return std::nullopt;
    }
    return frame;
}

std::optional<std::string> Director::Playing(Guest& guest, std::uint64_t controller) {
    std::int32_t count = 0;
    std::uint64_t slots = 0, slot0 = 0, text = 0;
    if (!ReadValue(guest, controller + addr::kSlotCount, count) || count <= 0 ||
        !ReadValue(guest, controller + addr::kSlots, slots) || slots == 0 ||
        !ReadValue(guest, slots, slot0) || slot0 == 0 ||
        !ReadValue(guest, slot0 + addr::kSlotName, text) || text == 0) {
        return std::nullopt;
    }
    // In 16-byte pieces that stay inside a 16-byte block (as PlayerReader reads it).
    std::string name;
    std::uint64_t at = text;
    while (name.size() < block::kAnimCapacity) {
        const std::uint64_t end = (at | 0xf) + 1;
        std::array<char, 16> piece{};
        const std::size_t n = static_cast<std::size_t>(end - at);
        if (!guest.Read(at, piece.data(), n)) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (piece[i] == 0) {
                return name;
            }
            if (piece[i] < 0x20 || piece[i] > 0x7e) {
                return std::nullopt;
            }
            name += piece[i];
        }
        at = end;
    }
    return std::nullopt;  // no end within the buffer's size
}

std::optional<Call> Director::SetFrame(Slot& slot, std::uint64_t controller, float frame) {
    Call c;
    c.function = input_.main_base + addr::kSetAsFrame;
    c.x[0] = controller;
    c.x[1] = 0;  // slot 0
    c.s0 = frame;
    slot.frame_set_in = frames_;
    pending_ = Pending::frame;
    return c;
}

// One animation call for a live puppet in its scene, or none.
// - show: play the received command when it changes, then set its frame;
//   after that, set the frame only when a new sample finds it more than
//   kFrameSlack off, so the game's own playback runs in between.
// - hold: their command played as in show when the puppet has it (read back from its
//   slot 0, puppet-anim); else the frame it had when the hold began, every frame.
// - hide: nothing (it is out of sight).
std::optional<Call> Director::Animate(Slot& slot, const Target& target, Guest& guest,
                                      std::uint64_t now_ms) {
    if (target.show == Show::hide) {
        return std::nullopt;
    }
    const std::uint64_t controller = Controller(guest, slot.actor);
    if (controller == 0) {
        return std::nullopt;
    }
    const std::string who = "P" + std::to_string(slot.number);
    // puppet-anim (#36, the item-get pose): in their cutscene the puppet first tries their
    // command (the event's, e.g. holding up a chest's item) through the same play call. Once
    // played, the puppet's own slot 0 is read back the next frame: when it does not name that
    // command (the puppet has no such animation), the command is not tried again in this
    // hold and the puppet freezes at its frame as before (issue 194).
    bool hold_plays = false;
    if (target.show == Show::hold && target.anim && !target.anim->name.empty() &&
        target.anim->name.size() < block::kAnimCapacity &&
        target.anim->name != slot.cut_refused) {
        const std::string& name = target.anim->name;
        hold_plays = true;
        if (slot.anim_name == name && !slot.frame_to_set && slot.sync_next == 0 &&
            frames_ >= slot.cut_verify_frame) {
            const std::optional<std::string> playing = Playing(guest, controller);
            if (!playing || *playing != name) {
                slot.cut_refused = name;
                slot.anim_name.clear();  // played again once the hold ends
                hold_plays = false;
                Say(who + " has no '" + name + "' (their cutscene, its own is '" +
                    playing.value_or("?") + "'): held at its frame");
            }
        }
    }
    if (target.show != Show::hold) {
        slot.cut_refused.clear();
    }
    if (target.show == Show::hold && !hold_plays) {
        if (!slot.held_frame) {
            slot.held_frame = Frame(guest, controller);
            if (!slot.held_frame) {
                return std::nullopt;
            }
            if (anim_) {
                Say(who + " held");
            }
        }
        if (slot.frame_set_in == frames_) {
            return std::nullopt;  // once a frame
        }
        return SetFrame(slot, controller, *slot.held_frame);
    }
    if (slot.held_frame && anim_) {
        Say(who + " moves again");
    }
    slot.held_frame.reset();
    if (!target.anim || target.anim->name.empty() ||
        target.anim->name.size() >= block::kAnimCapacity) {
        return std::nullopt;
    }
    const Anim& anim = *target.anim;
    // Backstop (puppet-anim, #36/#41, Hot Onion's idea): what the puppet's slot 0 really
    // plays is read now and then; another command than its player's, twice in a row, is
    // played again through the game's own play call (0x2022a0, below) and logged.
    if (target.show == Show::show && !slot.anim_name.empty() && anim.name == slot.anim_name &&
        !slot.frame_to_set && slot.sync_next == 0 && now_ms >= slot.backstop_ms) {
        slot.backstop_ms = now_ms + kBackstopMs;
        const std::optional<std::string> playing = Playing(guest, controller);
        if (playing && *playing != anim.name) {
            if (++slot.backstop_misses >= kBackstopMisses) {
                slot.backstop_misses = 0;
                ++slot.backstops;
                if (slot.backstops <= 3 || slot.backstops % 20 == 0) {
                    Say(who + " backstop: its own animation is '" + *playing + "', not '" +
                        anim.name + "': played again (" + std::to_string(slot.backstops) +
                        " so far)");
                }
                slot.anim_name.clear();  // played again just below
            }
        } else {
            slot.backstop_misses = 0;
        }
    }
    if (anim.name != slot.anim_name) {
        const std::uint64_t text =
            slot.block + block::kAnimText + block::kAnimCapacity * slot.anim_buffer;
        std::array<char, block::kAnimCapacity> name{};
        std::memcpy(name.data(), anim.name.data(), anim.name.size());
        if (!guest.Write(text, name.data(), name.size())) {
            return std::nullopt;
        }
        slot.anim_buffer ^= 1;
        const float rate = std::isfinite(anim.rate) ? anim.rate : 1.0f;
        std::array<std::uint8_t, 0x30> request{};
        const float unset = -1.0f;
        const std::int32_t index = 0;
        std::memcpy(request.data() + 0x00, &slot.actor, 8);
        std::memcpy(request.data() + 0x08, &text, 8);
        std::memcpy(request.data() + 0x10, &index, 4);
        std::memcpy(request.data() + 0x14, &rate, 4);
        std::memcpy(request.data() + 0x18, &unset, 4);
        std::memcpy(request.data() + 0x1c, &unset, 4);
        // +0x20 skip-if-same stays 0: we only play a new name.
        const std::uint64_t at = slot.block + block::kPlayRequest;
        if (!guest.Write(at, request.data(), request.size())) {
            return std::nullopt;
        }
        slot.play_name = anim.name;
        slot.play_frame = std::isfinite(anim.frame) ? anim.frame : 0.0f;
        slot.play_serial = target.anim_serial;
        slot.cut_verify_frame = frames_ + 1;  // a hold reads it back from the next frame
        Call c;
        c.function = input_.main_base + addr::kPlayAs;
        c.x[0] = at;
        pending_ = Pending::play;
        return c;
    }
    if (slot.frame_to_set) {
        return SetFrame(slot, controller, *slot.frame_to_set);
    }
    if (slot.sync_next > 0) {
        std::int32_t count = 0;
        ReadValue(guest, controller + addr::kSlotCount, count);
        if (slot.sync_next < std::min(count, addr::kMaxAsSlots)) {
            Call c;
            c.function = input_.main_base + addr::kSyncAsSlot;
            c.x[0] = slot.actor;
            c.x[1] = 0;
            c.x[2] = static_cast<std::uint64_t>(slot.sync_next);
            pending_ = Pending::sync;
            return c;
        }
        slot.sync_next = 0;
    }
    if (target.anim_serial != slot.anim_serial) {
        slot.anim_serial = target.anim_serial;
        // A still frame on a playing pose is not followed (FrameFollowed): the copy plays on, no snap back.
        const bool followed = FrameFollowed(slot.sample_name, slot.sample_frame, anim);
        slot.sample_name = anim.name;
        slot.sample_frame = anim.frame;
        if (!followed) {
            return std::nullopt;
        }
        const auto now = Frame(guest, controller);
        if (std::isfinite(anim.frame) &&
            (!now || std::fabs(*now - anim.frame) > kFrameSlack)) {
            if (anim_) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " frame %.1f -> %.1f", now.value_or(-1.0f),
                              anim.frame);
                Say(who + " '" + anim.name + "'" + buf);
            }
            return SetFrame(slot, controller, anim.frame);
        }
    }
    return std::nullopt;
}

// Issue 194 (coop_snap.h): a shown or held puppet drawn where its player is. Every jump of
// the place we are handed is logged ("target jump"; eased while shown, at once while held:
// their cutscene's samples move smoothly, so a jump then is a real teleport). The meter reads
// the puppet's place back from the game (what it drew since our last write) and logs each step
// past CoopSnap::kSnapM as a snap ("warp" past kWarpM).
// The puppet's held pieces, each as the actor the game keeps for it.
std::vector<Director::PieceActor> Director::Pieces(Slot& slot, Guest& guest) const {
    std::vector<Director::PieceActor> out;
    std::uint64_t parts = 0;
    std::uint64_t eu = 0;
    if (!ReadValue(guest, slot.actor + addr::kActorParts, parts) || parts == 0 ||
        !ReadValue(guest, parts + addr::kPartsEquipmentUser, eu) || eu == 0) {
        return out;
    }
    for (std::uint32_t s = 0; s < 8; ++s) {
        const std::uint64_t link = eu + addr::kEquipSlots + addr::kEquipSlotStride * s;
        std::uint32_t id = 0xFFFFFFFFu;
        std::uint64_t holder = 0;
        std::uint64_t piece = 0;
        std::uint32_t state = 0;
        if (!ReadValue(guest, link + addr::kEquipSlotId, id) || id == 0xFFFFFFFFu ||
            !ReadValue(guest, link + addr::kEquipSlotHolder, holder) || holder == 0 ||
            !ReadValue(guest, holder + addr::kHolderActor, piece) || piece == 0 ||
            !ReadValue(guest, piece + addr::kActorState, state) || state != addr::kStatePresent) {
            continue;
        }
        out.push_back({piece, state});
    }
    return out;
}

void Director::Park(Slot& slot, Guest& guest, const Vec3& position, std::uint64_t now_ms) {
    // The pieces first: with the puppet already under the floor, one that the game
    // still follows from the old place is what it trips over.
    for (const Director::PieceActor& p : Pieces(slot, guest)) {
        WriteValue(guest, p.actor + addr::kActorPosition, position);
        WriteValue(guest, p.actor + addr::kActorPositionRequest, position);
    }
    Pose(slot, guest, position, nullptr, now_ms);
}

void Director::DrawAt(Slot& slot, Guest& guest, const Target& target, std::uint64_t now_ms) {
    const std::string who = "P" + std::to_string(slot.number);
    const bool held = target.show == Show::hold;
    Vec3 at{};
    std::optional<Vec3> read;
    if (g_watch_places.load(std::memory_order_relaxed) && slot.number != 0) {
        // Native puppets phase 3: where the game had the copy before this write; left to the game while our
        // Link holds it (SetGameHolds): no place, turn or setter call of ours then.
        Vec3 game{};
        std::optional<Vec3> game_read;
        if (!slot.meter_skip && ReadValue(guest, slot.actor + addr::kActorPosition, game)) {
            game_read = game;
        }
        const bool left = g_game_holds.load(std::memory_order_relaxed) == slot.number;
        {
            std::lock_guard lock(g_places_mutex);
            KeptPlace& k = g_places[slot.number];
            k.game = game_read;
            k.at_ms = now_ms;
        }
        if (left) {
            slot.meter.Reset();
            slot.follow.Reset();
            slot.raw_follow.Reset();
            slot.turner.Reset();
            return;
        }
    }
    if (slot.meter_skip) {
        slot.meter_skip = false;  // still the parked spot: start again from the next frame
        slot.meter.Reset();
        slot.steps.Break();
        slot.stillness.Break();
    } else if (ReadValue(guest, slot.actor + addr::kActorPosition, at)) {
        read = at;
        if (const auto snap = slot.meter.Step(at)) {
            Say(who + " snap " + std::to_string(*snap).substr(0, 5) + " m (" +
                (*snap >= CoopSnap::kWarpM ? std::string("warp") : ShowText(target.show)) + ")");
        }
        slot.smooth.Spawned(at, target.position);  // a new puppet: eased from where the game made it (no spawn pop)
    }
    // jitter-b (coop_follow.h): their place taken to this game frame and followed in even
    // steps. Held (their cutscene) it is drawn where the samples say, as before.
    Vec3 place = target.position;
    Vec3 raw = target.position;
    // jitter-2: at the even frame time (coop_follow.h Cadence), so each frame's step is even.
    const double even_ms = cadence_.At(now_ms);
    if (target.velocity && !held) {
        place = slot.follow.Step(target.position, *target.velocity, input_.made_ms, even_ms);
        raw = slot.raw_follow.Step(target.position, *target.velocity, input_.made_ms,
                                   static_cast<double>(now_ms));
    } else {
        slot.follow.Reset();
        slot.raw_follow.Reset();
    }
    // jitter-3: the rotation followed the same way (coop_follow.h Turner).
    Mat3 rotation = target.rotation;
    std::optional<CoopFollow::Quat> turned;  // idle-twitch: for the still meter
    if (target.turn && !held) {
        turned = slot.turner.Step(*target.turn, input_.made_ms, even_ms);
        rotation = QuatToMatrix(*turned);
    } else {
        slot.turner.Reset();
    }
    float jump = 0.0f;
    const Vec3 drawn = slot.smooth.Step(place, now_ms, !held, &jump);
    if (jump > 0.0f && slot.smooth.FromSpawn()) {
        // Held (their cutscene) the smoother does not ease: the step is drawn at once then.
        Say(who + (held ? " drawn at once from where it was made, " : " eased from where it was made, ") +
            std::to_string(jump).substr(0, 5) + " m");
    } else if (jump > 0.0f) {
        Say(who + " target jump " + std::to_string(jump).substr(0, 5) + " m (" + ShowText(target.show) +
            (jump >= CoopSnap::kWarpM ? ", warp" : held ? ", at once" : ", eased") + ")");
    }
    if (jump > 0.0f) {
        slot.steps.Break();
        slot.raw_steps.Break();
        slot.stillness.Break();
    }
    // idle-twitch's proof line: while they stand still, how much the puppet still moves ('drawn')
    // and how much the place handed in does ('handed in': their sender's and presence's noise).
    const bool still = target.velocity && !held &&
                       std::hypot((*target.velocity)[0], (*target.velocity)[1], (*target.velocity)[2]) <
                           CoopFollow::kIdleSpeed;
    if (const auto r = slot.stillness.Step(still, target.position, target.turn, drawn, turned, now_ms)) {
        constexpr float kDeg = 57.29578f;
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      " still: %d frames (%d held), drawn max step %.1f mm (%.1f mm/s), max turn %.2f deg; "
                      "handed in max step %.1f mm (%.1f mm/s), max turn %.2f deg",
                      r->frames, r->held, r->drawn_step * 1000.0f, r->drawn_path * 1000.0f, r->drawn_turn * kDeg,
                      r->in_step * 1000.0f, r->in_path * 1000.0f, r->in_turn * kDeg);
        Say(who + buf);
    }
    const auto before = slot.raw_steps.Step(std::nullopt, raw, now_ms);
    if (const auto r = slot.steps.Step(read, drawn, now_ms)) {
        // jitter-2's proof line: 'uneven' near 0 is even steps; 'game moved' is what the game
        // itself moved it between our writes (a second writer).
        char raw_text[16] = "n/a";  // the raw-time steps did not report this frame
        if (before) {
            std::snprintf(raw_text, sizeof(raw_text), "%.2f", before->uneven);
        }
        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      " steps: %d frames (%d moving), mean %.3f m, uneven %.2f (worst %.2f; raw frame time "
                      "%s), frame %.1f ms, game moved %.3f m (max %.3f)",
                      r->frames, r->moving, r->mean_step, r->uneven, r->uneven_max, raw_text,
                      cadence_.PeriodMs(), r->game_moved, r->game_moved_max);
        Say(who + buf);
    }
    Pose(slot, guest, drawn, &rotation, now_ms);
    if (g_watch_places.load(std::memory_order_relaxed) && slot.number != 0) {
        std::lock_guard lock(g_places_mutex);
        KeptPlace& k = g_places[slot.number];
        k.drawn = drawn;
        k.drawn_known = true;
    }
}

// The puppet's pose: +0x31c/+0x328 and the engine's pending transform
// (+0x358/+0x364), which the actor's calc applies over +0x31c. With the
// debug anim log, once a second, what the game left there since the last
// write, so a writer that undoes ours shows up.
void Director::Tint(Slot& slot, Guest& guest, std::uint32_t color) {
    const auto result = CoopColors::Apply(guest, slot.actor, CoopColors::TintOf(color, tint_));
    if (result == CoopColors::Applied::no_model) {
        return;  // not made yet; asked again next frame
    }
    if (slot.tint_actor != slot.actor || slot.tint_color != color) {
        slot.tint_actor = slot.actor;
        slot.tint_color = color;
        char strength[16];
        std::snprintf(strength, sizeof(strength), "%.2f", tint_);
        Say("P" + std::to_string(slot.number) + " colour " + CoopColors::Text(color) +
            " (strength " + strength + ") on model " +
            Hex(CoopColors::ModelOf(guest, slot.actor)) + " of " + Hex(slot.actor));
    }
}

void Director::Pose(Slot& slot, Guest& guest, const Vec3& position, const Mat3* rotation,
                    std::uint64_t now_ms) {
    if (anim_ && now_ms - slot.pose_log_ms >= 1000) {
        slot.pose_log_ms = now_ms;
        Vec3 at{};
        Vec3 asked{};
        ReadValue(guest, slot.actor + addr::kActorPosition, at);
        ReadValue(guest, slot.actor + addr::kActorPositionRequest, asked);
        Say("P" + std::to_string(slot.number) + " pose: at " + VecText(at) + ", request " +
            VecText(asked) + ", now to " + VecText(position));
    }
    WriteValue(guest, slot.actor + addr::kActorPosition, position);
    WriteValue(guest, slot.actor + addr::kActorPositionRequest, position);
    if (rotation != nullptr) {
        WriteValue(guest, slot.actor + addr::kActorRotation, *rotation);
        WriteValue(guest, slot.actor + addr::kActorRotationRequest, *rotation);
    }
}

// The slot's taken actor becomes a stray: no longer the puppet, deleted until gone.
// Only while it is still that actor (Resolve: the vtable it was taken with) and not
// being deleted already (state 7-11): a freed or reused address is only let go.
void Director::Abandon(Slot& slot, Guest& guest, std::uint64_t now_ms, const char* why) {
    const std::uint64_t actor = slot.taken;
    if (actor == 0 || slot.vtable == 0) {
        return;
    }
    std::uint32_t state = 0;
    const bool alive = Resolve(slot, guest) == actor &&
                       ReadValue(guest, actor + addr::kActorState, state) && !Gone(state);
    slot.taken = 0;
    slot.actor = 0;
    if (!alive) {
        return;
    }
    for (const auto& s : slot.strays) {
        if (s.actor == actor) {
            return;
        }
    }
    slot.strays.push_back({actor, slot.vtable, slot.actor_name, now_ms, 0});
    Say("P" + std::to_string(slot.number) + ": actor " + Hex(actor) + " (" + slot.actor_name +
        ") is a stray (" + why + "): deleted with the game's call until it is gone");
}

// Why the stray must not be called on any more; empty while it may be deleted.
std::string Director::StrayCheck(const Slot::Stray& s, Guest& guest) const {
    std::uint64_t vtable = 0;
    std::uint32_t state = 0;
    if (!ReadValue(guest, s.actor, vtable) || vtable != s.vtable) {
        return "freed or reused";
    }
    if (!ReadValue(guest, s.actor + addr::kActorState, state)) {
        return "not readable";
    }
    if (Gone(state)) {
        return "state " + std::to_string(state);
    }
    if (!NameFits(guest, s.actor, s.name)) {
        return "another actor's name";
    }
    for (const auto& other : slots_) {
        if (other.taken == s.actor) {
            return "a puppet again";
        }
    }
    if (s.deletes >= kStrayMaxDeletes) {
        return "given up: the game kept it through " + std::to_string(s.deletes) + " delete calls";
    }
    return {};
}

void Director::KeepStrays(Slot& slot, Guest& guest) {
    std::erase_if(slot.strays, [&](const Slot::Stray& s) {
        const std::string why = StrayCheck(s, guest);
        if (why.empty()) {
            return false;
        }
        Say("P" + std::to_string(slot.number) + ": stray actor " + Hex(s.actor) + " let go (" + why +
            ", " + std::to_string(s.deletes) + " delete calls)");
        return true;
    });
}

// At most one stray delete a frame over all slots, each one checked right before
// its call (another slot may have taken the address since the frame began).
std::optional<Call> Director::DeleteStray(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    if (stray_frame_ == frames_) {
        return std::nullopt;
    }
    for (auto& s : slot.strays) {
        if (now_ms < s.next_ms || !StrayCheck(s, guest).empty()) {
            continue;  // not due, or let go next frame
        }
        s.next_ms = now_ms + kStrayDeleteMs;
        ++s.deletes;
        stray_actor_ = s.actor;
        stray_frame_ = frames_;
        Call c;
        c.function = input_.main_base + addr::kRequestDelete;
        c.x[0] = s.actor;
        c.x[1] = 0;
        pending_ = Pending::stray;
        return c;
    }
    return std::nullopt;
}

// The actor taken from the request, while its vtable is the one it had then
// (an actor the engine freed and reused is not written to).
std::uint64_t Director::Resolve(Slot& slot, Guest& guest) {
    std::uint64_t vtable = 0;
    if (slot.taken == 0 || !ReadValue(guest, slot.taken, vtable) || vtable != slot.vtable) {
        return 0;
    }
    return slot.taken;
}

// While creating, before the actor is taken: what the request says.
void Director::Poll(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    std::uint64_t request = 0;
    std::uint32_t word = 0;
    if (slot.taken != 0 || slot.take || slot.drop || slot.handle == 0 ||
        !ReadValue(guest, slot.handle, request) || request == 0 ||
        !ReadValue(guest, request, word)) {
        return;
    }
    const std::uint32_t state = word & 0xffff;
    slot.request = request;
    if (state == addr::kRequestReady) {
        slot.take = true;
    } else if (state == addr::kRequestDrop) {
        slot.drop = true;
    } else if (state == 5 || state == 6) {
        Say("P" + std::to_string(slot.number) + " " + slot.actor_name + ": request " +
            Hex(request) + " failed (state " + std::to_string(state) + "); trying again in 5 s");
        WriteValue(guest, slot.handle, std::uint64_t{0});
        slot.phase = Phase::idle;
        slot.retry_ms = now_ms + 5000;
    }
}

void Director::Status(Guest& guest, std::uint64_t now_ms) {
    if (!verbose_) {
        return;
    }
    if (status_ms_ == 0) {
        status_ms_ = now_ms;
        frames_at_status_ = frames_;
        return;
    }
    if (now_ms - status_ms_ < 1000) {
        return;
    }
    const double seconds = double(now_ms - status_ms_) / 1000.0;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f cave frames/s, scene '%s', %zu wanted",
                  double(frames_ - frames_at_status_) / seconds, input_.scene.c_str(),
                  input_.targets.size());
    Say(buf);
    status_ms_ = now_ms;
    frames_at_status_ = frames_;
    for (const auto& slot : slots_) {
        std::string line = "P" + std::to_string(slot.number) + " " + PhaseText(slot.phase);
        if (slot.actor != 0) {
            std::uint32_t state = 0;
            Vec3 at{};
            ReadValue(guest, slot.actor + addr::kActorState, state);
            ReadValue(guest, slot.actor + addr::kActorPosition, at);
            line += " actor " + Hex(slot.actor) + " state " + std::to_string(state) + " at " +
                    VecText(at);
        }
        if (!slot.anim_name.empty()) {
            const auto frame = slot.actor ? Frame(guest, Controller(guest, slot.actor))
                                          : std::nullopt;
            char buf[32];
            std::snprintf(buf, sizeof(buf), " frame %.1f", frame.value_or(-1.0f));
            line += " plays '" + slot.anim_name + "'" + buf;
        }
        if (const Target* t = Wanted(slot.number)) {
            line += ", wanted at " + VecText(t->position);
            line += t->show == Show::show ? " (show)" : t->show == Show::hold ? " (hold)" : " (hide)";
        }
        // While it is being made, what the handle holds, to see it fill in.
        if (std::uint64_t h = 0; slot.phase == Phase::creating && slot.handle != 0 &&
                                 ReadValue(guest, slot.handle, h) && h != 0) {
            std::array<std::uint64_t, 8> words{};
            guest.Read(h, words.data(), sizeof(words));
            line += "; handle holds " + Hex(h) + ":";
            for (auto w : words) {
                line += " " + Hex(w);
            }
        }
        Say(line);
    }
}

Gear GearOf(const coop::protocol::Equipment& e) {
    Gear gear{e.weapon, e.shield,     e.head,         e.armor, e.boots,   e.held,
              e.hand,   e.sub_weapon, e.third_weapon, e.tool,  e.sub_tool};
    // The Ocarina and its belt holder (issue 152): "Item_Ocarina_Fairy" and
    // "Item_Ocarina_Fairy_Holder" (or _Time); none while the player has none.
    if (e.ocarina && e.ocarina->starts_with("Item_Ocarina_")) {
        gear[kGearOcarinaHolder] = *e.ocarina + "_Holder";
        gear[kGearOcarina] = e.ocarina;
    }
    // What is really in the sword hand (a Deku stick) goes before the
    // equipped sword's name; an empty slot leaves the flag's name.
    if (e.weapon_slot && !e.weapon_slot->empty()) {
        gear[0] = e.weapon_slot;
    }
    // Only a mask is worn on the head (row 197): a 0.11.8 player still sends
    // the Lens of Truth's base `Armor_GlassTruth` from its flag, whose adult,
    // unposed glasses drew spikes and a ground streak on the puppet. Nothing
    // worn there, so a puppet that wears it takes it off.
    if (gear[kGearHead] && !gear[kGearHead]->empty() &&
        !coop::protocol::is_drawn_head(*gear[kGearHead])) {
        gear[kGearHead] = std::string{};
    }
    return gear;
}

Input Planner::Plan(std::uint64_t main_base, const std::optional<std::string>& scene,
                    const std::optional<Vec3>& position, bool local_event,
                    const std::vector<Remote>& remotes, std::uint64_t now_ms,
                    const Gear& own_gear,
                    std::optional<std::uint8_t> own_drawn,
                    std::optional<std::uint32_t> own_color) {
    Input in;
    if (main_base == 0) {
        return in;
    }
    in.main_base = main_base;
    in.scene = scene.value_or("");
    if (in.scene != scene_) {
        scene_ = in.scene;
        scene_since_ms_ = now_ms;
        debug_spot_.reset();
    }
    if (in.scene.empty()) {
        return in;
    }
    for (const auto& r : remotes) {
        if (r.number == 0 || !(r.fade > 0.0f)) {
            continue;
        }
        const std::optional<std::string_view> theirs =
            coop::host::view_of(r.scene);
        const std::optional<std::string_view> anim =
            coop::host::name_of(r.anim);
        Show show = Show::show;
        const char* why = "";
        switch (coop::host::visibility(in.scene, local_event, theirs, r.status, anim)) {
        case coop::host::Visibility::hide:
            if (!coop::host::same_place(in.scene, theirs)) {
                continue;  // not here: no puppet (one made is deleted)
            }
            show = Show::hide;  // our cutscene: kept, out of sight
            why = "our cutscene";
            break;
        case coop::host::Visibility::ride:
            // Issue 199: shown in its riding animation; CoopRide draws a
            // copy of the horse under it.
            why = "riding";
            break;
        case coop::host::Visibility::hold:
            show = Show::hold;
            why = "their cutscene";
            break;
        case coop::host::Visibility::show:
            if (coop::host::activity_of(anim) == coop::host::Activity::ocarina) {
                why = "ocarina";
            }
            break;
        }
        Target t;
        t.number = r.number;
        t.color = r.color ? r.color : coop::protocol::slot_color(r.number);
        t.actor = ActorFor(r.adult, t.color, r.character, r.tunic_copy);
        if (r.number >= 1 && r.number < 32 && (PvpCopies() & (1u << r.number)) != 0) {
            t.actor = CoopPvpNative::PvpActor(t.actor);  // PvP on real bodies (phase 4)
        }
        if (r.character != 0 && IsCharacterActor(t.actor)) {
            t.custom = (static_cast<std::uint64_t>(r.character) << 32) | (r.custom & 0xFFFFFFu);
        }
        t.position = r.position;
        t.velocity = r.velocity;
        if (r.rotation) {
            t.rotation = QuatToMatrix(*r.rotation);
            t.turn = r.rotation;
        }
        t.show = show;
        t.why = why;
        t.anim = r.anim;
        t.anim_serial = r.anim_serial;
        t.gear = r.gear;
        const bool own_copy = r.look_copy && *r.look_copy >= 1 && *r.look_copy <= settings_.look_copies;
        if (own_copy) {
            DressInLook(t.gear, *r.look_copy, settings_.look_items);
        }
        DressInBlades(t.gear, r.number, settings_.blade_items, settings_.blade_copies);
        // (row 198: no copy of its own (more players than slots): the neutral copy, never the game's item, whose
        // texture your own colour is drawn over; a sword already on its player's own wearer copy keeps it)
        if (!own_copy && settings_.look_neutral > settings_.look_copies) {
            DressInLook(t.gear, settings_.look_neutral, settings_.look_items);
        }
        t.drawn = r.drawn;
        t.lit = r.lit;
        t.ocarina = r.ocarina;
        if (show != Show::hide && CoopLinkSound::InHearing(position, r.position)) {
            t.sounds = r.sounds;  // too far, or out of sight: not heard
        }
        in.targets.push_back(std::move(t));
    }
    if (settings_.debug && position && now_ms - scene_since_ms_ >= settings_.debug_delay_ms) {
        if (!debug_spot_) {
            debug_spot_ = Vec3{(*position)[0] + settings_.debug_offset[0],
                               (*position)[1] + settings_.debug_offset[1],
                               (*position)[2] + settings_.debug_offset[2]};
        }
        Target t;
        t.number = 0;
        t.actor = ActorFor(false, own_color);
        t.position = *debug_spot_;
        t.gear = own_gear;
        t.drawn = own_drawn;
        t.color = own_color;  // the test puppet wears your own colour, if you chose one
        in.targets.push_back(std::move(t));
    }
    return in;
}

std::vector<std::string> PackQuietScenes(std::string_view manifest) {
    // "quiet_scenes": ["...", ...], as build.py writes it (json.dumps).
    std::vector<std::string> out;
    const auto at = manifest.find("\"quiet_scenes\"");
    if (at == std::string_view::npos) {
        return out;
    }
    const auto lb = manifest.find_first_not_of(" \t\r\n:", at + 14);
    if (lb == std::string_view::npos || manifest[lb] != '[') {
        return out;
    }
    const auto rb = manifest.find(']', lb);
    if (rb == std::string_view::npos) {
        return out;
    }
    std::string_view list = manifest.substr(lb + 1, rb - lb - 1);
    while (true) {
        const auto q0 = list.find('"');
        const auto q1 = q0 == std::string_view::npos ? q0 : list.find('"', q0 + 1);
        if (q1 == std::string_view::npos) {
            break;
        }
        if (q1 > q0 + 1) {
            out.emplace_back(list.substr(q0 + 1, q1 - q0 - 1));
        }
        list.remove_prefix(q1 + 1);
    }
    return out;
}

bool SeenPuppetActor(std::string_view actor, bool perceived) {
    return perceived && actor.starts_with("CoopPuppet_") && actor.find("Quiet") == std::string_view::npos;
}

bool PackPerception(std::string_view manifest) {
    // "perception": true, as build.py writes it (json.dumps) at the top level, AFTER "actors": every actor entry
    // has its own "perception" too (null, or its emitter {"param", "size"}), so the first key is an actor's and never
    // the pack's (until row 177's review every pack read as without perception). The pack's is the last key whose
    // value is a bool.
    bool on = false;
    for (auto at = manifest.find("\"perception\""); at != std::string_view::npos;
         at = manifest.find("\"perception\"", at + 12)) {
        const auto v = manifest.find_first_not_of(" \t\r\n:", at + 12);
        if (v == std::string_view::npos) {
            break;
        }
        if (manifest.substr(v, 4) == "true") {
            on = true;
        } else if (manifest.substr(v, 5) == "false") {
            on = false;
        }
    }
    return on;
}

std::string QuietActor(std::string_view actor) {
    for (const std::string_view plain : {std::string_view("CoopPuppet_Child"), std::string_view("CoopPuppet_Adult")}) {
        if (actor.substr(0, plain.size()) == plain &&
            (actor.size() == plain.size() || actor[plain.size()] == '_')) {
            return std::string(plain) + "Quiet" + std::string(actor.substr(plain.size()));
        }
    }
    return std::string(actor);
}

std::string LookItemName(std::string_view item, std::size_t copy) {
    return std::string(item) + "_Look" + std::to_string(copy);
}

void DressInLook(Gear& gear, std::size_t copy, const std::vector<std::string>& items) {
    for (auto& piece : gear) {
        if (piece && std::find(items.begin(), items.end(), *piece) != items.end()) {
            piece = LookItemName(*piece, copy);
        }
    }
}

std::string BladeItemName(std::string_view item, std::size_t number) {
    return std::string(item) + "_Blade" + std::to_string(number);
}

void DressInBlades(Gear& gear, std::size_t number, const std::vector<std::string>& items, std::size_t copies) {
    if (number < 1 || number > copies) {
        return;
    }
    for (auto& piece : gear) {
        if (piece && std::find(items.begin(), items.end(), *piece) != items.end()) {
            piece = BladeItemName(*piece, number);
        }
    }
}

LookItems PackLookItems(std::string_view manifest, std::string_view key) {
    // "look_items": {"slots": N, "copies": 2N, "actors": ["...", ...], "files": [...]}, as
    // build.py writes it (json.dumps); null for a pack without them. "blade_items" the same,
    // without "slots".
    LookItems out;
    const std::string quoted = "\"" + std::string(key) + "\"";
    const auto at = manifest.find(quoted);
    if (at == std::string_view::npos) {
        return out;
    }
    const auto open = manifest.find_first_not_of(" \t\r\n:", at + quoted.size());
    if (open == std::string_view::npos || manifest[open] != '{') {
        return out;   // null: none
    }
    const auto close = manifest.find('}', open);
    if (close == std::string_view::npos) {
        return out;
    }
    const std::string_view body = manifest.substr(open, close - open);
    if (const auto s = body.find("\"neutral\""); s != std::string_view::npos) {
        std::size_t i = body.find_first_not_of(" \t\r\n:", s + 9);
        while (i < body.size() && body[i] >= '0' && body[i] <= '9' && out.neutral < 100) {
            out.neutral = out.neutral * 10 + static_cast<std::size_t>(body[i] - '0');
            ++i;
        }
    }
    std::size_t copies = 0;
    if (const auto s = body.find("\"copies\""); s != std::string_view::npos) {
        std::size_t i = body.find_first_not_of(" \t\r\n:", s + 8);
        while (i < body.size() && body[i] >= '0' && body[i] <= '9' && copies < 100) {
            copies = copies * 10 + static_cast<std::size_t>(body[i] - '0');
            ++i;
        }
    }
    const auto a = body.find("\"actors\"");
    const auto lb = a == std::string_view::npos ? a : body.find('[', a);
    const auto rb = lb == std::string_view::npos ? lb : body.find(']', lb);
    if (rb == std::string_view::npos || copies == 0) {
        return out;
    }
    std::string_view list = body.substr(lb + 1, rb - lb - 1);
    while (true) {
        const auto q0 = list.find('"');
        if (q0 == std::string_view::npos) {
            break;
        }
        const auto q1 = list.find('"', q0 + 1);
        if (q1 == std::string_view::npos) {
            break;
        }
        if (q1 > q0 + 1) {
            out.actors.emplace_back(list.substr(q0 + 1, q1 - q0 - 1));
        }
        list.remove_prefix(q1 + 1);
    }
    if (!out.actors.empty()) {
        out.copies = copies;
    }
    return out;
}

std::string CharacterActor(bool adult, std::uint16_t file) {
    if (file > 999) {
        return {};
    }
    std::string name = adult ? "CoopPuppet_Adult_C" : "CoopPuppet_Child_C";
    name += static_cast<char>('0' + file / 100);
    name += static_cast<char>('0' + file / 10 % 10);
    name += static_cast<char>('0' + file % 10);
    return name;
}

std::map<std::uint16_t, std::uint16_t> PackCharacterNumbers(std::string_view manifest) {
    // "characters": {"mod": "...", "numbers": {"<file>": <number>, ...}, "variants": {...}}, as build.py writes it
    // (json.dumps); null for a pack without them.
    std::map<std::uint16_t, std::uint16_t> out;
    const auto at = manifest.find("\"characters\"");
    if (at == std::string_view::npos) {
        return out;
    }
    const auto open = manifest.find_first_not_of(" \t\r\n:", at + 12);
    if (open == std::string_view::npos || manifest[open] != '{') {
        return out;   // null: none
    }
    const auto key = manifest.find("\"numbers\"", open);
    const auto lb = key == std::string_view::npos ? key : manifest.find('{', key);
    const auto rb = lb == std::string_view::npos ? lb : manifest.find('}', lb);
    if (rb == std::string_view::npos || manifest.substr(open, key - open).find('}') != std::string_view::npos) {
        return out;
    }
    std::string_view body = manifest.substr(lb + 1, rb - lb - 1);
    const auto number = [](std::string_view& text, unsigned& value) {
        const auto i = text.find_first_of("0123456789");
        if (i == std::string_view::npos) {
            return false;
        }
        text.remove_prefix(i);
        value = 0;
        while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
            value = value * 10 + static_cast<unsigned>(text.front() - '0');
            text.remove_prefix(1);
            if (value > 0xFFFF) {
                return false;
            }
        }
        return true;
    };
    unsigned file = 0;
    unsigned stored = 0;
    while (number(body, file) && number(body, stored)) {
        if (file <= 999 && stored != 0) {
            out[static_cast<std::uint16_t>(stored)] = static_cast<std::uint16_t>(file);
        }
    }
    return out;
}

bool IsCharacterActor(std::string_view actor) {
    for (const std::string_view head : {std::string_view("CoopPuppet_Child"), std::string_view("CoopPuppet_Adult")}) {
        if (actor.substr(0, head.size()) != head) {
            continue;
        }
        std::string_view rest = actor.substr(head.size());
        if (rest.substr(0, 5) == "Quiet") {
            rest.remove_prefix(5);
        }
        // (its `_Pvp` copy, CoopPvpNative::PvpActor, is the same character)
        if (rest.size() > 4 && rest.substr(rest.size() - 4) == "_Pvp") {
            rest.remove_suffix(4);
        }
        return rest.size() == 5 && rest[0] == '_' && rest[1] == 'C' && rest[2] >= '0' && rest[2] <= '9' &&
               rest[3] >= '0' && rest[3] <= '9' && rest[4] >= '0' && rest[4] <= '9';
    }
    return false;
}

std::vector<std::string> PackCharacterActors(std::string_view manifest) {
    std::vector<std::string> out;
    constexpr std::string_view kHead = "\"CoopPuppet_";
    for (std::size_t at = manifest.find(kHead); at != std::string_view::npos; at = manifest.find(kHead, at + 1)) {
        const std::size_t end = manifest.find('"', at + 1);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string_view name = manifest.substr(at + 1, end - at - 1);
        if (IsCharacterActor(name)) {
            out.emplace_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<std::pair<std::uint64_t, std::array<std::uint8_t, 16>>> PackCharacterColours(
    std::string_view manifest) {
    const std::size_t at = manifest.find("\"colours\"");
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t end = manifest.find('}', at);
    const std::string_view body = manifest.substr(at, end == std::string_view::npos ? end : end - at);
    std::size_t o = body.find("\"offset\"");
    std::size_t h = body.find("\"head\"");
    if (o == std::string_view::npos || h == std::string_view::npos) {
        return std::nullopt;
    }
    o = body.find(':', o);
    std::uint64_t offset = 0;
    bool digits = false;
    for (o = o == std::string_view::npos ? body.size() : o + 1; o < body.size(); ++o) {
        if (body[o] >= '0' && body[o] <= '9') {
            offset = offset * 10 + static_cast<std::uint64_t>(body[o] - '0');
            digits = true;
            if (offset > 0xFFFFFFFFull) {
                return std::nullopt;
            }
        } else if (digits || (body[o] != ' ' && body[o] != '\n' && body[o] != '\r' && body[o] != '\t')) {
            break;
        }
    }
    h = body.find(':', h);
    h = h == std::string_view::npos ? h : body.find('"', h);
    std::array<std::uint8_t, 16> head{};
    if (!digits || offset == 0 || h == std::string_view::npos || h + 33 >= body.size() || body[h + 33] != '"') {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        const char ch = body[h + 1 + i];
        const int v = ch >= '0' && ch <= '9'   ? ch - '0'
                      : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                      : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                               : -1;
        if (v < 0) {
            return std::nullopt;
        }
        head[i / 2] = static_cast<std::uint8_t>(head[i / 2] << 4 | v);
    }
    return std::make_pair(offset, head);
}

bool PackHasGreenPuppets(std::string_view manifest) {
    return manifest.find("\"CoopPuppet_Child_Green\"") != std::string_view::npos &&
           manifest.find("\"CoopPuppet_Adult_Green\"") != std::string_view::npos;
}

bool PackHasTunicPuppets(std::string_view manifest) {
    return manifest.find("\"CoopPuppet_Child_") != std::string_view::npos ||
           manifest.find("\"CoopPuppet_Adult_") != std::string_view::npos;
}

std::string Planner::ActorFor(bool adult, std::optional<std::uint32_t> color, std::uint16_t character,
                              const std::optional<std::string>& tunic_copy) const {
    if (!settings_.actor.empty()) {
        return settings_.actor;
    }
    if (settings_.native_player) {
        return CoopNativePlayer::ActorFor(adult);
    }
    // Issue 150: in a scene with stealth guards, the quiet copy (PlannerSettings::quiet_scenes).
    // Everywhere when the runtime cannot make a perceived puppet safe (PlannerSettings::quiet_everywhere).
    const bool quiet = settings_.quiet_everywhere ||
                       std::find(settings_.quiet_scenes.begin(), settings_.quiet_scenes.end(), scene_) !=
                           settings_.quiet_scenes.end();
    const auto named = [quiet](std::string name) { return quiet ? QuietActor(name) : name; };
    const std::string plain = adult ? "CoopPuppet_Adult" : "CoopPuppet_Child";
    const auto works = [this](const std::string& name) {
        return std::find(failed_tunics_.begin(), failed_tunics_.end(), name) == failed_tunics_.end();
    };
    // The character they picked in the Character Select mod, when this pack has its puppet (and it did not
    // fail, as a colour's): before the colours, since the character's own model has its own clothes.
    // The number is the sender's pick, the mod's id of the character; this pack's own map gives its file.
    const auto file = settings_.character_files.find(character);
    const std::string picked =
        file == settings_.character_files.end() ? std::string{} : CharacterActor(adult, file->second);
    if (!picked.empty()) {
        std::string name = named(picked);
        if (std::binary_search(settings_.character_actors.begin(), settings_.character_actors.end(), name) &&
            works(name)) {
            return name;
        }
    }
    if (settings_.tunics && (color || tunic_copy)) {
        // (0.11.9: the copy given to this player, repainted in their exact colour; else the one nearest)
        const std::string_view tunic = tunic_copy ? std::string_view(*tunic_copy) : CoopColors::TunicOf(*color);
        if (!tunic.empty()) {
            std::string name = named(plain + '_' + std::string(tunic));
            if (works(name)) {
                return name;
            }
        }
    }
    // The plain puppet wears your own colour (PlannerSettings::green_puppet):
    // a green player, tunics off, or a colour that failed get the green one.
    if (settings_.green_puppet) {
        std::string green = named(plain + "_Green");
        if (works(green)) {
            return green;
        }
    }
    return named(plain);
}

void Director::TunicFails(const std::string& actor, const std::string& why) {
    std::lock_guard lock(tunic_mutex_);
    if (tunic_failed_.insert(actor).second) {
        Say(actor + " " + why + "; that colour shows as the plain puppet from now on");
    }
}

namespace {

std::once_flag g_once;
std::atomic<bool> g_configured = false;
std::unique_ptr<Director> g_director;
std::unique_ptr<Planner> g_planner;  // the present thread only

// Each remote player's newest animation, from the network thread.
struct Received {
    Anim anim;
    std::uint32_t serial = 0;
};
std::mutex g_anim_mutex;
// One entry per player number, 0 (unused) to kMaxPlayers (up to 16 players).
constexpr std::size_t kNumbers = std::size_t{coop::protocol::kMaxPlayers} + 1;
// jitter-3: by their sample time, taken at presence's shown time (coop_follow.h Delayed).
std::array<CoopFollow::Delayed<Received>, kNumbers> g_anims;  // by player number; guarded
std::array<std::uint32_t, kNumbers> g_anim_serials{};         // the same; guarded
std::array<Gear, kNumbers> g_gear;                         // the same; guarded
std::array<std::optional<std::uint8_t>, kNumbers> g_drawn;  // the same; guarded
std::array<std::optional<std::uint8_t>, kNumbers> g_lit;    // the same; guarded (issue 248)
std::array<std::optional<std::uint32_t>, kNumbers> g_color; // the same; guarded
std::optional<std::uint32_t> g_own_color;                    // ours; guarded
std::array<CoopPuppetSound::Wanted, kNumbers> g_ocarina;  // the same; guarded (issue 152)
std::array<std::vector<CoopLinkSound::Sound>, kNumbers> g_sounds;  // the same; guarded (LINK_SOUND)

} // namespace

namespace {
std::mutex g_tunic_copies_mutex;
std::map<std::uint8_t, std::string> g_tunic_copies;   // guarded by g_tunic_copies_mutex
}  // namespace

void SetTunicCopies(std::map<std::uint8_t, std::string> copies) {
    std::lock_guard lock(g_tunic_copies_mutex);
    g_tunic_copies = std::move(copies);
}

std::optional<std::string> TunicCopyOf(std::uint8_t number) {
    std::lock_guard lock(g_tunic_copies_mutex);
    const auto it = g_tunic_copies.find(number);
    return it == g_tunic_copies.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::optional<std::uint32_t> PlayerColor(std::uint8_t number) {
    std::lock_guard lock(g_anim_mutex);
    return number < g_color.size() ? g_color[number] : std::nullopt;
}

void SetOwnColor(std::optional<std::uint32_t> rgb) {
    std::lock_guard lock(g_anim_mutex);
    g_own_color = rgb;
}

std::optional<std::uint32_t> OwnColor() {
    std::lock_guard lock(g_anim_mutex);
    return g_own_color;
}

std::uint32_t NameColor(std::uint8_t number, std::uint8_t self) {
    return (number == self ? OwnColor() : PlayerColor(number))
        .value_or(coop::protocol::slot_color(number));
}

void OnEvent(const coop::net::Event& event) {
    if (!g_configured) {
        return;
    }
    using coop::net::EventKind;
    if (event.kind == EventKind::disconnected) {
        std::lock_guard lock(g_anim_mutex);
        g_anims = {};
        g_gear = {};
        g_drawn = {};
        g_lit = {};
        g_color = {};
        g_ocarina = {};
        g_sounds = {};
    } else if (event.kind == EventKind::player_left && event.player < g_anims.size()) {
        std::lock_guard lock(g_anim_mutex);
        g_anims[event.player].Clear();
        g_gear[event.player] = {};
        g_drawn[event.player].reset();
        g_lit[event.player].reset();
        g_color[event.player].reset();
        g_ocarina[event.player] = {};
        g_sounds[event.player].clear();
    } else if (event.kind == EventKind::player_equip && event.player != 0 &&
               event.player < g_gear.size()) {
        const auto& e = event.equipment;
        g_director->Note("P" + std::to_string(event.player) + " PLAYER_EQUIP: weapon '" +
                         e.weapon.value_or("?") + "', shield '" + e.shield.value_or("?") + "', held '" +
                         e.held.value_or("?") + "', slots 0 '" + e.weapon_slot.value_or("?") +
                         "' 1 '" + e.sub_weapon.value_or("?") + "' 2 '" +
                         e.third_weapon.value_or("?") + "' 5 '" + e.tool.value_or("?") + "' 6 '" +
                         e.sub_tool.value_or("?") + "', hand '" + e.hand.value_or("?") + "'" +
                         (e.lit ? ", lit " + std::to_string(*e.lit) : std::string()));
        std::lock_guard lock(g_anim_mutex);
        g_gear[event.player] = GearOf(e);
        g_drawn[event.player] = e.drawn;
        g_lit[event.player] = e.lit;
        g_color[event.player] = e.color;
    } else if (const auto* note = std::get_if<coop::protocol::OcarinaNote>(&event.message);
               event.kind == EventKind::message && note != nullptr) {
        if (note->player_number == 0 || note->player_number >= g_ocarina.size()) {
            return;
        }
        const auto now_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        std::lock_guard lock(g_anim_mutex);
        auto& w = g_ocarina[note->player_number];
        w = CoopPuppetSound::Wanted{note->note, w.serial + 1, now_ms};
    } else if (const auto* sound = std::get_if<coop::protocol::LinkSound>(&event.message);
               event.kind == EventKind::message && sound != nullptr) {
        if (sound->player_number == 0 || sound->player_number >= g_sounds.size()) {
            return;
        }
        const auto now_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        std::lock_guard lock(g_anim_mutex);
        auto& queue = g_sounds[sound->player_number];
        if (queue.size() >= CoopLinkSound::kHeardMax) {
            queue.erase(queue.begin());  // the newest are kept
        }
        queue.push_back(CoopLinkSound::Sound{sound->name, now_ms});
    } else if (event.kind == EventKind::message) {
        const auto* sample = std::get_if<coop::protocol::PlayerSample>(&event.message);
        if (sample == nullptr || !sample->animation || sample->player_number == 0 ||
            sample->player_number >= g_anims.size()) {
            return;
        }
        std::lock_guard lock(g_anim_mutex);
        const std::uint32_t serial = ++g_anim_serials[sample->player_number];
        g_anims[sample->player_number].Push(
            sample->time.server_time_us,
            Received{{sample->animation->name, sample->animation->frame, sample->animation->rate}, serial});
    }
}

void Configure(PlannerSettings settings, bool verbose, Director::Log log) {
    std::call_once(g_once, [&] {
        g_director = std::make_unique<Director>(std::move(log), verbose, settings.debug_anim);
        g_director->SetPoseCopy(true);
        g_director->SetCharacterApply(settings.character_apply, settings.character_apply_head);
        g_director->SetNullReadsZero(settings.null_reads_zero);
        g_director->SetPerceived(settings.perceived && !settings.quiet_everywhere);
        // docs/puppet.md, "Colour". LICHII_COOP_PUPPET_TINT: 0 to 1, or off.
        // The model tint darkens the whole Link (reports puppet-colors-2): off
        // unless asked for; the colour is the tunic (PlannerSettings::tunics).
        const char* tint = std::getenv("LICHII_COOP_PUPPET_TINT");
        g_director->SetTint(tint != nullptr ? CoopColors::StrengthFrom(tint) : 0.0f);
        g_planner = std::make_unique<Planner>(std::move(settings));
        g_configured = true;
    });
}

Director& TheDirector() {
    Configure({}, false, {});
    return *g_director;
}

void WatchGamePlaces(bool on) {
    g_watch_places.store(on);
    if (!on) {
        g_game_holds.store(0);
        std::lock_guard lock(g_places_mutex);
        g_places.clear();
    }
}

void SetGameHolds(std::uint8_t number) {
    g_game_holds.store(g_watch_places.load() ? number : 0);
}

std::uint8_t GameHolds() {
    return g_game_holds.load();
}

namespace {
std::atomic<std::uint32_t> g_pvp_copies{0};
}  // namespace

void SetPvpCopies(std::uint32_t mask) {
    g_pvp_copies.store(mask, std::memory_order_relaxed);
}

std::uint32_t PvpCopies() {
    return g_pvp_copies.load(std::memory_order_relaxed);
}

std::vector<GamePlace> GamePlaces() {
    std::vector<GamePlace> out;
    if (!g_watch_places.load()) {
        return out;
    }
    const std::uint64_t now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              std::chrono::steady_clock::now().time_since_epoch())
                                                              .count());
    std::lock_guard lock(g_places_mutex);
    for (const auto& [number, k] : g_places) {
        if (k.drawn_known && now - k.at_ms <= 250) {
            out.push_back(GamePlace{number, k.drawn, k.game});
        }
    }
    return out;
}

std::uint32_t LivePuppets() {
    return g_configured && g_director ? g_director->LiveMask() : 0;
}

std::vector<Director::DressView> DressTargets() {
    return g_configured && g_director ? g_director->DressTargets() : std::vector<Director::DressView>{};
}

std::vector<Remote> Collect(coop::host::Session* session, std::uint64_t server_now_us) {
    std::vector<Remote> remotes;
    if (session != nullptr && server_now_us != 0) {
        // jitter-3: the animation at the place's own time; report 37: that time is each
        // player's own (its delay follows its samples' ages: coop/presence show_delay.hpp).
        std::array<std::uint64_t, coop::protocol::kMaxPlayers + 1> show_us{};
        session->with_presence([&](coop::presence::Presence& p) {
            for (const std::uint8_t n : p.players()) {
                // The same time as the markers' collect() this frame.
                const auto pose = p.shown(n, server_now_us);
                if (!pose) {
                    continue;
                }
                if (n < show_us.size()) {
                    show_us[n] = pose->at_us;
                    CoopShowDelay::Set(n, server_now_us > pose->at_us ? server_now_us - pose->at_us : 0);
                }
                Remote r;
                r.number = n;
                r.position = pose->position;
                r.velocity = pose->velocity;
                r.rotation = pose->rotation;
                r.fade = pose->fade;
                if (const auto* player = p.player(n)) {
                    r.scene = player->scene();
                    r.status = player->flags();
                    r.adult = player->age() == coop::protocol::Age::adult;
                    r.young = player->age() == coop::protocol::Age::child;
                }
                remotes.push_back(std::move(r));
            }
        });
        std::lock_guard lock(g_anim_mutex);
        for (auto& r : remotes) {
            if (r.number < g_anims.size()) {
                if (const Received* a = g_anims[r.number].At(show_us[r.number])) {
                    r.anim = a->anim;
                    r.anim_serial = a->serial;
                }
            }
            if (r.number < g_gear.size()) {
                r.gear = g_gear[r.number];
                r.drawn = g_drawn[r.number];
                r.lit = g_lit[r.number];
                r.color = g_color[r.number];
                r.tunic_copy = TunicCopyOf(r.number);
                r.ocarina = g_ocarina[r.number];
                r.sounds = std::exchange(g_sounds[r.number], {});
            }
            r.look_copy = CoopLookShare::CopyOf(r.number);
            r.character = CoopLookShare::CharacterOf(r.number);
            r.custom = CoopLookShare::CustomOf(r.number);
        }
    }
    return remotes;
}

void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us) {
    if (!g_configured) {
        return;  // puppets are off
    }
    std::vector<Remote> remotes = Collect(session, server_now_us);
    Gear own_gear;
    std::optional<std::uint8_t> own_drawn;
    std::optional<std::uint32_t> own_color;
    if (session != nullptr) {
        if (const auto& e = session->sampler().last_equipment()) {
            own_gear = GearOf(*e);
            own_drawn = e->drawn;
            own_color = e->color;
        }
    }
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    g_planner->SetFailedTunics(g_director->FailedTunics());
    auto input = g_planner->Plan(main_base, scene, position, local_event, remotes, now_ms, own_gear, own_drawn, own_color);
    if (main_base != 0) CoopLocalGame::AddTargets(input);   // local co-op's players (coop_local_game.h)
    input.made_ms = now_ms;
    StampDrawing(input, now_ms);
    g_director->SetInput(std::move(input));
}

void StampDrawing(Input& input, std::uint64_t now_ms) {
    // The camera as the hook's markers judge it (kCameraStale there). Called
    // by the puppets and the fairies in the same frame: the second call finds
    // the run already begun and gives the same answer.
    static bool camera_seen = false;           // the present thread only
    static std::uint64_t drawn_since_ms = 0;
    const auto camera = CoopCamera::Latest();
    const bool drawn = camera && std::chrono::steady_clock::now() - camera->at <
                                     std::chrono::milliseconds(250);
    if (!drawn) {
        drawn_since_ms = 0;
    } else if (drawn_since_ms == 0) {
        drawn_since_ms = now_ms;
    }
    camera_seen = camera_seen || drawn;
    input.camera = camera_seen;
    input.drawn_since_ms = drawn_since_ms;
    input.settle_ms = 1000;
}

} // namespace CoopPuppet
