// Tests for local co-op's input routing and its game side
// (lichii/hook/coop_local_input.h, coop_local_game.h).
#include <ostream>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "coop_local_game.h"
#include "coop_local_input.h"
#include "coop_local_net.h"
#include "coop_local_probe.h"
#include "coop_player_input.h"

using CoopLocalGame::Facts;
using CoopLocalGame::Pad;
using CoopLocalGame::kKeyboardSlot;
using CoopLocalGame::LocalNumber;
using CoopPuppet::Phase;
using SlotView = CoopPuppet::Director::SlotView;

namespace {

// Guest memory as a map of 8-byte words; anything else is unmapped.
struct Memory {
    std::map<std::uint64_t, std::uint64_t> words;
    bool Read(std::uint64_t address, void* out, std::size_t size) const {
        if (size != 8) return false;
        const auto it = words.find(address);
        if (it == words.end()) return false;
        std::memcpy(out, &it->second, 8);
        return true;
    }
    // An AI node whose actor is `actor`: [[[node+8]+8]+0x1f0].
    void Node(std::uint64_t node, std::uint64_t actor) {
        words[node + 8] = node + 0x1000;
        words[node + 0x1000 + 8] = node + 0x2000;
        words[node + 0x2000 + 0x1f0] = actor;
    }
};

struct World {
    std::vector<Pad> pads{{0, 11, "Pad A", 0}, {1, 12, "Pad B", 0}, {2, 13, "Pad C", 0}};
    std::vector<SlotView> slots;
    std::vector<std::string> log;
    Memory memory;
    bool wraps = true;
    std::vector<std::string> takes;

    CoopLocalGame::Deps Deps() {
        CoopLocalGame::Deps d;
        d.pads = [this] { return pads; };
        d.take = [this](int slot, std::uint32_t serial, int npad) {
            for (Pad& p : pads)
                if (p.slot == slot && p.serial == serial) {
                    p.npad = npad;
                    takes.push_back(std::to_string(slot) + "->" + std::to_string(npad));
                    return true;
                }
            return false;
        };
        d.wraps = [this] { return wraps; };
        d.slots = [this] { return slots; };
        d.read = [this](std::uint64_t a, void* out, std::size_t n) { return memory.Read(a, out, n); };
        d.log = [this](const std::string& line) { log.push_back(line); };
        return d;
    }
    void Live(std::uint8_t number, std::uint64_t actor) {
        SlotView s;
        s.number = number;
        s.phase = Phase::live;
        s.actor = actor;
        slots.push_back(s);
    }
};

Facts InPlay() {
    Facts f;
    f.game = true;
    f.position = CoopPuppet::Vec3{10.0f, 0.0f, 20.0f};
    return f;
}

CoopLocal::Seat SeatOn(int device) {
    CoopLocal::Seat s;
    s.device = device;
    return s;
}

}  // namespace

TEST_CASE("local input: the manager's controller slots are where getController reads them") {
    using CoopLocalInput::ControllerSlot;
    CHECK(ControllerSlot(0) == 0xb0);
    CHECK(ControllerSlot(1) == 0x110);
    CHECK(ControllerSlot(2) == 0x118);
    CHECK(ControllerSlot(3) == 0x120);
    CHECK(ControllerSlot(4) == 0xb8);
    CHECK(ControllerSlot(9) == 0x110);
    CHECK(CoopLocalInput::ControllerFor(1) == 1);
    CHECK(CoopLocalInput::ControllerFor(3) == 3);
}

TEST_CASE("local input: players are found by their actor, 0 for anyone else") {
    CoopLocalInput::Players players;
    CHECK_FALSE(players.Any());
    players.Set(1, 0x1000);
    players.Set(3, 0x3000);
    players.Set(4, 0x4000);   // (no such player: ignored)
    players.Set(0, 0x5000);
    CHECK(players.Any());
    CHECK(players.Of(0x1000) == 1);
    CHECK(players.Of(0x3000) == 3);
    CHECK(players.Of(0x4000) == 0);
    CHECK(players.Of(0x5000) == 0);
    CHECK(players.Of(0) == 0);
    players.Set(1, 0);
    CHECK(players.Of(0x1000) == 0);
    players.Clear();
    CHECK_FALSE(players.Any());
}

