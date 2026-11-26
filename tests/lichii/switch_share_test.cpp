// Tests for the timed switches' presses (lichii/hook/coop_switch_share.cpp, room-objects).
#include <doctest/doctest.h>

#include <array>
#include <set>
#include <string>
#include <vector>

#include "coop_room_objects.h"
#include "coop_switch_share.h"

using namespace CoopSwitchShare;
namespace proto = coop::protocol;

namespace {

constexpr const char* kScene = "DodongoCavern";

// Dodongo's Cavern's moving-ground plate: Obj_StepSwitch_A_01 OnState through a timer (GroundControlActor_SimpleMoveTo).
const CoopRoomObjects::SwitchPlug* Plate() {
    const auto lines = CoopRoomObjects::SwitchPlugsOf(kScene, "Obj_StepSwitch_A_01", {17.2f, 112.3f, -23.0f});
    REQUIRE(lines.size() == 1);
    return lines[0];
}

struct Game {
    Sharer sharer;
    std::uint8_t self = 1;
    bool on = false;  // our switch's plug as the game has it
    std::vector<proto::SwitchState> sent;
    std::vector<Hold> holds;
    std::vector<std::string> notes;
    std::set<std::uint8_t> away;  // the players whose world stands still (PLAYER_STATUS paused or loading)

