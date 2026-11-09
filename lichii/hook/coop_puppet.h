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
namespace coop::net {
struct Event;
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
// The pending transform the engine's own setter 0xc6fd60(actor, mtx34, 0)
// (the run of OneShotActorRequestSetMtx, 0x28b2050) leaves on an actor with
// no physics body: position at +0x358, rotation rows at +0x364. The actor's
// calc applies it to +0x31c and +0x328; written with them every frame.
inline constexpr std::uint64_t kActorPositionRequest = 0x358;
inline constexpr std::uint64_t kActorRotationRequest = 0x364;
inline constexpr std::uint32_t kStatePresent = 4;
inline constexpr std::uint64_t kLinkHolder = 0x08;
inline constexpr std::uint64_t kLinkId = 0x10;
inline constexpr std::uint64_t kHolderActor = 0x40;
inline constexpr std::uint64_t kHolderId = 0x48;

// Animation (docs/puppet.md, Animation). The AS controller is actor +0x270,
// then +0x40, +0x18, +0x18; its slot count at +0x18, slots at +0x20 (0x48
// bytes each, the first 8 point to the slot); slot +0x10 the command name,
// +0xe8 the frame.
inline constexpr std::uint64_t kPlayAs = 0x2022a0;      // (request)
inline constexpr std::uint64_t kSetAsFrame = 0x37b5d70; // (controller, slot, s0 = frame)
inline constexpr std::array<std::uint64_t, 4> kControllerPath{0x270, 0x40, 0x18, 0x18};
inline constexpr std::uint64_t kSlotCount = 0x18;
inline constexpr std::uint64_t kSlots = 0x20;
inline constexpr std::uint64_t kSlotName = 0x10;
inline constexpr std::uint64_t kSlotFrame = 0xe8;
// ExecuteASSyncPartialAnimation's call: 0x3abec10(actor, from slot, to slot,
// node 0, 0) plays the command of one slot on another (request skip-if-same
// 1, the source's rate) through 0x37b59c0. Link's upper body and other
// partial slots are filled this way from slot 0.
inline constexpr std::uint64_t kSyncAsSlot = 0x3abec10;
inline constexpr std::int32_t kMaxAsSlots = 8;  // slots synced from slot 0

// Gear (docs/puppet.md, Equipment): the EquipmentUser is [[actor + 0x270] +
// 0x200]; 0x2df7b20(eu, kind, &char*, state 2, -1, 1, 0) wears the named
// piece in a slot kind, as the game's own GameData path does.
inline constexpr std::uint64_t kEquip = 0x2df7b20;
inline constexpr std::uint64_t kActorParts = 0x270;
inline constexpr std::uint64_t kPartsEquipmentUser = 0x200;
}  // namespace addr

// The slot kinds the puppet wears, in Target::gear order: weapon, shield,
// head, armor, boots (reports/s2-equip-create: kinds 0, 6, 7, 8, 9).
inline constexpr std::array<std::uint32_t, 5> kGearKinds{0, 6, 7, 8, 9};
inline constexpr std::array<const char*, 5> kGearNames{"weapon", "shield", "head", "armor",
                                                       "boots"};

// A puppet's memory block, from the game's malloc, kept for the whole run.
namespace block {
inline constexpr std::uint64_t kSize = 0x500;
inline constexpr std::uint64_t kCreateArg = 0x000;  // 0x100 bytes
inline constexpr std::uint64_t kName = 0x100;       // {char*, u32 capacity}
inline constexpr std::uint64_t kHandles = 0x110;    // kHandleCount slots of 8 bytes
inline constexpr std::uint64_t kHandleCount = 6;
inline constexpr std::uint64_t kNameText = 0x140;   // kNameCapacity bytes
inline constexpr std::uint32_t kNameCapacity = 0x40;
// The play request for 0x2022a0: +0 actor, +8 name, +0x10 slot, +0x14 rate,
// +0x18 and +0x1c -1.0, +0x20 skip-if-same, +0x28 node (0, as 0x2a95c4).
inline constexpr std::uint64_t kPlayRequest = 0x200;  // 0x30 bytes
// Two name buffers, used in turn: the slot may keep the pointer it was
// given, and a new name in the same buffer would look like the same one.
inline constexpr std::uint64_t kAnimText = 0x240;
inline constexpr std::uint64_t kAnimCapacity = 0x50;  // 64 characters and the 0
// One name cell per gear kind (kGearKinds order), and the char* to each that
// the equip call is given.
inline constexpr std::uint64_t kGearText = 0x300;
inline constexpr std::uint64_t kGearCapacity = 0x40;
inline constexpr std::uint64_t kGearPtr = 0x440;  // 5 x 8 bytes
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
    float s0 = 0.0f;                     // the first float argument
};

// A remote player's animation, as PLAYER_SAMPLE field 7 carries it.
struct Anim {
    std::string name;  // the AS command
    float frame = 0.0f;
    float rate = 1.0f;
};

// coop/host/scene.hpp's Visibility, for a puppet that exists.
enum class Show { show, hold, hide };