TEST_CASE("local input: only an extra player's read of controller 0 is turned, and only to a controller the game has") {
    CoopLocalInput::Thread t;
    const auto all = [](std::uint32_t) { return true; };
    const auto none = [](std::uint32_t) { return false; };
    CHECK_FALSE(t.ControllerGet(0, all).has_value());   // nobody reading
    t.Read(0, 0);                                      // the first Link reads
    CHECK_FALSE(t.ControllerGet(0, all).has_value());
    t.Read(1, 0);
    t.Read(0, 2);                                      // local player 2 reads
    CHECK(t.ControllerGet(0, all) == 2u);
    CHECK_FALSE(t.ControllerGet(1, all).has_value());  // (another index asked: left alone)
    CHECK_FALSE(t.ControllerGet(0, none).has_value()); // the game has no controller 2
    t.Read(1, 0);
    CHECK(t.Reading() == 0);
    CHECK_FALSE(t.ControllerGet(0, all).has_value());  // a menu's read after it: controller 0
}

TEST_CASE("local game: devices are the keyboard and the pads, supported only with the pads and the wraps") {
    World w;
    CoopLocalGame::Game game(w.Deps());
    CHECK(game.Supported());
    const auto devices = game.Devices();
    REQUIRE(devices.size() == 4);
    CHECK(devices[0].slot == kKeyboardSlot);
    CHECK(devices[0].keyboard);
    CHECK(devices[2].name == "Pad B");
    w.wraps = false;
    CHECK_FALSE(game.Supported());
    CoopLocalGame::Deps none;
    CoopLocalGame::Game bare(std::move(none));
    CHECK_FALSE(bare.Supported());
    CHECK(bare.Devices().size() == 1);
}

TEST_CASE("local game: a second player takes its pad, is planned beside the first Link and is made by the Director") {
    World w;
    CoopLocalGame::Game game(w.Deps());
    CHECK_FALSE(game.InPlay());
    game.See(InPlay());
    CHECK(game.InPlay());
    REQUIRE(game.Add(1, SeatOn(1)));
    CHECK(w.takes == std::vector<std::string>{"1->1"});
    CoopPuppet::Input input;
    game.AddTargets(input);
    REQUIRE(input.targets.size() == 1);
    const auto& t = input.targets[0];
    CHECK(t.number == LocalNumber(1));
    CHECK(t.number == 17);
    CHECK(t.actor == "Player_Child");
    CHECK(t.position == CoopPuppet::Vec3{11.0f, 0.0f, 20.0f});
    CHECK(t.color.has_value());
    CHECK(game.Added(1) == CoopLocal::Game::Made::Pending);
    w.Live(17, 0xabc0);
    CHECK(game.Added(1) == CoopLocal::Game::Made::Yes);
    CHECK(game.Players().Of(0xabc0) == 1);
}

TEST_CASE("local game: the keyboard, a missing pad or no wraps cannot make a second player") {
    World w;
    CoopLocalGame::Game game(w.Deps());
    game.See(InPlay());
    CHECK_FALSE(game.Add(1, SeatOn(kKeyboardSlot)));
    CHECK_FALSE(game.Add(1, SeatOn(7)));
    CHECK_FALSE(game.Add(0, SeatOn(1)));
    CHECK_FALSE(game.Add(4, SeatOn(1)));
    w.wraps = false;
    CHECK_FALSE(game.Add(1, SeatOn(1)));
    CHECK(w.takes.empty());
    CoopPuppet::Input input;
    game.AddTargets(input);
    CHECK(input.targets.empty());
}

TEST_CASE("local game: local players' numbers are past every session number") {
    for (int k = 1; k <= CoopLocalInput::kMostExtra; k++) {
        CHECK(LocalNumber(k) > coop::protocol::kMaxPlayers);
        CHECK(LocalNumber(k) < 32);
    }
    CHECK(LocalNumber(1) != LocalNumber(2));
}

