// SPDX-License-Identifier: GPL-3.0-or-later
//
// deku-switches (#176, #129; tracker #90, #38; plan reports/deku-ladder-a.md option (c)): another player shot an eye
// switch, so our copy of its room event runs the room's own way.
//
// The Deku Tree's slingshot-room eye (Obj_EyeSwitch_A_02) has an AI that, on a slingshot hit by its own player, sets
// its logic output plug FirstOn. The room graph (Logic/InsideTheDekuTree_Level_Room_C) reads FirstOn in an
// IsFirstOnEventTrigger node and raises the room asker's (EventRequestActor) EventRequestTrigger; the asker's AI
// then asks InsideTheDekuTree_Gimmick_FallingLadder ITSELF, and that event's EventTriggerSetEventBool drops the
// ladder; its end sets RoomC_Cleared, deletes the asker and moves the pillar. Asking the event from outside (by name,
// with the asker linked as starter, or through the asker's performer: coop_live_events.h) plays the cutscene only:
// the asker's AI is still waiting for its trigger and writes EventBool 0 every frame (deku-platforms-b2).
//
// So for these events we do not ask the event: we set our eye's FirstOn to 1 with the game's own set-bool
// (coop_logic_set.h), as the eye's AI would after our own shot, and the room and its asker do the rest. Only our
// eye's model stays open (its blackboard isActive is not ours to set). Each frame up to kTryMs until an event runs
// (the asker asked it) or the asker is gone; FirstOn already 1 when we come: our game did it, nothing to do. When
// the eye or its FirstOn plug is never found, or no event starts in time, the event goes back to the old way
// (Requester::WantFromRoom, TakeFallbacks): no worse than before.
//
// deku-ladder (tracker rows 90, 102): the cause is the same for every room event whose asker's EventRequestTrigger is
// wired in the room graph to a switch's output plug, so the table holds all of them, read from the romfs wiring by
// tools/puppet_pack/eye_switches.py (gates, multi-inputs and module inputs followed to the switch): 63 events in 9
// scenes, eye, step, hit, hammer, dive and sun switches. Each line names its switch at its placement and the plug the
// graph reads: OnState (the Deku Tree's eyes: NOT FirstOn, the graph reads the eye's OnState), FirstOn, or isLightMax
// (sun switches). Every asker has exactly one switch on its trigger (no AND of two switches). Not here: askers with no
// wired trigger (the Water Temple's WaterTemple_EyeSwitch_Focus: the eye's own AI asks it; it spans two rooms,
// CoopLiveEvents::SpansRooms), the Forest Temple's room spins (silver eyes' isHit; their flags are not shared, they
// are rides: coop_dungeon.cpp), and events with no live place (not shared or not startable).
//
// OFF unless LICHII_COOP_EYE_SWITCH=1 until the probe run (scenario hs-eye-probe) shows FirstOn's key on the eye,
// whether it latches, and that the asker's trigger follows it. No emulator code (tests/lichii/eye_switch_test.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_puppet.h"

