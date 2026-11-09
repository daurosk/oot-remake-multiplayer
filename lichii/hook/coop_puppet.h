// Puppets: a remote player shown as an actor in the local game (plan stage 1
// step 2, docs/puppet.md). No emulator code here, so it is tested on its own.
//
// Two threads:
// - the present thread (CoopHook::OnFrame) calls Publish: it decides what
//   should be shown (Planner) and hands it to the Director;
// - the game's calc thread runs the code cave once per game frame
//   (coop_svc.s). Each `svc #0xf4` from it reaches Director::Step, which
//   reads and writes guest memory directly and returns at most one game
//   function for the cave to call: malloc for a puppet's memory block, the
//   create request, the delete request.
// The pose is written straight into the actor every game frame.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coop::host {
class Session;
}

namespace CoopPuppet {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;  // x, y, z, w
using Mat3 = std::array<float, 9>;  // rows of 3, as the actor holds it

// Offsets into main of build e0f3ccfaf50065ae (docs/puppet.md).
namespace addr {
inline constexpr std::uint64_t kCreate = 0x372a70;          // requestCreateActorAsync
inline constexpr std::uint64_t kActorManagerPtr = 0x4e91888;
inline constexpr std::uint64_t kMalloc = 0x14c6c90;
inline constexpr std::uint64_t kRequestDelete = 0xa6490;    // (actor, 0)
// What the create puts in the handle slot is a request: its low 16 bits at
// +0 are its state, +8 the actor. SeqExecuteCreateActor's check (0x32bcb30)
// waits for state 3, then takes the actor with 0x36fbd0(request, 1, 0),
// which wakes it (0x94f30) and returns it, and clears the slot. State 4 it
// hands to 0x36fde0(request); 5 and 6 are failures.
inline constexpr std::uint64_t kTakeCreated = 0x36fbd0;     // (request, 1, 0) -> actor
inline constexpr std::uint64_t kDropCreate = 0x36fde0;      // (request)
inline constexpr std::uint64_t kRequestActor = 0x08;
inline constexpr std::uint32_t kRequestReady = 3;
inline constexpr std::uint32_t kRequestDrop = 4;
// The GOT entry the preload walker reads for create argument +0x40.
inline constexpr std::uint64_t kGotCreateDefault = 0x4c7dad0;

inline constexpr std::uint64_t kActorState = 0x2c;
inline constexpr std::uint64_t kActorPosition = 0x31c;
inline constexpr std::uint64_t kActorRotation = 0x328;
inline constexpr std::uint32_t kStatePresent = 4;
inline constexpr std::uint64_t kLinkHolder = 0x08;
inline constexpr std::uint64_t kLinkId = 0x10;
inline constexpr std::uint64_t kHolderActor = 0x40;
inline constexpr std::uint64_t kHolderId = 0x48;
}  // namespace addr

// A puppet's memory block, from the game's malloc, kept for the whole run.
namespace block {
inline constexpr std::uint64_t kSize = 0x200;
inline constexpr std::uint64_t kCreateArg = 0x000;  // 0x100 bytes
inline constexpr std::uint64_t kName = 0x100;       // {char*, u32 capacity}
inline constexpr std::uint64_t kHandles = 0x110;    // kHandleCount slots of 8 bytes
inline constexpr std::uint64_t kHandleCount = 6;
inline constexpr std::uint64_t kNameText = 0x140;   // kNameCapacity bytes
inline constexpr std::uint32_t kNameCapacity = 0x40;
}  // namespace block

// Guest memory, as the director sees it. Failures are quiet.
class Guest {
public:
    virtual ~Guest() = default;
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
};

// One game function for the cave to call.
struct Call {
    std::uint64_t function = 0;          // absolute
    std::array<std::uint64_t, 8> x{};    // x0 to x7
    std::array<std::uint64_t, 4> stack{};// what the function sees at sp
};

// What one puppet should show.
struct Target {
    std::uint8_t number = 0;  // the player; 0 is the debug puppet
    std::string actor;        // the actor pack name
    Vec3 position{};
    Mat3 rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
};

struct Input {
    std::uint64_t main_base = 0;  // 0: no game, nothing is done
    std::string scene;            // the local scene; "" not known
    std::vector<Target> targets;
};

Mat3 QuatToMatrix(const Quat& q);  // rows; normalises q, identity if it is 0

enum class Phase { no_block, allocating, idle, creating, live, deleting };
const char* PhaseText(Phase phase);

class Director {
public:
    using Log = std::function<void(const std::string&)>;
    // `verbose`: a status line per puppet every second (LICHII_COOP_DEBUG_PUPPET).
    explicit Director(Log log = {}, bool verbose = false);

    // Any thread. The next frame of the cave uses it.
    void SetInput(Input input);