TEST_CASE("local game: removing gives the pad back and waits for the Director to delete the Player") {
    World w;
    CoopLocalGame::Game game(w.Deps());
    game.See(InPlay());
    REQUIRE(game.Add(1, SeatOn(2)));
    w.Live(17, 0x7000);
    REQUIRE(game.Added(1) == CoopLocal::Game::Made::Yes);
    CHECK_FALSE(game.Remove(1));   // still live
    CHECK(w.takes == std::vector<std::string>{"2->1", "2->0"});
    CHECK(game.Players().Of(0x7000) == 0);
    CoopPuppet::Input input;
    game.AddTargets(input);
    CHECK(input.targets.empty());   // no longer planned: the Director deletes it
    w.slots[0].phase = Phase::idle;
    CHECK(game.Remove(1));
    CHECK(w.takes.size() == 2);     // (given back once)
}

TEST_CASE("local game: a new scene's Player is followed, and a changed device is taken in its place") {
    World w;
    CoopLocalGame::Game game(w.Deps());
    game.See(InPlay());
    REQUIRE(game.Add(1, SeatOn(0)));
    w.Live(17, 0x1111);
    game.See(InPlay());
    CHECK(game.Players().Of(0x1111) == 1);
    w.slots[0].actor = 0x2222;   // made again in the next scene
    game.See(InPlay());
    CHECK(game.Players().Of(0x1111) == 0);
    CHECK(game.Players().Of(0x2222) == 1);
    CoopLocal::Status status;
    status.state = CoopLocal::State::On;
    status.players = 2;
    status.seat[0] = SeatOn(kKeyboardSlot);
    status.seat[1] = SeatOn(2);
    status.seat[1].color = 0x123456u;
    game.Apply(status);
    CHECK(w.takes == std::vector<std::string>{"0->1", "0->0", "2->1"});
    CoopPuppet::Input input;
    game.AddTargets(input);
    REQUIRE(input.targets.size() == 1);
    CHECK(input.targets[0].color == 0x123456u);
}

TEST_CASE("local game: the wraps turn only the extra player's controller reads, and only to a controller that exists") {
    World w;
    CoopLocalGame::Game game(w.Deps());
    game.See(InPlay());
    REQUIRE(game.Add(1, SeatOn(1)));
    w.Live(17, 0x9000);
    game.See(InPlay());
    w.memory.Node(0x100000, 0x9000);   // the extra player's AI node
    w.memory.Node(0x200000, 0x8000);   // the first Link's
    const std::uint64_t manager = 0x300000;
    w.memory.words[manager + 0x110] = 0x310000;   // controller 1 exists

    std::uint64_t x[32] = {};
    const auto read_and_ask = [&](std::uint64_t node, std::uint32_t wrap) {
        x[19] = node;
        game.Wrap(wrap, 0, x);
        std::uint64_t get[32] = {};
        get[0] = manager;
        get[1] = 0xdead'0000'0000ull;   // (the upper half is not the index: kept)
        get[2] = 0x1234;
        game.Wrap(CoopPlayerInput::kWrapControllerGet, 0, get);
        game.Wrap(CoopPlayerInput::kWrapControllerGet, 1, get);
        game.Wrap(wrap, 1, x);
        return get[1];
    };
    CHECK(read_and_ask(0x100000, CoopPlayerInput::kWrapButtons) == 0xdead'0000'0001ull);
    CHECK(read_and_ask(0x100000, CoopPlayerInput::kWrapSticks) == 0xdead'0000'0001ull);
    CHECK(read_and_ask(0x200000, CoopPlayerInput::kWrapButtons) == 0xdead'0000'0000ull);
    CHECK(read_and_ask(0x400000, CoopPlayerInput::kWrapButtons) == 0xdead'0000'0000ull);   // (unmapped node)
    // After the read: a menu's controller 0 is controller 0.
    std::uint64_t menu[32] = {};
    menu[0] = manager;
    game.Wrap(CoopPlayerInput::kWrapControllerGet, 0, menu);
    CHECK(menu[1] == 0);
    // No controller 1 in the manager: the Player keeps controller 0, said once.
    w.memory.words.erase(manager + 0x110);
    CHECK(read_and_ask(0x100000, CoopPlayerInput::kWrapButtons) == 0xdead'0000'0000ull);
    CHECK(read_and_ask(0x100000, CoopPlayerInput::kWrapButtons) == 0xdead'0000'0000ull);
    int said = 0;
    for (const auto& line : w.log) said += line.find("has no controller 1") != std::string::npos;
    CHECK(said == 1);
}