namespace CoopEyeSwitch {

inline constexpr std::uint32_t kFirstOn = 0x2d9754fd;  // Murmur3("FirstOn"); the plug's key is ^ 7

// A room event started by a switch's output plug, one per (event, asker). Generated (see above). An event asked
// by two askers (SpiritTemple_Level_Room_19_SwitchOn_01) has two lines; the one nearest our Link whose asker is
// here is used.
struct Switch {
    std::string_view event;  // without "HavePlayedEvent."
    std::string_view scene;
    std::string_view actor;      // the switch's actor name
    std::array<float, 3> at;     // the switch's placement (x, height, z); switches do not move
    std::string_view plug;       // the output the room graph reads (OnState, FirstOn, isLightMax)
    std::array<float, 3> asker_at;  // its asker's placement (a CoopLiveEvents::Places() place)
};
std::span<const Switch> Switches();
// The first line of `event` in `scene`, if it is one.
const Switch* Find(std::string_view event, std::string_view scene);
// Every line of `event` in `scene` that we drive (Latches), nearest switch to `link` first (table order without `link`).
std::vector<const Switch*> FindAll(std::string_view event, std::string_view scene,
                                   const std::optional<std::array<float, 3>>& link = std::nullopt);
// The plug's key as the node block names it (Murmur3 of the name; the plug keeps it ^ 7).
std::uint32_t PlugKey(std::string_view plug);
// Whether our set of this line's plug stays until the room graph reads it (deku-ladder, offline hs-step-probe
// 2026-11-16): an eye's OnState latches after its hit (hs-eye-probe PASS: the plug read back 1 the next frame), and
// FirstOn is a first-on latch. A step switch's OnState (and a hit, hammer, dive or sun switch's OnState/isLightMax)
// is rewritten by the switch's own AI every frame from its contact/hit/light: our 1 read back 0 every frame for 2 s
// (Jabu-Jabu room 1, no event), and no game function sets its inputs (ForceControl) yet. Those lines stay in the
// table (generated) but are not driven: their events go the old way at once, and their guard flags are not held -
// except the lines of Inputs() (below), which are driven through the switch's input.
bool Latches(const Switch& s);

// deku-ladder (A #39): the 39 lines whose plug does not latch, classified from the romfs wiring
// (tools/puppet_pack/eye_switches.py --classify): 36 are class A - every room result of the event reaches our room
// through a shared flag (the switch's ON input, the room's actors, the asker's delete all read it), or is only the
// event's moment (camera, a chest's appear act, the event's own air walls): the other player's live flags are the
// native path and our room ends as theirs does after a reload. g3-objects: 38 class A, 1 class B (GTG room 6,
// below); Inputs() is EMPTY. Jabu-Jabu room 14's water switches (upper and lower) were the two input lines while
// the water flags were each game's own; with those flags shared (coop_dungeon.cpp) both are class A, and running
// our own asker would write the shared two-way water flags a second time, late (a second writer: it could flip the
// water back for everyone). The tool refuses an input or split drive for any asker that writes a shared flag some
// asker also writes the other way. The input path below stays for a future class B line with no such write.
// Before g3-objects: 36 class A, 3 class B - a result hangs on OUR asker only.
// Two of them (Jabu-Jabu room 14's water switch, upper and lower: the event toggles the water through flags that
// were each game's own and re-arms the switch) had an ON input the room graph leaves unwired (FirstOn): we set that
// input (the set-bool's own store, CoopLogicSet::SetInput), the switch's own AI turns it on and keeps OnState, and
// our asker asks the event. Only where the input's value address is provably that plug's own on this instance
// (CoopLogicSet::InputNotOwn; else the old way), and the old byte is stored back when the try ends (the event
// started, it gave up, our game has it played: the switch is re-armed by its own event and must not stay on). The third (Gerudo Training Ground room 6's fire: its burn sign ANDs the asker's
// EventBool) has no settable input (a step switch: BootUp only) and stays on the old way (coop_eye_switch.cpp).
struct Input {
    std::string_view event;
    std::string_view scene;
    std::string_view actor;   // the switch, as its Switch line
    std::array<float, 3> at;  // its placement, as its Switch line
    std::string_view input;   // the unwired ON input we set
};
std::span<const Input> Inputs();
// The input line of `s`, if it has one.
const Input* InputOf(const Switch& s);
// Tests only (g3-objects): Inputs() is empty in the generated table, so the input path's tests put their own lines
// in for one scope (the previous table comes back on scope exit). Not thread-safe; never used by the hook.
class ScopedInputsForTest {
public:
    explicit ScopedInputsForTest(std::span<const Input> lines);
    ~ScopedInputsForTest();
    ScopedInputsForTest(const ScopedInputsForTest&) = delete;
    ScopedInputsForTest& operator=(const ScopedInputsForTest&) = delete;

private:
    std::span<const Input> old_;
};

// AND-gated triggers (deku-ladder round 3): a trigger can AND the switch with a saved room flag read NEGATED (the
// room's "done" flag: GTG room 6's fire, Jabu room 1's first press), and the asker itself is deleted by a room flag
// (Logic_Delete: the Deku ladder's RoomC_Cleared, Ganon's Castle's switch flags). The other player's game sets that
// flag during or at the end of its event and it is shared, so it reaches us BEFORE our copy of the event runs: our
// asker is gone or our gate is false, and setting our switch starts nothing. These are the guards (generated with
// the table, only flags that coop_dungeon.cpp shares): while the switch path is on, the hook holds each guard flag in
// its dungeon (like the held HavePlayedEvent flags and RoomC_Cleared, coop_dungeon.h HeldIn) until our own event sets
// it (our game has the event played), we leave the dungeon or load, or the switch path's try of its event ended this
// visit without our copy running (no asker, the old way: then the flag comes as before). The other inputs the tool finds are listed in coop_eye_switch.cpp (not covered, with why).
struct Guard {
    std::string_view flag;
    std::string_view scene;
    std::string_view event;
};
std::span<const Guard> Guards();

// Other switches of the same kind within this of ours: their input's value address must differ from ours (InputNotOwn;
// Jabu-Jabu room 14's upper and lower switches are 8 m apart).
inline constexpr float kSameKindM = 60.0f;
// Our switch is the one of its name within this of its placement (the placement is exact; other switches of the
// room are metres away: Dodongo's Cavern's fire-wall eyes, 9 and 19 m from its event's eye, start other graphs).
inline constexpr float kEyeAtM = 1.5f;
// How long the set is tried (and then waited for the asker's event) before the old way is taken.
inline constexpr std::uint64_t kTryMs = 2000;

// LICHII_COOP_EYE_SWITCH: "1" on; anything else off (the default until the probe run).
inline bool OnFromEnv(std::string_view value) { return value == "1"; }

// The present actors named one of `names` within `max_m` of `at` (x, height, z), nearest first. The calc thread.
using Finder = std::function<std::vector<std::uint64_t>(CoopPuppet::Guest&, std::uint64_t main,
                                                        std::span<const std::string_view> names,
                                                        const std::array<float, 3>& at, float max_m)>;
// The game's actor list (CoopFairy::WalkActors).
std::vector<std::uint64_t> FindActors(CoopPuppet::Guest& guest, std::uint64_t main,
                                      std::span<const std::string_view> names, const std::array<float, 3>& at,
                                      float max_m);

// The svc caller: one switch event at a time.
class Switcher {
public:
    explicit Switcher(Finder find = FindActors) : find_(std::move(find)) {}
    // The hook thread: start `event` in `scene` by its eye; `link` (our Link's place) picks the room when the event
    // has two. False: not a switch event (ask it the old way).
    bool Want(const std::string& event, const std::string& scene,
              std::optional<std::array<float, 3>> link = std::nullopt);
    // The hook thread, once a frame: drops what was wanted in another scene than `scene`, and what `played(event)`
    // says our game has played already (HavePlayedEvent.<event>: our own asker ran it; never set the eye again).
    void Expire(const std::string& scene, const std::function<bool(const std::string&)>& played = nullptr);
    // The calc thread (CoopSvc caller contract).
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main, std::uint64_t now_ms);
    std::vector<std::string> TakeNotes();
    // G1: called with the event's name when our switch is set for another game's event (the
    // room graph starts that event here next; the hook logs it).
    void OnSet(std::function<void(const std::string&)> set) { on_set_ = std::move(set); }
    // (event, scene) to ask the old way (Requester::WantFromRoom).
    std::vector<std::pair<std::string, std::string>> TakeFallbacks();
    // Any thread (the flag hold): whether the shared flag `hash` is a guard (Guards()) of an event of `scene` that
    // our copy has not played yet and whose try did not end without our copy running, so it waits in our game.
    bool HoldsGuard(std::uint32_t hash, std::string_view scene);

private:
    struct Try {
        std::string event;
        std::string scene;
        std::optional<std::array<float, 3>> link;
        std::uint64_t until_ms = 0;  // 0: not begun
        bool set = false;            // FirstOn written at least once
        bool told_waiting = false;
        struct Restore {  // an input line's store, to be taken back when the try ends
            std::uint64_t actor = 0;
            std::uint32_t key = 0;
            std::uint64_t value_word = 0;
            std::uint8_t old = 0;
            std::string_view name;
            std::array<float, 3> at{};
        };
        std::optional<Restore> restore;
    };
    // Calc thread: stores each ended try's old input byte back, if its switch and plug are still the same.
    void RestoreInputs(CoopPuppet::Guest& guest, std::uint64_t main);
    std::optional<CoopPuppet::Call> Frame(CoopPuppet::Guest& guest, std::uint64_t main, std::uint64_t now_ms);
    void End(const std::string& why, bool fallback, bool ran = false);
    static bool EventRunning(CoopPuppet::Guest& guest, std::uint64_t main);

    Finder find_;
    std::mutex mutex_;
    std::deque<Try> wanted_;  // front: the one being tried
    std::vector<std::string> notes_;
    std::function<void(const std::string&)> on_set_;
    std::vector<std::pair<std::string, std::string>> fallbacks_;
    std::uint64_t block_ = 0;
    enum class Next { none, malloc, execute };
    Next next_ = Next::none;
    bool failed_ = false;  // the malloc gave nothing: every event the old way
    std::string scene_;                    // the scene of this visit (Expire)
    std::vector<std::string> ended_;  // events of this visit whose guards come as before (not run here, or played)
    std::vector<std::string> ran_;    // events of this visit our copy ran: guards held until our game has them played
    std::vector<Try::Restore> restores_;  // ended input stores, taken back on the next Step (calc thread)
    // HoldsGuard's flag hash per kGuards line (0: its event has no line we drive), built from the input lines it
    // was made with: built again when they change (a test's ScopedInputsForTest), never left stale.
    std::vector<std::uint32_t> guard_hashes_;
    const Input* guard_inputs_ = nullptr;
    std::size_t guard_inputs_size_ = 0;
};

}  // namespace CoopEyeSwitch
