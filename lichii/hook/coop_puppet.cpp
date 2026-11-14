// Puppets (coop_puppet.h). No emulator code.
#include "coop_puppet.h"

#include "coop_camera.h"
#include "coop_colors.h"
#include "coop_local_game.h"
#include "coop_pose.h"

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

// The puppet plays the other player's Ocarina notes (issue 152) only with
// LICHII_COOP_OCARINA_SOUND=1: off by default until the game's own note player
// is found (the notes are in the Ocarina.bars bank, not an SLink user, 2026-11-12).
bool OcarinaSoundOn() {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_OCARINA_SOUND");
        return v != nullptr && std::string_view(v) == "1";
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
        out.push_back({s.number, s.phase, s.block, s.actor, s.actor_name, s.anim_name});
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

// Once a frame, the full-body pose copy (coop_pose.cpp): our skeleton is
// read and each live puppet's is written.
void Director::PoseFrame(Guest& guest, std::uint64_t now_ms) {
    if (!pose_copy_) {
        return;
    }
    std::vector<std::pair<std::uint8_t, std::uint64_t>> puppets;
    for (const auto& slot : slots_) {
        if (slot.phase == Phase::live && slot.actor != 0 && !slot.native) {
            puppets.emplace_back(slot.number, slot.actor);
        }
    }
    CoopPose::OnCalcFrame(guest, input_.main_base, puppets, now_ms);
}

void Director::NewFrame(Guest& guest, std::uint64_t now_ms) {
    ++frames_;
    calls_ = 0;
    first_slot_ = slots_.empty() ? 0 : (first_slot_ + 1) % slots_.size();
    if (pending_ != Pending::none) {
        // The cave did not make the call (its own limit): undo the phase.
        Slot& slot = SlotFor(pending_slot_);
        if (pending_ == Pending::malloc) {
            slot.phase = Phase::no_block;
        } else if (pending_ == Pending::create) {
            slot.phase = Phase::idle;
        } else if (pending_ == Pending::take || pending_ == Pending::drop) {
            // Still creating with the flag set: asked again.
        } else {
            slot.phase = Phase::live;
        }
        // A sound call not made: its step is asked again (note) or the sound is lost
        // (link sound), never left waiting for a Finish that cannot come.
        if (pending_ == Pending::sound) {
            slot.sound.pending = CoopPuppetSound::State::Step::none;
        } else if (pending_ == Pending::link_sound) {
            CoopLinkSound::Dropped(slot.link_sound);
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
                    slot.link_sound.heard.Push(std::move(s.name), s.at_ms);
                }
            }
        }
        t.sounds.clear();
    }
    if (input_.scene != scene_) {
        scene_ = input_.scene;
        scene_since_ms_ = now_ms;
    }
    std::uint32_t live = 0;  // LiveMask(): rebuilt each frame
    if (input_.main_base == 0) {
        live_mask_.store(0, std::memory_order_relaxed);
        return;
    }
    for (auto& slot : slots_) {
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
                    slot.meter.Reset();
                    Vec3 parked = t->position;
                    parked[1] -= kHideDrop;
                    Pose(slot, guest, parked, nullptr, now_ms);
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
    Status(guest, now_ms);
}