namespace {

// Guest memory as bytes at addresses, for the probe.
struct Bytes {
    std::map<std::uint64_t, std::uint8_t> at;
    void Put(std::uint64_t address, const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < n; i++) at[address + i] = p[i];
    }
    void Word(std::uint64_t address, std::uint64_t v) { Put(address, &v, 8); }
    bool Read(std::uint64_t address, void* out, std::size_t n) const {
        auto* p = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; i++) {
            const auto it = at.find(address + i);
            p[i] = it == at.end() ? 0 : it->second;   // (unwritten bytes read as 0, as a zeroed heap)
        }
        return true;
    }
};

}  // namespace

TEST_CASE("local probe: each layer's name, camera and viewport-looking floats are logged") {
    namespace a = CoopLocalProbe::addr;
    const std::uint64_t main = 0x80020000;
    Bytes m;
    const std::uint64_t owner = 0x10000000, array = 0x10001000, layer = 0x10002000, camera = 0x10003000;
    m.Word(main + a::kLayers, owner);
    m.Word(owner, main + 0x123450);
    const std::uint32_t count = 2;
    m.Put(owner + a::kLayerCount, &count, 4);
    m.Word(owner + a::kLayerArray, array);
    m.Word(array, layer);   // id 0; id 1 empty
    m.Word(layer, main + 0x200000);
    m.Word(layer + a::kLayerCamera, camera);
    m.Word(camera, main + a::kLookAtCamera);
    m.Put(layer + a::kLayerName, "Main_3D_0", 10);
    const float viewport[4] = {0.0f, 0.0f, 1920.0f, 1080.0f};
    m.Put(layer + 0x300, viewport, sizeof viewport);
    const auto read = [&m](std::uint64_t ad, void* out, std::size_t n) { return m.Read(ad, out, n); };
    const auto lines = CoopLocalProbe::Layers(read, main);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("count 2") != std::string::npos);
    CHECK(lines[0].find("vtable main+0x123450") != std::string::npos);
    CHECK(lines[1].find("layer 0 ") == 0);
    CHECK(lines[1].find("\"Main_3D_0\"") != std::string::npos);
    CHECK(lines[1].find("(LookAtCamera)") != std::string::npos);
    CHECK(lines[1].find("+0x300 (0 0 1920 1080)") != std::string::npos);
}

TEST_CASE("local probe: a name behind a string object, the controllers, and no owner") {
    namespace a = CoopLocalProbe::addr;
    const std::uint64_t main = 0x80020000;
    Bytes m;
    const auto read = [&m](std::uint64_t ad, void* out, std::size_t n) { return m.Read(ad, out, n); };
    CHECK(CoopLocalProbe::Layers(read, main) == std::vector<std::string>{"layers: no owner at main+0x4e81cf0"});
    CHECK(CoopLocalProbe::Controllers(read, main) ==
          std::vector<std::string>{"controllers: no manager at main+0x4e92f10"});
    const std::uint64_t layer = 0x20000000;
    m.Word(layer + a::kLayerName, main + 0x777000);   // (a vtable: no text)
    m.Word(layer + a::kLayerName + 8, 0x20100000);
    m.Put(0x20100000, "UI_2D", 6);
    CHECK(CoopLocalProbe::LayerName(read, layer) == "UI_2D");
    CHECK(CoopLocalProbe::LayerName(read, 0x30000000) == "?");
    const std::uint64_t manager = 0x40000000;
    m.Word(main + a::kControllers, manager);
    m.Word(manager + 0xb0, 0x40100000);
    m.Word(manager + 0x110, 0x40200000);
    m.Word(0x40200000, main + 0x500);
    const auto lines = CoopLocalProbe::Controllers(read, main);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("0: 0x40100000") != std::string::npos);
    CHECK(lines[0].find("1: 0x40200000 vtable main+0x500") != std::string::npos);
    CHECK(lines[0].find("2: 0 ") != std::string::npos);
}

