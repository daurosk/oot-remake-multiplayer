// Puppets (coop_puppet.h). No emulator code.
#include "coop_puppet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "coop/host/session.hpp"
#include "coop/presence/presence.hpp"

namespace CoopPuppet {
namespace {

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

Director::Director(Log log, bool verbose) : log_(std::move(log)), verbose_(verbose) {}

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
        out.push_back({s.number, s.phase, s.block, s.actor, s.actor_name});
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
    } else if (pending_ != Pending::none) {
        Finish(guest, result, now_ms);
    }
    if (input_.main_base == 0 || calls_ >= kMaxCallsPerFrame) {
        return std::nullopt;
    }
    for (const auto& t : input_.targets) {
        SlotFor(t.number);
    }
    for (auto& slot : slots_) {
        if (auto call = Next(slot, guest, now_ms)) {
            pending_slot_ = slot.number;
            ++calls_;
            return call;
        }
    }
    return std::nullopt;
}

void Director::NewFrame(Guest& guest, std::uint64_t now_ms) {
    ++frames_;
    calls_ = 0;
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
        pending_ = Pending::none;
    }
    {
        std::lock_guard lock(input_mutex_);
        input_ = next_input_;
    }
    if (input_.main_base == 0) {
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
            } else if (now_ms - slot.since_ms > kCreateTimeoutMs) {
                Say("P" + std::to_string(slot.number) + " " + slot.actor_name +
                    " not spawned after " + std::to_string(kCreateTimeoutMs) + " ms (actor " +
                    Hex(slot.actor) + ", state " + (have_state ? std::to_string(state) : "-") +
                    "); trying again in 5 s");
                slot.phase = Phase::idle;
                slot.retry_ms = now_ms + 5000;
                slot.take = slot.drop = false;
            }
            break;
        case Phase::live:
            if (!have_state || Gone(state)) {
                Say("P" + std::to_string(slot.number) + " gone (" +
                    (have_state ? "state " + std::to_string(state) : std::string("no actor")) +
                    ")");
                slot.phase = Phase::idle;
                slot.actor = 0;
                slot.taken = 0;
            } else if (const Target* t = Wanted(slot.number);
                       t != nullptr && state == addr::kStatePresent && t->actor == slot.actor_name &&
                       input_.scene == slot.scene) {
                WriteValue(guest, slot.actor + addr::kActorPosition, t->position);
                WriteValue(guest, slot.actor + addr::kActorRotation, t->rotation);
            }
            break;
        case Phase::deleting:
            if (!have_state || Gone(state) || now_ms - slot.since_ms > 5000) {
                Say("P" + std::to_string(slot.number) + " deleted (" +
                    (have_state ? "state " + std::to_string(state) : std::string("no actor")) +
                    ")");
                slot.phase = Phase::idle;
                slot.actor = 0;
                slot.taken = 0;
            }
            break;
        default:
            break;
        }
    }
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
    case Pending::none:
        break;
    }
    pending_ = Pending::none;
}

std::optional<Call> Director::Next(Slot& slot, Guest& guest, std::uint64_t now_ms) {
    const Target* t = Wanted(slot.number);
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
        if (t != nullptr && slot.block != 0 && now_ms >= slot.retry_ms && !input_.scene.empty()) {
            auto c = Create(slot, *t, guest);
            if (c) {
                slot.since_ms = now_ms;
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
        if (t == nullptr || t->actor != slot.actor_name || input_.scene != slot.scene) {
            Call c;
            c.function = input_.main_base + addr::kRequestDelete;
            c.x[0] = slot.actor;
            c.x[1] = 0;
            slot.phase = Phase::deleting;
            slot.since_ms = now_ms;
            pending_ = Pending::remove;
            return c;
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
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
    slot.scene = input_.scene;
    slot.request = 0;
    slot.taken = 0;
    slot.vtable = 0;
    slot.take = slot.drop = false;
    pending_ = Pending::create;
    return c;
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
        if (const Target* t = Wanted(slot.number)) {
            line += ", wanted at " + VecText(t->position);
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

Input Planner::Plan(std::uint64_t main_base, const std::optional<std::string>& scene,
                    const std::optional<Vec3>& position, const std::vector<Remote>& remotes,
                    std::uint64_t now_ms) {
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
        if (r.number == 0 || !r.scene || *r.scene != in.scene || !(r.fade > 0.0f)) {
            continue;
        }
        Target t;
        t.number = r.number;
        t.actor = settings_.actor;
        t.position = r.position;
        if (r.rotation) {
            t.rotation = QuatToMatrix(*r.rotation);
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
        t.actor = settings_.actor;
        t.position = *debug_spot_;
        in.targets.push_back(std::move(t));
    }
    return in;
}

namespace {

std::once_flag g_once;
std::atomic<bool> g_configured = false;
std::unique_ptr<Director> g_director;
std::unique_ptr<Planner> g_planner;  // the present thread only

} // namespace

void Configure(PlannerSettings settings, bool verbose, Director::Log log) {
    std::call_once(g_once, [&] {
        g_director = std::make_unique<Director>(std::move(log), verbose);
        g_planner = std::make_unique<Planner>(std::move(settings));
        g_configured = true;
    });
}

Director& TheDirector() {
    Configure({}, false, {});
    return *g_director;
}

void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, coop::host::Session* session,
             std::uint64_t server_now_us) {
    if (!g_configured) {
        return;  // puppets are off
    }
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
                }
                remotes.push_back(std::move(r));
            }
        });
    }
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    g_director->SetInput(g_planner->Plan(main_base, scene, position, remotes, now_ms));
}

} // namespace CoopPuppet