// What one puppet should show.
struct Target {
    std::uint8_t number = 0;  // the player; 0 is the debug puppet
    std::string actor;        // the actor pack name
    Vec3 position{};
    Mat3 rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Show show = Show::show;   // hold: frozen as it is; hide: parked out of sight
    std::optional<Anim> anim;
    std::uint32_t anim_serial = 0;  // goes up with each received animation
    // What the player wears, in kGearKinds order; unset: not known yet,
    // "" nothing (not taken off yet; the puppet keeps what it has).
    std::array<std::optional<std::string>, 5> gear;
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
    // `anim`: a line for each animation played or frame set (LICHII_COOP_DEBUG_ANIM).
    explicit Director(Log log = {}, bool verbose = false, bool anim = false);

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
        std::string anim_name;  // what was last played on it
    };
    std::vector<SlotView> Slots() const;  // the calc thread, or tests
    std::uint64_t Frames() const { return frames_; }

    static constexpr int kMaxCallsPerFrame = 8;      // the cave allows 16
    static constexpr std::uint64_t kCreateTimeoutMs = 10000;
    // A live puppet whose player drops out of the plan in the same scene
    // (a stalled or late peer fades to 0 for a moment) is kept this long
    // before it is deleted, so a hiccup does not delete and create it again.
    static constexpr std::uint64_t kLeaveGraceMs = 2000;
    // A received frame further than this from the puppet's is set on it.
    static constexpr float kFrameSlack = 3.0f;
    // A hidden puppet is moved this far down, under the floor.
    static constexpr float kHideDrop = 500.0f;

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
        std::string anim_name;        // played on it; "" none yet
        std::uint32_t anim_serial = 0;
        std::uint64_t anim_buffer = 0;  // which name buffer is next
        std::optional<float> frame_to_set;  // after a play
        std::optional<float> held_frame;    // while held
        std::uint64_t frame_set_in = 0;     // the cave frame of the last frame set
        std::string play_name;              // the play being made
        float play_frame = 0.0f;
        std::uint32_t play_serial = 0;
        std::int32_t sync_next = 0;         // next slot to sync from slot 0; 0 none
        std::uint64_t pose_log_ms = 0;      // last read-back line (debug anim)
        std::array<std::string, 5> worn;    // equipped on it, by gear index
        std::size_t equip_index = 0;        // the equip being made
        std::string equip_name;
        std::uint64_t gear_log_ms = 0;      // last gear line (debug anim)
    };
    enum class Pending { none, malloc, create, take, drop, remove, play, frame, sync, equip };

    void NewFrame(Guest& guest, std::uint64_t now_ms);
    void PoseFrame(Guest& guest, std::uint64_t now_ms);
    void Finish(Guest& guest, std::uint64_t result, std::uint64_t now_ms);
    std::optional<Call> Next(Slot& slot, Guest& guest, std::uint64_t now_ms);
    std::optional<Call> Create(Slot& slot, const Target& target, Guest& guest);
    std::uint64_t Resolve(Slot& slot, Guest& guest);
    std::optional<Call> Animate(Slot& slot, const Target& target, Guest& guest);
    std::optional<Call> Equip(Slot& slot, const Target& target, Guest& guest,
                              std::uint64_t now_ms);
    std::optional<Call> SetFrame(Slot& slot, std::uint64_t controller, float frame);
    static std::uint64_t Controller(Guest& guest, std::uint64_t actor);
    static std::optional<float> Frame(Guest& guest, std::uint64_t controller);
    void Pose(Slot& slot, Guest& guest, const Vec3& position, const Mat3* rotation,
              std::uint64_t now_ms);
    void Poll(Slot& slot, Guest& guest, std::uint64_t now_ms);
    const Target* Wanted(std::uint8_t number) const;
    Slot& SlotFor(std::uint8_t number);
    void Say(const std::string& text) const;
    void Status(Guest& guest, std::uint64_t now_ms);

    Log log_;
    bool verbose_;
    bool anim_;

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
    std::uint8_t status = 0;  // PLAYER_STATUS flags
    bool adult = false;
    std::optional<Anim> anim;
    std::uint32_t anim_serial = 0;
    std::array<std::optional<std::string>, 5> gear;  // as Target::gear
};

struct PlannerSettings {
    // The actor pack; "" picks CoopPuppet_Child or CoopPuppet_Adult by the
    // player's age (LICHII_COOP_PUPPET_ACTOR overrides it).
    std::string actor;
    bool debug_anim = false;      // LICHII_COOP_DEBUG_ANIM
    bool debug = false;           // LICHII_COOP_DEBUG_PUPPET: one more, beside you
    std::uint64_t debug_delay_ms = 3000;  // after the scene is known
    Vec3 debug_offset{2.0f, 0.0f, 0.0f};  // from you, in game units (about metres)
};

class Planner {
public:
    explicit Planner(PlannerSettings settings = {}) : settings_(std::move(settings)) {}
    // `local_event`: our own cutscene runs (it hides every puppet).
    Input Plan(std::uint64_t main_base, const std::optional<std::string>& scene,
               const std::optional<Vec3>& position, bool local_event,
               const std::vector<Remote>& remotes, std::uint64_t now_ms);
    std::string ActorFor(bool adult) const;
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
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us);

// The session's on_event (network thread): keeps each remote player's latest
// animation from PLAYER_SAMPLE field 7 and gear from PLAYER_EQUIP.
void OnEvent(const coop::net::Event& event);

} // namespace CoopPuppet