TEST_CASE("local probe: what looks like a viewport") {
    const float whole[4] = {0, 0, 1280, 720};
    const float unit[4] = {0, 0, 1, 1};
    const float half[4] = {640, 0, 640, 720};
    const float small[4] = {0, 0, 8, 8};
    const float fraction[4] = {0, 0, 1280.5f, 720};
    const float negative[4] = {-1, 0, 1280, 720};
    CHECK(CoopLocalProbe::LooksLikeViewport(whole));
    CHECK(CoopLocalProbe::LooksLikeViewport(unit));
    CHECK(CoopLocalProbe::LooksLikeViewport(half));
    CHECK_FALSE(CoopLocalProbe::LooksLikeViewport(small));
    CHECK_FALSE(CoopLocalProbe::LooksLikeViewport(fraction));
    CHECK_FALSE(CoopLocalProbe::LooksLikeViewport(negative));
}

namespace {

struct Wire {
    std::vector<coop::protocol::Message> sent;
    CoopLocalNet::Seats::Send Send() {
        return [this](const coop::protocol::Message& m) {
            sent.push_back(m);
            return true;
        };
    }
};

std::vector<CoopLocalGame::SeatNow> OneSeat(const std::string& name = "Saria") {
    return {{1, 0x1000, name, std::nullopt}};
}

}  // namespace

TEST_CASE("local net: a seat in play asks for a number once connected, again every 3 s until answered") {
    using coop::protocol::LocalJoin;
    CoopLocalNet::Seats seats;
    Wire w;
    seats.Frame(false, OneSeat(), 1000, w.Send());
    CHECK(w.sent.empty());   // not connected: nothing
    seats.Frame(true, OneSeat(), 1000, w.Send());
    REQUIRE(w.sent.size() == 1);
    CHECK(w.sent[0] == coop::protocol::Message{LocalJoin{1, "Saria"}});
    seats.Frame(true, OneSeat(), 2000, w.Send());
    CHECK(w.sent.size() == 1);
    seats.Frame(true, OneSeat(), 4000, w.Send());
    CHECK(w.sent.size() == 2);   // asked again
    seats.Answer({1, 7, coop::protocol::RefuseReason::server_full});
    CHECK(seats.Number(1) == 7);
    seats.Frame(true, OneSeat(), 9000, w.Send());
    CHECK(w.sent.size() == 2);   // in: no more asking
    CHECK_FALSE(seats.Number(2).has_value());
}

TEST_CASE("local net: a seat out of play leaves, a refusal stops asking until the next connection") {
    using coop::protocol::LocalLeave;
    std::vector<std::string> log;
    CoopLocalNet::Seats seats([&log](const std::string& line) { log.push_back(line); });
    Wire w;
    seats.Frame(true, OneSeat(), 0, w.Send());
    seats.Answer({1, 7, coop::protocol::RefuseReason::server_full});
    seats.Frame(true, {}, 100, w.Send());
    REQUIRE(w.sent.size() == 2);
    CHECK(w.sent[1] == coop::protocol::Message{LocalLeave{1}});
    CHECK_FALSE(seats.Number(1).has_value());
    // In again, refused: no more asking on this connection.
    w.sent.clear();
    seats.Frame(true, OneSeat("Navi"), 200, w.Send());
    seats.Answer({1, 0, coop::protocol::RefuseReason::name_taken});
    seats.Frame(true, OneSeat("Navi"), 9000, w.Send());
    CHECK(w.sent.size() == 1);
    CHECK(log.back().find("plays here only: its name is taken") != std::string::npos);
    // A new connection: asks again.
    seats.Welcomed();
    seats.Frame(true, OneSeat("Navi"), 9100, w.Send());
    CHECK(w.sent.size() == 2);
}