void Director::Finish(Guest& guest, std::uint64_t result, std::uint64_t now_ms) {
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
                Say(who + ": create refused; trying again each second");
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
        break;
    case Pending::unequip: {
        const std::size_t i = slot.equip_index;
        if ((result & 0xff) != 0) {
            Say(who + " takes off " + kGearNames[i] + " '" + slot.worn[i] + "' (returned " +
                Hex(result & 0xff) + ")");
            slot.worn[i].clear();
            slot.off_since_ms[i] = 0;
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
    case Pending::move:
        break;
    case Pending::sound:
        CoopPuppetSound::Finish(slot.sound, result, who, [this](const std::string& line) { Say(line); });
        break;
    case Pending::link_sound:
        CoopLinkSound::Finish(slot.link_sound, result, guest, slot.block, who,
                              [this](const std::string& line) { Say(line); }, anim_);
        break;
    case Pending::none:
        break;
    }
    pending_ = Pending::none;
}

std::optional<Call> Director::Next(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    const Target* t = Wanted(slot.number);
    if (t == nullptr) {
        slot.wanted_actor.clear();
    } else if (t->actor != slot.wanted_actor) {
        slot.wanted_actor = t->actor;
        slot.wanted_since_ms = now_ms;
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
        if (t == nullptr || t->actor != slot.actor_name || input_.scene != slot.scene ||
            slot.remake) {
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
        if (auto c = Equip(slot, *t, guest, now_ms)) {
            return c;
        }
        if (auto c = Hold(slot, *t, guest, now_ms)) {
            return c;
        }
        if (matrix_call_ && t->show != Show::hold) {
            if (auto c = Move(slot, guest)) {
                return c;
            }
        }
        if (pose_copy_ && CoopPose::Drives(slot.number, now_ms)) {
            return std::nullopt;  // the pose copy moves it
        }
        return Animate(slot, *t, guest);
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
                " record " + Hex(hand) + " flags " + Hex(flags);
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
                " state " + std::to_string(state) + " " + place;
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
    for (std::size_t i = 0; i < target.gear.size(); ++i) {
        const auto& want = target.gear[i];
        if (want && want->empty() && !slot.worn[i].empty() && !GearIsCarried(i)) {
            // Empty for a moment (a load) is not taken off: kTakeOffMs first.
            if (slot.off_since_ms[i] == 0) {
                slot.off_since_ms[i] = std::max<std::uint64_t>(now_ms, 1);
                continue;
            }
            if (now_ms - slot.off_since_ms[i] < kTakeOffMs) {
                continue;
            }
            if (GearIsArmor(i) && !slot.unequip_refused[i]) {
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
                c.x[1] = kGearKinds[i] - addr::kArmorKindFirst;
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
        c.x[1] = kGearKinds[i];
        c.x[2] = cell;
        c.x[3] = EquipState(kGearKinds[i]);
        c.x[4] = 0xFFFFFFFF;
        c.x[5] = 1;
        c.x[6] = 0;
        pending_ = Pending::equip;
        return c;
    }
    return std::nullopt;
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
        if (s < 0 || slot.worn[i].empty()) {
            continue;
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
    for (std::size_t i = 0; i < kGearSlots.size(); ++i) {
        const int s = kGearSlots[i];
        if (s < 0 || slot.worn[i].empty()) {
            continue;
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
            continue;
        }
        if (drawn && ocarina) {
            Say("P" + std::to_string(slot.number) + " " + kGearNames[i] + " '" + slot.worn[i] +
                "' put away while the Ocarina plays ('" + playing + "')");
        }
        slot.hold_slot = s;
        slot.hold_in_hand = want;
        Call c;
        c.function = input_.main_base + addr::kEquipChangeState;
        c.x[0] = eu;
        c.x[1] = static_cast<std::uint64_t>(s);
        c.x[2] = want ? addr::kEquipStateInHand : addr::kEquipStateAway;
        pending_ = Pending::hold;
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
// - hold: the frame it had when the hold began, every frame.
// - hide: nothing (it is out of sight).
std::optional<Call> Director::Animate(Slot& slot, const Target& target, Guest& guest) {
    if (target.show == Show::hide) {
        return std::nullopt;
    }
    const std::uint64_t controller = Controller(guest, slot.actor);
    if (controller == 0) {
        return std::nullopt;
    }
    const std::string who = "P" + std::to_string(slot.number);
    if (target.show == Show::hold) {
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
void Director::DrawAt(Slot& slot, Guest& guest, const Target& target, std::uint64_t now_ms) {
    const std::string who = "P" + std::to_string(slot.number);
    const bool held = target.show == Show::hold;
    Vec3 at{};
    if (slot.meter_skip) {
        slot.meter_skip = false;  // still the parked spot: start again from the next frame
        slot.meter.Reset();
    } else if (ReadValue(guest, slot.actor + addr::kActorPosition, at)) {
        if (const auto snap = slot.meter.Step(at)) {
            Say(who + " snap " + std::to_string(*snap).substr(0, 5) + " m (" +
                (*snap >= CoopSnap::kWarpM ? std::string("warp") : ShowText(target.show)) + ")");
        }
        slot.smooth.Spawned(at, target.position);  // a new puppet: eased from where the game made it (no spawn pop)
    }
    float jump = 0.0f;
    const Vec3 drawn = slot.smooth.Step(target.position, now_ms, !held, &jump);
    if (jump > 0.0f && slot.smooth.FromSpawn()) {
        // Held (their cutscene) the smoother does not ease: the step is drawn at once then.
        Say(who + (held ? " drawn at once from where it was made, " : " eased from where it was made, ") +
            std::to_string(jump).substr(0, 5) + " m");
    } else if (jump > 0.0f) {
        Say(who + " target jump " + std::to_string(jump).substr(0, 5) + " m (" + ShowText(target.show) +
            (jump >= CoopSnap::kWarpM ? ", warp" : held ? ", at once" : ", eased") + ")");
    }
    Pose(slot, guest, drawn, &target.rotation, now_ms);
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
            r.scene ? std::optional<std::string_view>(*r.scene) : std::nullopt;
        const std::optional<std::string_view> anim =
            r.anim ? std::optional<std::string_view>(r.anim->name) : std::nullopt;
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
        t.actor = ActorFor(r.adult, t.color);
        t.position = r.position;
        if (r.rotation) {
            t.rotation = QuatToMatrix(*r.rotation);
        }
        t.show = show;
        t.why = why;
        t.anim = r.anim;
        t.anim_serial = r.anim_serial;
        t.gear = r.gear;
        t.drawn = r.drawn;
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

bool PackHasGreenPuppets(std::string_view manifest) {
    return manifest.find("\"CoopPuppet_Child_Green\"") != std::string_view::npos &&
           manifest.find("\"CoopPuppet_Adult_Green\"") != std::string_view::npos;
}

bool PackHasTunicPuppets(std::string_view manifest) {
    return manifest.find("\"CoopPuppet_Child_") != std::string_view::npos ||
           manifest.find("\"CoopPuppet_Adult_") != std::string_view::npos;
}

std::string Planner::ActorFor(bool adult, std::optional<std::uint32_t> color) const {
    if (!settings_.actor.empty()) {
        return settings_.actor;
    }
    if (settings_.native_player) {
        return CoopNativePlayer::ActorFor(adult);
    }
    const std::string plain = adult ? "CoopPuppet_Adult" : "CoopPuppet_Child";
    if (settings_.tunics && color) {
        std::string_view tunic = CoopColors::TunicOf(*color);
        if (tunic.empty() && settings_.green_puppet) {
            tunic = "Green";  // the plain puppet wears your own colour (PlannerSettings)
        }
        if (!tunic.empty()) {
            std::string name = plain + '_' + std::string(tunic);
            if (std::find(failed_tunics_.begin(), failed_tunics_.end(), name) == failed_tunics_.end()) {
                return name;
            }
        }
    }
    return plain;
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
std::array<std::optional<Received>, kNumbers> g_anims;  // by player number; guarded
std::array<Gear, kNumbers> g_gear;                         // the same; guarded
std::array<std::optional<std::uint8_t>, kNumbers> g_drawn;  // the same; guarded
std::array<std::optional<std::uint32_t>, kNumbers> g_color; // the same; guarded
std::optional<std::uint32_t> g_own_color;                    // ours; guarded
std::array<CoopPuppetSound::Wanted, kNumbers> g_ocarina;  // the same; guarded (issue 152)
std::array<std::vector<CoopLinkSound::Sound>, kNumbers> g_sounds;  // the same; guarded (LINK_SOUND)

} // namespace

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
        g_color = {};
        g_ocarina = {};
        g_sounds = {};
    } else if (event.kind == EventKind::player_left && event.player < g_anims.size()) {
        std::lock_guard lock(g_anim_mutex);
        g_anims[event.player].reset();
        g_gear[event.player] = {};
        g_drawn[event.player].reset();
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
                         e.sub_tool.value_or("?") + "', hand '" + e.hand.value_or("?") + "'");
        std::lock_guard lock(g_anim_mutex);
        g_gear[event.player] = GearOf(e);
        g_drawn[event.player] = e.drawn;
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
        auto& slot = g_anims[sample->player_number];
        const std::uint32_t serial = slot ? slot->serial + 1 : 1;
        slot = Received{{sample->animation->name, sample->animation->frame,
                         sample->animation->rate},
                        serial};
    }
}

void Configure(PlannerSettings settings, bool verbose, Director::Log log) {
    std::call_once(g_once, [&] {
        g_director = std::make_unique<Director>(std::move(log), verbose, settings.debug_anim);
        g_director->SetPoseCopy(true);
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

std::uint32_t LivePuppets() {
    return g_configured && g_director ? g_director->LiveMask() : 0;
}

std::vector<Remote> Collect(coop::host::Session* session, std::uint64_t server_now_us) {
    std::vector<Remote> remotes;
    if (session != nullptr && server_now_us != 0) {
        session->with_presence([&](coop::presence::Presence& p) {
            for (const std::uint8_t n : p.players()) {
                // The same time as the markers' collect() this frame.
                const auto pose = p.shown(n, server_now_us);
                if (!pose) {
                    continue;
                }
                Remote r;
                r.number = n;
                r.position = pose->position;
                r.rotation = pose->rotation;
                r.fade = pose->fade;
                if (const auto* player = p.player(n)) {
                    r.scene = player->scene();
                    r.status = player->flags();
                    r.adult = player->age() == coop::protocol::Age::adult;
                }
                remotes.push_back(std::move(r));
            }
        });
        std::lock_guard lock(g_anim_mutex);
        for (auto& r : remotes) {
            if (r.number < g_anims.size() && g_anims[r.number]) {
                r.anim = g_anims[r.number]->anim;
                r.anim_serial = g_anims[r.number]->serial;
            }
            if (r.number < g_gear.size()) {
                r.gear = g_gear[r.number];
                r.drawn = g_drawn[r.number];
                r.color = g_color[r.number];
                r.ocarina = g_ocarina[r.number];
                r.sounds = std::exchange(g_sounds[r.number], {});
            }
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