    void Tick(std::uint64_t now_ms, bool others = true, const std::string& scene = kScene) {
        std::vector<proto::SwitchState> out;
        holds = sharer.Tick(scene, {Seen{Plate(), 0x1000, on, true}}, others, self, now_ms, out, notes, away);
        for (auto& m : out) {
            m.player_number = self;  // the server's relay writes the sender
            sent.push_back(m);
        }
    }
};

bool Has(const std::vector<std::string>& notes, const char* text) {
    for (const auto& n : notes) {
        if (n.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("switch share: the generated lines are the timed switches no CoopEyeSwitch line covers") {
    CHECK(CoopRoomObjects::SwitchPlugs().size() == 16);  // 12 timed + 4 behind an asker guard (g3-objects)
    CHECK(Plate()->plug == "OnState");
    // The Water Temple fountain reads one switch's OffState.
    CHECK(CoopRoomObjects::SwitchPlugsOf("WaterTemple", "Obj_SwitchHit_A_06", {52.1f, 110.0f, -23.2f}).size() == 1);
    // A switch elsewhere, or another scene: no line.
    CHECK(CoopRoomObjects::SwitchPlugsOf(kScene, "Obj_StepSwitch_A_01", {0.0f, 0.0f, 0.0f}).empty());
    CHECK(CoopRoomObjects::SwitchPlugsOf("ForestTemple", "Obj_StepSwitch_A_01", {17.2f, 112.3f, -23.0f}).empty());
    CHECK(CoopRoomObjects::SharedSwitchName("Obj_SwitchHit_A_06"));
    CHECK_FALSE(CoopRoomObjects::SharedSwitchName("Obj_DiveSwitch_A_01"));  // a CoopEyeSwitch line only
}

TEST_CASE("switch share: a presser whose world stands still is not held, and is again when back (row 164 follow-up)") {
    Game a;
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    a.on = true;
    a.Tick(t += 100);
    REQUIRE(a.sent.size() == 1);
    b.sharer.Receive(a.sent[0], t);
    b.Tick(t += 100);
    REQUIRE(b.holds.size() == 1);  // A stands on the plate: B's held
    // A opens the pause or quick-switch menu: its own timer stands still, B's plate is let go at once (not after
    // kRemoteHoldMs), so B's own timer runs as when A steps off.
    b.away = {1};
    b.Tick(t += 100);
    CHECK(b.holds.empty());
    b.Tick(t += 1000);
    CHECK(b.holds.empty());
    // A is back, still on the plate: held again, with no new press needed within kRemoteHoldMs.
    b.away.clear();
    b.Tick(t += 100);
    CHECK(b.holds.size() == 1);
    // Another player away changes nothing for A's press.
    b.away = {3};
    b.Tick(t += 100);
    CHECK(b.holds.size() == 1);
}

TEST_CASE("switch share: our press is sent, held on the other game's switch while on, let go when off") {
    Game a;
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    a.Tick(t);
    b.Tick(t);
    CHECK(a.sent.empty());
    // A's player steps on the plate.
    a.on = true;
    a.Tick(t += 100);
    REQUIRE(a.sent.size() == 1);
    CHECK(a.sent[0].on);
    CHECK(a.sent[0].name == "Obj_StepSwitch_A_01");
    CHECK(a.sent[0].plug == PlugKey("OnState"));
    CHECK(Has(a.notes, "our player's press: sent"));
    b.sharer.Receive(a.sent[0], t);
    b.Tick(t += 50);
    REQUIRE(b.holds.size() == 1);
    CHECK(b.holds[0] == Hold{0x1000, PlugKey("OnState")});
    CHECK(Has(b.notes, "another player's is on"));
    // Our set made B's plug 1: still held (nothing to set while 1), and never sent back.
    b.on = true;
    for (int i = 0; i < 30; ++i) b.Tick(t += 100);
    CHECK(b.sent.empty());
    // A sends again while on (a later player gets it).
    for (int i = 0; i < 25; ++i) a.Tick(t += 100);
    CHECK(a.sent.size() >= 2);
    // A's plate goes off: off sent, B lets go; B's own 0 comes back and B sends nothing.
    a.on = false;
    a.Tick(t += 100);
    REQUIRE(!a.sent.empty());
    CHECK_FALSE(a.sent.back().on);
    b.sharer.Receive(a.sent.back(), t);
    b.Tick(t += 50);
    CHECK(b.holds.empty());
    CHECK(Has(b.notes, "let go after"));
    CHECK(Has(a.notes, "our player's press ended after"));
    b.on = false;
    for (int i = 0; i < 20; ++i) b.Tick(t += 100);
    CHECK(b.sent.empty());
}

TEST_CASE("switch share: our plug still 1 just after we held it is not a press of ours (echo)") {
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.sharer.Receive(on, t);
    b.on = true;
    b.Tick(t += 50);
    auto off = on;
    off.on = false;
    b.sharer.Receive(off, t);
    b.Tick(t += 50);  // our plug says 1 for a moment more
    b.Tick(t += 300);
    CHECK(b.sent.empty());
    // Later our own player presses it: that is ours.
    b.on = false;
    b.Tick(t += kEchoMs);
    b.on = true;
    b.Tick(t += 100);
    CHECK(b.sent.size() == 1);
}

TEST_CASE("switch share: solo sends nothing and holds nothing, and a remote on not sent again is let go") {
    Game a;
    std::uint64_t t = 1000;
    a.on = true;
    for (int i = 0; i < 30; ++i) a.Tick(t += 100, false);
    CHECK(a.sent.empty());
    CHECK(a.holds.empty());
    Game b;
    b.self = 2;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.sharer.Receive(on, t);
    b.Tick(t += 100);
    CHECK(b.holds.size() == 1);
    b.Tick(t += kRemoteHoldMs + 100);  // the sender left without an off
    CHECK(b.holds.empty());
    // Another plug key or a far place is no line of ours.
    auto other = on;
    other.plug = PlugKey("FirstOn");
    b.sharer.Receive(other, t);
    other = on;
    other.place = {0.0f, 0.0f, 0.0f};
    b.sharer.Receive(other, t);
    b.Tick(t += 100);
    CHECK(b.holds.empty());
}

TEST_CASE("switch share: a player who leaves is let go at once, not after the timeout") {
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.sharer.Receive(on, t);
    auto three = on;
    three.player_number = 3;
    b.sharer.Receive(three, t);
    b.Tick(t += 100);
    REQUIRE(b.holds.size() == 1);
    b.sharer.Forget(1);
    b.Tick(t += 100);
    CHECK(b.holds.size() == 1);  // player 3's is still on
    b.sharer.Forget(3);
    b.Tick(t += 100);  // well before kRemoteHoldMs
    CHECK(b.holds.empty());
    CHECK(Has(b.notes, "let go after"));
}

TEST_CASE("switch share: a new scene forgets what was held") {
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.Tick(t);
    b.sharer.Receive(on, t);
    b.Tick(t += 100, true, "KokiriForest");
    b.Tick(t += 100);
    CHECK(b.holds.empty());
}

TEST_CASE("switch share: a plug that also raises our event asker is held only once its guard flags are on here") {
    // g3-objects, one line per class: a step switch's OnState read by shutters (Jabu-Jabu room 1), by lifts (Deku Tree
    // room D) and by a chest (GTG room 5), and a sun switch's isLightMax (Spirit room 19). Before the presser's guard
    // flag reaches our game our own asker could still fire on the plug (a replay): no hold, and a note.
    struct Line {
        const char* scene;
        const char* name;
        std::array<float, 3> at;
        const char* plug;
    };
    const Line lines[] = {
        {"InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_StepSwitch_A_03", {8.5f, 102.5f, -51.4f}, "OnState"},
        {"InsideTheDekuTree", "Obj_StepSwitch_A_02", {-17.5f, 119.6f, -4.9f}, "OnState"},
        {"GerudoTrainingGround", "Obj_StepSwitch_A_04", {52.2f, 97.5f, -86.0f}, "OnState"},
        {"SpiritTemple", "Obj_SolarSwitch_A_02", {45.8f, 121.5f, -47.5f}, "isLightMax"},
        // review-a38-3: askers with no live place: Forest room 11's step switch (shared guard) and a Dodongo room 17
        // eye switch (its own NaviHelp flag guard)
        {"ForestTemple", "Obj_StepSwitch_A_01", {-74.0f, 125.5f, -1.0f}, "OnState"},
        {"DodongoCavern", "Obj_EyeSwitch_A_05", {65.7f, 121.8f, -23.0f}, "OnState"},
    };
    for (const Line& l : lines) {
        CAPTURE(l.name);
        const auto found = CoopRoomObjects::SwitchPlugsOf(l.scene, l.name, l.at);
        REQUIRE(found.size() == 1);
        REQUIRE_FALSE(found[0]->guard.empty());
        Sharer b;
        std::vector<proto::SwitchState> out;
        std::vector<std::string> notes;
        std::uint64_t t = 1000;
        proto::SwitchState on;
        on.player_number = 1;
        on.scene = l.scene;
        on.name = l.name;
        on.place = l.at;
        on.plug = PlugKey(l.plug);
        on.on = true;
        b.Receive(on, t);
        // objects-next-a-6: the Deku Tree room D guard is read by the room only at its load (Seen::load_on)
        const bool at_load = CoopRoomObjects::GuardFlags(*found[0])[0].at_load;
        CHECK(at_load == (std::string_view(l.scene) == "InsideTheDekuTree"));
        const auto seen = [&](bool plug, bool guard_on) {
            return std::vector<Seen>{Seen{found[0], 0x2000, plug, at_load || guard_on, !at_load || guard_on}};
        };
        // Not armed: nothing held, nothing sent (our plug is 0), the wait is logged once.
        for (int i = 0; i < 5; ++i) {
            const auto holds = b.Tick(l.scene, seen(false, false), true, 2, t += 100, out, notes);
            CHECK(holds.empty());
        }
        CHECK(out.empty());
        CHECK(Has(notes, "waiting for"));
        if (at_load) {
            // The load guard came live: our asker still exists (it is deleted only at a load): still no hold.
            b.Receive(on, t);
            CHECK(b.Tick(l.scene, seen(false, true), true, 2, t += 100, out, notes).empty());
            b.Tick(l.scene, {}, true, 2, t += 100, out, notes);  // the room reloads: a walk without the switch
        }
        // The guard flag came (our asker is gone or its trigger is false): the hold runs the room objects only.
        b.Receive(on, t);
        auto holds = b.Tick(l.scene, seen(false, true), true, 2, t += 100, out, notes);
        REQUIRE(holds.size() == 1);
        CHECK(holds[0] == Hold{0x2000, PlugKey(l.plug)});
        CHECK(Has(notes, "ours held on"));
        // Our plug now 1 from that hold is not sent back as a press of ours.
        holds = b.Tick(l.scene, seen(true, true), true, 2, t += 100, out, notes);
        CHECK(out.empty());
    }
}

namespace {
// The Deku Tree room D compass lifts' plate: its asker is deleted by RoomD_StepSwitch_On_01 only at the room's load.
struct DekuD {
    const CoopRoomObjects::SwitchPlug* line = nullptr;
    Sharer b;
    std::vector<proto::SwitchState> out;
    std::vector<std::string> notes;
    std::uint64_t t = 1000;
    DekuD() {
        const auto found =
            CoopRoomObjects::SwitchPlugsOf("InsideTheDekuTree", "Obj_StepSwitch_A_02", {-17.5f, 119.6f, -4.9f});
        REQUIRE(found.size() == 1);
        line = found[0];
    }
    void Press() {
        proto::SwitchState on;
        on.player_number = 1;
        on.scene = "InsideTheDekuTree";
        on.name = "Obj_StepSwitch_A_02";
        on.place = {-17.5f, 119.6f, -4.9f};
        on.plug = PlugKey("OnState");
        on.on = true;
        b.Receive(on, t);
    }
    // One walk: our switch at `address`, the load guard on in our game now or not.
    bool Held(bool flag_on, std::uint64_t address = 0x2000) {
        Press();
        return !b.Tick("InsideTheDekuTree", {Seen{line, address, false, true, flag_on}}, true, 2, t += 100, out, notes)
                    .empty();
    }
    // One walk of this room without the switch (its room unloaded, or the game is reloading).
    void Gone() { b.Tick("InsideTheDekuTree", {}, true, 2, t += 100, out, notes); }
};
}  // namespace

TEST_CASE("switch share: a load guard that lands mid-visit never arms, a room load with it on does") {
    // objects-next-a-6 (events-next-a-5, Room_D_Prefab_b6c3): the lifts are ORPulse(asker.EventBool, AND(flag,
    // plate)) and the asker's delete reads the flag through UpdateOnce 1. The other player's press lands the flag here
    // mid-visit: our asker still exists, so holding our plate would make it ask the event again (a replay).
    DekuD d;
    CHECK_FALSE(d.Held(false));  // the room loaded with the flag off
    CHECK(Has(d.notes, "load guard off"));
    for (int i = 0; i < 20; ++i) CHECK_FALSE(d.Held(true));  // the flag came live: never armed this visit
    CHECK(Has(d.notes, "waiting for load:InsideTheDekuTree_RoomD_StepSwitch_On_01"));
    // No walk for a long time (a pause menu) is no room load: still not armed.
    d.t += 60000;
    CHECK_FALSE(d.Held(true));
    // The room loads again (a reload, or the player came back after it unloaded): a walk without the switch, then
    // seen with the flag on; our asker was deleted at that load: the hold gives the result.
    d.Gone();
    CHECK(d.Held(true));
    CHECK(Has(d.notes, "load guard on"));
    CHECK(d.Held(true));
}

TEST_CASE("switch share: a load guard on at the room's load arms at once, a new switch actor is a new load") {
    DekuD d;
    CHECK(d.Held(true));  // the save had the flag: the asker never came
    // A new switch actor (the room reloaded from an older save, the flag off): latched again, not held.
    CHECK_FALSE(d.Held(false, 0x5000));
    CHECK_FALSE(d.Held(true, 0x5000));  // and the flag landing live afterwards changes nothing
    // Back to a new load with it on.
    CHECK(d.Held(true, 0x6000));
}

TEST_CASE("switch share: a new scene forgets the load guard's latch") {
    DekuD d;
    CHECK_FALSE(d.Held(false));
    std::vector<proto::SwitchState> out;
    std::vector<std::string> notes;
    d.b.Tick("KokiriForest", {}, true, 2, d.t += 50, out, notes);  // another scene, then back
    CHECK(d.Held(true));
}

TEST_CASE("switch share: a remote press that ends while not armed logs the next wait again") {
    // review-a38-3 nit: told_waiting was reset only once armed, so a second unarmed press went unlogged.
    const auto found = CoopRoomObjects::SwitchPlugsOf("InsideTheDekuTree", "Obj_StepSwitch_A_02", {-17.5f, 119.6f, -4.9f});
    REQUIRE(found.size() == 1);
    Sharer b;
    std::vector<proto::SwitchState> out;
    std::vector<std::string> notes;
    std::uint64_t t = 1000;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = "InsideTheDekuTree";
    on.name = "Obj_StepSwitch_A_02";
    on.place = {-17.5f, 119.6f, -4.9f};
    on.plug = PlugKey("OnState");
    on.on = true;
    const auto waits = [&notes] {
        std::size_t n = 0;
        for (const std::string& s : notes) n += s.find("waiting for") != std::string::npos ? 1 : 0;
        return n;
    };
    const std::vector<Seen> unarmed{Seen{found[0], 0x2000, false, true, false}};  // its load guard off at the load
    b.Receive(on, t);
    b.Tick("InsideTheDekuTree", unarmed, true, 2, t += 100, out, notes);
    b.Tick("InsideTheDekuTree", unarmed, true, 2, t += 100, out, notes);
    CHECK(waits() == 1);
    proto::SwitchState off = on;
    off.on = false;
    b.Receive(off, t);
    b.Tick("InsideTheDekuTree", unarmed, true, 2, t += 100, out, notes);
    CHECK(waits() == 1);
    b.Receive(on, t);
    b.Tick("InsideTheDekuTree", unarmed, true, 2, t += 100, out, notes);
    CHECK(waits() == 2);
}

TEST_CASE("switch share: a guarded line not armed is still our own: our press is sent") {
    // The presser's game: its own guard flag is not on yet when its player steps on (its asker plays the event).
    const auto found = CoopRoomObjects::SwitchPlugsOf("InsideTheDekuTree", "Obj_StepSwitch_A_02", {-17.5f, 119.6f, -4.9f});
    REQUIRE(found.size() == 1);
    Sharer a;
    std::vector<proto::SwitchState> out;
    std::vector<std::string> notes;
    const auto holds = a.Tick("InsideTheDekuTree", {Seen{found[0], 0x3000, true, true, false}}, true, 2, 1000, out, notes);
    CHECK(holds.empty());
    REQUIRE(out.size() == 1);
    CHECK(out[0].on);
    CHECK(out[0].plug == PlugKey("OnState"));
}

TEST_CASE("switch share: two players on one switch keep a third game's hold (row 244)") {
    // piscesgamer97 (0.11.9, Dodongo's Cavern): two others stood on the moving-ground plate; each game held it for
    // the other and sent nothing, so the third game let go every ~5 s and the plate's logic ran again.
    std::array<Game, 3> g;
    for (std::uint8_t i = 0; i < 3; ++i) g[i].self = static_cast<std::uint8_t>(i + 1);
    std::array<bool, 3> pressing{true, true, false};
    std::array<bool, 3> held{};  // our hold keeps the plug 1 from the first set until we let go
    std::array<std::size_t, 3> delivered{};
    std::array<std::size_t, 3> let_go{};
    std::uint64_t t = 1000;
    const auto count = [](const Game& x, const char* text) {
        std::size_t n = 0;
        for (const auto& line : x.notes) n += line.find(text) != std::string::npos ? 1 : 0;
        return n;
    };
    const auto step = [&] {
        t += 100;
        for (std::size_t i = 0; i < 3; ++i) {
            g[i].on = pressing[i] || held[i];
            g[i].Tick(t);
            if (!g[i].holds.empty()) held[i] = true;
            const std::size_t n = count(g[i], "let go after");
            if (n != let_go[i]) held[i] = false;
            let_go[i] = n;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            for (; delivered[i] < g[i].sent.size(); ++delivered[i]) {
                for (std::size_t j = 0; j < 3; ++j) {
                    if (j != i) g[j].sharer.Receive(g[i].sent[delivered[i]], t);
                }
            }
        }
    };
    // Both press in the same walk: both sent before either heard the other.
    step();
    CHECK(Has(g[0].notes, "our player's press: sent"));
    CHECK(Has(g[1].notes, "our player's press: sent"));
    for (int i = 0; i < 300; ++i) {  // 30 s
        step();
        CHECK(held[2]);
    }
    CHECK(count(g[2], "let go after") == 0);
    CHECK(Has(g[0].notes, "ours is resent while held"));
    CHECK_FALSE(Has(g[1].notes, "ours is resent while held"));
    // The lowest number steps off: the other still presses, so the third game is held again after a short handover.
    pressing[0] = false;
    for (int i = 0; i < 100; ++i) step();
    CHECK(held[2]);
    const std::size_t lapses = count(g[2], "let go after");
    for (int i = 0; i < 200; ++i) step();
    CHECK(count(g[2], "let go after") == lapses);  // steady again: no lapse every ~5 s
    // Both off: nothing latches; every game lets go and stays let go.
    pressing[1] = false;
    for (int i = 0; i < 150; ++i) step();
    for (std::size_t i = 0; i < 3; ++i) CHECK_FALSE(held[i]);
    const std::size_t sent = g[0].sent.size() + g[1].sent.size() + g[2].sent.size();
    for (int i = 0; i < 100; ++i) step();
    CHECK(g[0].sent.size() + g[1].sent.size() + g[2].sent.size() == sent);
}

TEST_CASE("switch share: without our number (0) nothing is resent while held, as before") {
    Game a;
    a.self = 0;
    std::uint64_t t = 1000;
    a.on = true;
    a.Tick(t);
    REQUIRE(a.sent.size() == 1);
    proto::SwitchState on = a.sent[0];
    on.player_number = 2;
    for (int i = 0; i < 50; ++i) {
        a.sharer.Receive(on, t);
        a.Tick(t += 100);
    }
    CHECK(a.sent.size() == 1);
}