TEST_CASE("local net: a number for a seat that left meanwhile is given back, and a reconnect forgets numbers") {
    CoopLocalNet::Seats seats;
    Wire w;
    seats.Frame(true, OneSeat(), 0, w.Send());   // asks
    seats.Frame(true, {}, 10, w.Send());         // left play before the answer
    CHECK(w.sent.size() == 1);
    seats.Answer({1, 9, coop::protocol::RefuseReason::server_full});
    CHECK_FALSE(seats.Number(1).has_value());
    seats.Frame(true, {}, 20, w.Send());
    REQUIRE(w.sent.size() == 2);
    CHECK(w.sent[1] == coop::protocol::Message{coop::protocol::LocalLeave{1}});
    // Reconnect: the number is gone, asked for again.
    seats.Frame(true, OneSeat(), 30, w.Send());
    seats.Answer({1, 4, coop::protocol::RefuseReason::server_full});
    CHECK(seats.Number(1) == 4);
    seats.Welcomed();
    CHECK_FALSE(seats.Number(1).has_value());
    // Wrap: only a joined seat's own kinds.
    seats.Answer({1, 4, coop::protocol::RefuseReason::server_full});
    const auto wrapped = seats.Wrap(1, coop::protocol::PlayerStatus{0, 0});
    REQUIRE(wrapped.has_value());
    CHECK(std::holds_alternative<coop::protocol::LocalMessage>(*wrapped));
    CHECK_FALSE(seats.Wrap(1, coop::protocol::Chat{0, "hi"}).has_value());
    CHECK_FALSE(seats.Wrap(2, coop::protocol::PlayerStatus{0, 0}).has_value());
}

TEST_CASE("local game: a pad that reconnects into its slot is taken again for its player") {
    World w;
    CoopLocalGame::Game game(w.Deps());
    game.See(InPlay());
    REQUIRE(game.Add(1, SeatOn(1)));
    CHECK(w.pads[1].npad == 1);
    game.See(InPlay());
    CHECK(w.takes.size() == 1);   // (nothing changed: nothing taken)
    // Hookshot: a new opening in slot 1, given back to player 1's pad.
    w.pads[1].serial = 99;
    w.pads[1].npad = 0;
    game.See(InPlay());
    CHECK(w.takes == std::vector<std::string>{"1->1", "1->1"});
    CHECK(w.pads[1].npad == 1);
    game.See(InPlay());
    CHECK(w.takes.size() == 2);
    // Gone from its slot: nothing to take until it is back.
    const Pad gone = w.pads[1];
    w.pads.erase(w.pads.begin() + 1);
    game.See(InPlay());
    CHECK(w.takes.size() == 2);
    w.pads.push_back({1, 100, gone.name, 0});
    game.See(InPlay());
    CHECK(w.takes.size() == 3);
}

TEST_CASE("local game: an extra player's Player whose controller is missing gets input off, never player 1's pad") {
    World w;
    const std::uint64_t main = 0x80000000;
    auto deps = w.Deps();
    deps.main_base = [main] { return main; };
    CoopLocalGame::Game game(std::move(deps));
    game.See(InPlay());
    REQUIRE(game.Add(1, SeatOn(1)));
    w.Live(17, 0x9000);
    game.See(InPlay());
    w.memory.Node(0x100000, 0x9000);   // the extra player's node
    w.memory.Node(0x200000, 0x8000);   // the first Link's
    const std::uint64_t manager = 0x300000;
    w.memory.words[main + CoopLocalInput::addr::kControllerManager] = manager;
    const auto off = [&](std::uint64_t node) {
        std::uint64_t x[32] = {};
        x[19] = node;
        const int taken = game.Wrap(CoopPlayerInput::kWrapInputOff, 0, x);
        x[0] = 0;   // (what the game's check would have said)
        game.Wrap(CoopPlayerInput::kWrapInputOff, 1, x);
        return std::make_pair(taken, x[0]);
    };
    CHECK(off(0x100000) == std::make_pair(1, std::uint64_t{1}));   // no controller 1: off
    CHECK(off(0x200000) == std::make_pair(0, std::uint64_t{0}));   // the first Link: the game decides
    w.memory.words[manager + 0x110] = 0x310000;                    // controller 1 is there
    CHECK(off(0x100000) == std::make_pair(0, std::uint64_t{0}));
}