    // The game's calc thread, at each `svc #0xf4`. `after_call`: the cave
    // made the call returned last time and `result` is its x0. Returns the
    // next call, or nothing for "done for this frame".
    std::optional<Call> Step(Guest& guest, bool after_call, std::uint64_t result,
                             std::uint64_t now_ms);

    struct SlotView {
        std::uint8_t number = 0;
        Phase phase = Phase::no_block;
        std::uint64_t block = 0;
        std::uint64_t actor = 0;
        std::string actor_name;
    };
    std::vector<SlotView> Slots() const;  // the calc thread, or tests
    std::uint64_t Frames() const { return frames_; }

    static constexpr int kMaxCallsPerFrame = 4;      // the cave allows 16
    static constexpr std::uint64_t kCreateTimeoutMs = 10000;
    // A live puppet whose player drops out of the plan in the same scene
    // (a stalled or late peer fades to 0 for a moment) is kept this long
    // before it is deleted, so a hiccup does not delete and create it again.
    static constexpr std::uint64_t kLeaveGraceMs = 2000;

private:
    struct Slot {
        std::uint8_t number = 0;
        Phase phase = Phase::no_block;
        std::uint64_t block = 0;
        std::uint64_t handle = 0;   // the handle slot of the current create
        std::uint64_t handle_index = 0;
        std::uint64_t actor = 0;    // resolved this frame; 0 none
        std::uint64_t request = 0;  // what the handle slot held
        std::uint64_t taken = 0;    // the actor 0x36fbd0 gave
        std::uint64_t vtable = 0;   // its vtable then, to notice reuse
        bool take = false;          // call 0x36fbd0 next
        bool drop = false;          // call 0x36fde0 next
        std::string actor_name;     // what was created
        std::string scene;          // where it was created
        std::uint64_t since_ms = 0; // when the phase began
        std::uint64_t retry_ms = 0; // no new malloc or create before this
        std::uint64_t missing_ms = 0; // when its player left the plan; 0 wanted
    };
    enum class Pending { none, malloc, create, take, drop, remove };

    void NewFrame(Guest& guest, std::uint64_t now_ms);
    void Finish(Guest& guest, std::uint64_t result, std::uint64_t now_ms);
    std::optional<Call> Next(Slot& slot, Guest& guest, std::uint64_t now_ms);
    std::optional<Call> Create(Slot& slot, const Target& target, Guest& guest);
    std::uint64_t Resolve(Slot& slot, Guest& guest);
    void Poll(Slot& slot, Guest& guest, std::uint64_t now_ms);
    const Target* Wanted(std::uint8_t number) const;
    Slot& SlotFor(std::uint8_t number);
    void Say(const std::string& text) const;
    void Status(Guest& guest, std::uint64_t now_ms);

    Log log_;
    bool verbose_;

    mutable std::mutex input_mutex_;
    Input next_input_;  // guarded

    // The calc thread only.
    Input input_;
    std::vector<Slot> slots_;
    Pending pending_ = Pending::none;
    std::uint8_t pending_slot_ = 0;
    int calls_ = 0;  // this frame
    std::uint64_t frames_ = 0;
    std::uint64_t frames_at_status_ = 0;
    std::uint64_t status_ms_ = 0;
};

// The present thread's side: what to show this frame.
struct Remote {
    std::uint8_t number = 0;
    std::optional<std::string> scene;
    Vec3 position{};
    std::optional<Quat> rotation;
    float fade = 1.0f;
};

struct PlannerSettings {
    std::string actor = "Animal_Cow_LinkHouse";  // until the puppet pack exists
    bool debug = false;           // LICHII_COOP_DEBUG_PUPPET: one more, beside you
    std::uint64_t debug_delay_ms = 3000;  // after the scene is known
    Vec3 debug_offset{2.0f, 0.0f, 0.0f};  // from you, in game units (about metres)
};

class Planner {
public:
    explicit Planner(PlannerSettings settings = {}) : settings_(std::move(settings)) {}
    Input Plan(std::uint64_t main_base, const std::optional<std::string>& scene,
               const std::optional<Vec3>& position, const std::vector<Remote>& remotes,
               std::uint64_t now_ms);
    const PlannerSettings& settings() const { return settings_; }

private:
    PlannerSettings settings_;
    std::string scene_;
    std::uint64_t scene_since_ms_ = 0;
    std::optional<Vec3> debug_spot_;
};

// The process-wide pair the hook and the SVC handler share.
// Configure once, before the game starts (CoopSvc does it).
void Configure(PlannerSettings settings, bool verbose, Director::Log log);
Director& TheDirector();

// CoopHook::OnFrame, once per present, with no lock held. `session` may be
// null (not connected): then only the debug puppet is planned.
void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, coop::host::Session* session,
             std::uint64_t server_now_us);

} // namespace CoopPuppet
