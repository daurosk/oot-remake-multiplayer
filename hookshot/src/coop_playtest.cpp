// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge in the game (coop_playtest.h; docs/playtest.md, "Bridge protocol").
//
// One thread of its own: it listens on 127.0.0.1:<LICHII_COOP_PLAYTEST_PORT>, serves one client at a time, and
// answers each request in order. What it does through others:
// - the hold: the Gate (coop_playtest_core.h), which the game's calc thread waits in at the frame hook;
// - input, shots and Link's snapshot: Longshot's own control bridge (HOS_CONTROL), as a client of it, the
//   controller's own path (an input lands at the game's controller read, the frame after it is sent);
// - warp, flag, give: the module's test commands (CoopTestInput::PlayNow), the game's own sequence and flag
//   setters and the cheats panel's give, on the game's calc thread inside the hold;
// - dump: the shared flags (CoopTestInput::SharedFlags) and the state, as one JSON file in <game>/playtest/dumps.
// - flag_read: one flag by name (CoopTestInput::ReadFlag), read only, inside the hold.
// A client that leaves (closed, crashed) lets a held game go at once.
#include "coop_playtest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>


#include "common/logging.h"
#include "coop/host/game_items.hpp"
#include "coop_camera.h"
#include "coop_cheats.h"
#include "coop_fairy.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_map_drawn.h"
#include "coop_menu_state.h"
#include "coop_pause.h"
#include "coop_pause_tab.h"
#include "coop_playtest_core.h"
#include "coop_talk_text.h"
#include "coop_teleport_game.h"
#include "coop_testinput.h"
#include "hos_coop_pane_body.h"
#include "hos_coop_pane_text.h"
#include "core/core.h"

// (the platform's sockets and pipe after our own headers, as coop_lan_net.cpp: no Windows macro reaches them)
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace CoopPlaytest {
namespace {

using namespace std::chrono_literals;
using CoopModJson::Value;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_stop{false};
std::unique_ptr<Gate> g_gate;
MoveHold g_move;  // `move`: the world yaw the pad hook turns into the stick (PadMove)
std::thread g_thread;
Core::System* g_system = nullptr;
std::filesystem::path g_dir;  // <game>/playtest
SocketHandle g_listener = kNoSocket;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

void CloseSocket(SocketHandle s) {
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool SendAll(SocketHandle s, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
#ifdef _WIN32
        const int n = send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0);
#else
        const ssize_t n = send(s, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// 1: readable, 0: not within ms, -1: error.
int WaitReadable(SocketHandle s, int ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int r = select(0, &set, nullptr, nullptr, &tv);
#else
    pollfd p{s, POLLIN, 0};
    const int r = poll(&p, 1, ms);
#endif
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

// ---- Longshot's control bridge, as its client ----

class Control {
public:
    ~Control() { Close(); }

    bool Open() {
        if (Connected()) return true;
        const std::string where = Env("HOS_CONTROL");
        if (where.empty()) return false;
#ifdef _WIN32
        const std::string name = where.rfind("\\\\.\\pipe\\", 0) == 0 ? where : "\\\\.\\pipe\\" + where;
        pipe_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) return false;
#else
        sock_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_ < 0) return false;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (where.size() >= sizeof a.sun_path) return Close(), false;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", where.c_str());
        if (connect(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return Close(), false;
#endif
        buffer_.clear();
        return true;
    }

    bool Connected() const {
#ifdef _WIN32
        return pipe_ != INVALID_HANDLE_VALUE;
#else
        return sock_ >= 0;
#endif
    }

    void Close() {
#ifdef _WIN32
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
#else
        if (sock_ >= 0) close(sock_);
        sock_ = -1;
#endif
    }

    // Sends a request; its id is returned (replies are matched by it).
    std::optional<int> Send(const std::string& body_without_id_open) {
        if (!Open()) return std::nullopt;
        const int id = ++next_id_;
        const std::string line = "{\"id\":" + std::to_string(id) + "," + body_without_id_open + "\n";
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()), &n, nullptr) || n != line.size()) {
            Close();
            return std::nullopt;
        }
#else
        std::size_t sent = 0;
        while (sent < line.size()) {
            const ssize_t n = send(sock_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return Close(), std::nullopt;
            sent += static_cast<std::size_t>(n);
        }
#endif
        return id;
    }

    // Sends a whole request line someone made (ControlInput) under its own id.
    bool SendLine(const std::string& line) {
        if (!Open()) return false;
        const std::string l = line + "\n";
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile(pipe_, l.data(), static_cast<DWORD>(l.size()), &n, nullptr) || n != l.size()) return Close(), false;
#else
        if (send(sock_, l.data(), l.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(l.size())) return Close(), false;
#endif
        return true;
    }

    int NextId() { return ++next_id_; }

    // The reply to `id` (event lines and other replies skipped), within ms.
    std::optional<Value> Await(int id, int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (Connected()) {
            for (std::size_t nl; (nl = buffer_.find('\n')) != std::string::npos;) {
                const std::string line = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                auto v = CoopModJson::Parse(line);
                if (v && v->Number("id") && static_cast<int>(*v->Number("id")) == id) return v;
            }
            if (std::chrono::steady_clock::now() >= until) return std::nullopt;
            if (!ReadSome(20)) return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<Value> Call(const std::string& body, int ms = 2000) {
        const auto id = Send(body);
        if (!id) return std::nullopt;
        return Await(*id, ms);
    }

private:
    // Reads what is there (waiting up to ms); false when the connection broke.
    bool ReadSome(int ms) {
        char buf[16384];
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return Close(), false;
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms > 5 ? 5 : ms));
            return true;
        }
        DWORD n = 0;
        if (!ReadFile(pipe_, buf, avail < sizeof buf ? avail : sizeof buf, &n, nullptr)) return Close(), false;
        buffer_.append(buf, n);
#else
        const int r = WaitReadable(sock_, ms);
        if (r < 0) return Close(), false;
        if (r == 0) return true;
        const ssize_t n = recv(sock_, buf, sizeof buf, 0);
        if (n <= 0) return Close(), false;
        buffer_.append(buf, static_cast<std::size_t>(n));
#endif
        if (buffer_.size() > 8 * 1024 * 1024) buffer_.clear();
        return true;
    }

#ifdef _WIN32
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
#else
    int sock_ = -1;
#endif
    std::string buffer_;
    int next_id_ = 0;
};

Control g_control;  // (the bridge's thread only)
EnumValues g_enum_values;   // the enum values flag_read read: the only hashes `flag` writes back
bool g_verbs_runner = false, g_verbs_cheats = false;   // the gate for act, flag, warp and give (set at Start)

// ---- the state ----

std::string Field(const Value* game, const char* key) {
    const Value* v = game ? game->Get(key) : nullptr;
    return v ? Compact(*v) : std::string("null");
}

std::uint64_t BridgeFrame() { return g_gate ? g_gate->Frame() : 0; }
std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The world list (docs/playtest.md, "State"). Held: the game stands at a frame, so wait up to 300 ms for the object
// walk (the present thread keeps running) to publish a snapshot of that frame, but only when one is coming (there is a
// fresh one, behind the frame); with none (solo) or a stale one it does not wait. Running: the newest one, with its
// frame. `full`: the dump's whole list; else the nearest 40 known kinds.
std::string WorldText(bool full) {
    auto& store = CoopWorldSnapshot::TheStore();
    if (!store.Enabled()) return "";
    const std::uint64_t now = g_gate ? g_gate->Frame() : 0;
    auto snap = store.Get();
    if (g_gate && g_gate->Held()) {
        const auto until = std::chrono::steady_clock::now() + 300ms;
        while (CoopWorldSnapshot::WorthWaiting(snap, now, SteadyMs()) && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(10ms);
            snap = store.Get();
        }
    }
    return WorldMember(true, snap, now, SteadyMs(), full);
}

// Whether the objects walk has a usable world list now (a snapshot that is not stale): false in solo.
bool WorldListNow() {
    auto& store = CoopWorldSnapshot::TheStore();
    return store.Enabled() && WorldListLive(store.Get(), SteadyMs(), g_gate ? g_gate->Frame() : 0);
}

// Whether the sharing walk (the one that runs strikes) made the list: a live list that is not the solo walk's.
bool StrikeWalkNow() {
    auto& store = CoopWorldSnapshot::TheStore();
    if (!store.Enabled()) return false;
    const auto snap = store.Get();
    return WorldListLive(snap, SteadyMs(), g_gate ? g_gate->Frame() : 0) && !snap->solo;
}

constexpr auto kHoldWait = 5000ms;  // a hold (or a held job) that does not come in this time: timeout

// Longshot's status with a Link snapshot that is not old (SnapshotIsOld) and not missing (SnapshotMissing). Longshot
// takes that snapshot only in a running Link update while a status is asked, so a held game gives the one from its last
// such status, and a game held since its start has none at all (the first state on a fresh hold). Old or missing and
// held: ask a status, let ONE frame run (its Link update takes the snapshot) and read that reply. Still none after that
// (a title, a pause, a load: no Link update runs): the reply as it is, which LinkMembers shows as null, with
// `snapshot_stale` (old) and `not_ready` (the reason) saying why.
std::optional<Value> LinkStatus() {
    auto status = g_control.Call("\"cmd\":\"status\"}", 1500);
    if (status && (SnapshotIsOld(&*status) || SnapshotMissing(&*status)) && g_gate && g_gate->Held()) {
        const auto id = g_control.Send("\"cmd\":\"status\"}");
        if (id && g_gate->Step(1, kHoldWait)) {
            if (auto fresh = g_control.Await(*id, 1500)) status = std::move(fresh);
        }
    }
    return status;
}

// The state's members (no braces): Longshot's Link snapshot, then the module's equips, inventory and co-op. `status`: a
// status reply already read (input asks it before its last frame); none: LinkStatus() reads one.
std::string StateMembers(bool full_world = false, std::optional<Value> status = std::nullopt) {
    std::string out;
    if (!status) status = LinkStatus();
    const Value* game = status ? status->Get("game") : nullptr;
    if (game && game->kind != Value::Kind::Object) game = nullptr;
    out += "\"scene\":" + (status && status->Get("map") ? Compact(*status->Get("map")) : Field(game, "map"));
    {
        // The rooms table (the puppet pack's coop_rooms.txt), read once; empty without a pack (room null).
        static const CoopRooms::Table rooms = CoopFairy::RoomTable();
        std::string scene;
        if (status) {
            if (const auto map = status->Text("map")) scene = *map;
            else if (game) scene = game->Text("map").value_or("");
        }
        out += RoomMember(status ? &*status : nullptr, scene, rooms);
    }
    out += LinkMembers(status ? &*status : nullptr);
    {
        auto& store = CoopWorldSnapshot::TheStore();
        out += BodyMember(store.Enabled() ? store.Get() : std::nullopt, WorldListNow());
    }
    {
        const auto camera = CoopCamera::Latest();
        out += ViewMember(camera ? std::optional<std::array<float, 16>>(camera->clip_from_world) : std::nullopt,
                          camera && camera->y_down);
    }
    out += ",\"control\":" + std::string(status ? "true" : "false");
    Core::System& sys = *g_system;
    out += ",\"equips\":{";
    bool first = true;
    for (const char* slot : {"weapon", "shield", "head", "armor", "boots"}) {
        const auto flag = CoopTestInput::WearFlag(slot);
        const auto v = flag ? CoopTestInput::FlagText(sys, "PlayerStatus." + *flag) : std::nullopt;
        out += std::string(first ? "" : ",") + "\"" + slot + "\":" + (v ? Quote(*v) : "null");
        first = false;
    }
    out += "},\"inventory\":{";
    first = true;
    // Every item flag, the per-player counts too (bombs, arrows, seeds, rupees): the panel's list leaves those out.
    for (const auto& item : coop::host::item_flags()) {
        if (!item.on) continue;
        const auto v = CoopTestInput::FlagInt(sys, item.name);
        if (!v) continue;
        out += std::string(first ? "" : ",") + Quote(std::string(item.name)) + ":" + std::to_string(*v);
        first = false;
    }
    out += "},\"coop\":{\"players\":[";
    first = true;
    for (const auto& p : CoopHook::PlayerList()) {
        out += std::string(first ? "" : ",") + "{\"number\":" + std::to_string(p.number) + ",\"name\":" +
               Quote(p.name) + ",\"self\":" + (p.self ? "true" : "false") + ",\"state\":" + Quote(p.state) +
               ",\"scene\":" + (p.scene ? Quote(*p.scene) : "null") + "}";
        first = false;
    }
    out += "]}";
    out += ",\"coop_menu\":" + Quote(CoopPause::MenuState());
    out += ",\"menu\":" + CoopMenuState::Json(CoopPause::MenuTab(), CoopPauseTab::CustomizeScreen());
    {
        std::uint64_t presents = 0;
        const CoopMapDrawn::Frame drawn = CoopMapDrawn::Last(&presents);
        out += ",\"map_icons\":" + CoopMapDrawn::Json(drawn, presents);
    }
    out += WorldText(full_world);
    out += StrikeMember(CoopStrike::TheBook().Last());
    return out;
}

std::string StateObject(bool full_world = false, std::optional<Value> status = std::nullopt) {
    return "{" + StateMembers(full_world, std::move(status)) + "}";
}

// ---- the commands ----

std::string Handle(const Request& r) {
    Gate& gate = *g_gate;
    gate.Touch();
    const auto ok = [&](const std::string& rest) { return Reply(r.id, gate.Frame(), gate.Held(), rest); };
    const auto fail = [&](std::string_view code, std::string_view why) {
        return ErrorReply(r.id, gate.Frame(), gate.Held(), code, why);
    };
    // `asked`: the strike numbers before and after the lines ran, read inside the held call (so a script that queues a
    // strike between two calls cannot give an act with no strike a number).
    struct Asked {
        std::uint64_t before = 0, after = 0;
    };
    const auto played = [&](const std::string& lines, std::string& error,
                            const std::vector<CoopTestInput::Kind>* allowed = nullptr, Asked* asked = nullptr) {
        bool done = false;
        const bool ran = gate.RunHeld(
            [&] {
                if (asked) asked->before = CoopStrike::TheBook().LastAsked();
                done = CoopTestInput::PlayNow(*g_system, lines, error, allowed);
                if (asked) asked->after = CoopStrike::TheBook().LastAsked();
            },
            kHoldWait);
        if (!ran) error = "the game was not held in time (a load, or no frame hook)";
        return ran && done;
    };

    if (r.cmd == "hello") {
        const bool control = g_control.Open();
        return ok("\"protocol\":" + std::to_string(kProtocol) + ",\"control\":" + (control ? "true" : "false"));
    }
    if (r.cmd == "state") return ok(StateMembers());
    if (r.cmd == "pause") {
        return gate.Pause(kHoldWait) ? ok("") : fail("timeout", "no frame came to hold at (a load, or the title)");
    }
    if (r.cmd == "resume") {
        gate.Resume();
        return ok("");
    }
    if (r.cmd == "step") {
        std::uint64_t from = 0;
        if (!gate.Step(r.frames, kHoldWait, &from)) return fail("timeout", "the frames did not run in time");
        return ok("\"from\":" + std::to_string(from));
    }
    // input and move: Longshot's sequence for `frames` frames, those frames run, then held. The status is asked before
    // the last frame: that frame's Link update takes the snapshot, so the state in the reply is Link at the end of the
    // step (a status asked after it would read an old snapshot). Empty on success (`from` and `status` set).
    const auto run_pad = [&](const Request& pad, std::uint64_t& from, std::optional<Value>& status) -> std::string {
        if (!g_control.SendLine(ControlInput(pad, g_control.NextId()))) return fail("no_control", "HOS_CONTROL unset or unreachable");
        if (pad.frames > 1 && !gate.Step(pad.frames - 1, kHoldWait, &from)) {
            return fail("timeout", "the frames did not run in time");
        }
        const auto asked = g_control.Send("\"cmd\":\"status\"}");
        std::uint64_t last = 0;
        if (!gate.Step(1, kHoldWait, &last)) return fail("timeout", "the frames did not run in time");
        if (pad.frames == 1) from = last;
        status = asked ? g_control.Await(*asked, 1500) : std::nullopt;
        return {};
    };
    if (r.cmd == "input") {
        if (!gate.Pause(kHoldWait)) return fail("timeout", "no frame came to hold at");
        std::uint64_t from = 0;
        std::optional<Value> status;
        if (auto failed = run_pad(r, from, status); !failed.empty()) return failed;
        return ok("\"from\":" + std::to_string(from) + ",\"state\":" + StateObject(false, std::move(status)));
    }
    if (r.cmd == "move") {
        // The world yaw becomes the stick at every pad read (PadMove, from hos_coop_pad) with the camera of the last
        // drawn frame; Longshot's sequence only holds the real pad out (sticks centred) for the frames.
        if (!gate.Pause(kHoldWait)) return fail("timeout", "no frame came to hold at");
        const auto camera = CoopCamera::Latest();
        if (!camera || !MoveAxes(camera->clip_from_world, r.yaw, r.strength)) {
            return fail("no_camera", "no game camera to steer by (no frame drawn with the game's camera yet: a title, a "
                                     "load): use input with a stick instead");
        }
        Request pad = r;
        pad.stick.reset(), pad.rstick.reset(), pad.buttons.clear();
        g_move.Start(r.yaw, r.strength);
        std::uint64_t from = 0;
        std::optional<Value> status;
        const auto failed = run_pad(pad, from, status);
        const auto did = g_move.Stop();
        if (!failed.empty()) return failed;
        std::string rest = "\"from\":" + std::to_string(from) + ",\"yaw\":" + Compact(Value::Of(r.yaw)) +
                           ",\"reads\":" + std::to_string(did.reads) + ",\"no_camera\":" + std::to_string(did.no_camera);
        rest += ",\"stick\":";
        if (did.last) {
            const double angle = std::atan2(did.last->x, did.last->y) * 180.0 / 3.14159265358979323846;
            rest += "[" + Compact(Value::Of(std::round((angle < 0 ? angle + 360 : angle) * 100) / 100)) + "," +
                    Compact(Value::Of(std::round(std::hypot(did.last->x, did.last->y) * 1000) / 1000)) + "]";
        } else {
            rest += "null";
        }
        rest += ",\"view\":" + (did.view ? Compact(Value::Of(std::round(*did.view * 10) / 10)) : std::string("null"));
        return ok(rest + ",\"state\":" + StateObject(false, std::move(status)));
    }
    // The commands that change the game reach the wire and other players' games: only when the test runner asked for
    // them (LICHII_COOP_PLAYTEST_VERBS) and cheats are on.
    if (GatedCommand(r.cmd)) {
        const std::string refusal = VerbRefusal(g_verbs_runner, g_verbs_cheats);
        if (!refusal.empty()) return fail("refused", refusal);
    }
    if (r.cmd == "act") {
        std::string error;
        // An actor id is resolved in the sharing walk, which runs only in a session: without its list (none, or the
        // solo walk's), refuse it here (never run it as a name, which would strike another actor).
        const bool listed = StrikeWalkNow();
        if (const std::string refusal = StrikeIdRefusal(r.lines, listed); !refusal.empty()) {
            return fail("refused", refusal);
        }
        Asked asked;
        if (!played(r.lines, error, &CoopTestInput::ActKinds(), &asked)) return fail("failed", error);
        // The strikes this act queued: the walk leaves the result of the newest one in `state.strike` under this number.
        // A kill or hit that got no number found the queue full (16 waiting): nothing was queued.
        if (asked.after == asked.before && HasStrikeLine(r.lines)) {
            return fail("refused", "the strike queue is full (16 wait for the walk): step a few frames and send it again");
        }
        return ok("\"lines\":" + Quote(r.lines) + ",\"note\":\"asked: the game does it on its next frames\"" +
                  (asked.after != asked.before ? ",\"strike_seq\":" + std::to_string(asked.after) : std::string()));
    }
    if (r.cmd == "warp") {
        // Two ways (PlanWarp): while an event runs, the module's own scene request (the game takes it only then,
        // docs/scenes.md "Free-play scene change"); in free play, Longshot's control `teleport` (taken at Link's next
        // update, so frames must run). Refused at once, with the reason, when neither fits.
        const auto now = LinkStatus();
        WarpPlan plan = PlanWarp(now ? &*now : nullptr, !r.entrance.empty(), r.scene);
        if (plan.route == WarpRoute::refuse) return fail(plan.code, plan.why);
        if (!plan.set_age.empty()) {
            // The age first, and frames for the game to take it (the lane recipe: setflag, wait, then warp).
            std::string error;
            if (!played(std::string("setflag Sequence_IsChildhood ") + (plan.set_age == "child" ? "1" : "0"), error)) return fail("failed", error);
            if (!gate.Step(30, kHoldWait)) return fail("timeout", "the frames did not run in time");
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(r.timeout_s);
        std::string wait_for = r.scene;   // the map to wait for: the teleport route takes Longshot's (Link's age, its spelling)
        // Free play, an entrance or a map Longshot's list lacks (a boss stage): the game's own map change event
        // (CoopTeleport::MapChange). Refused with the reason when the scene or start is not one the game has.
        const auto ask_map_event = [&]() -> std::string {
            if (const std::string problem = CoopTestInput::WarpProblem(r.scene, r.entrance); !problem.empty()) return problem;
            if (!CoopTeleport::MapChange(r.scene, r.entrance)) {
                return "the map change event is off (LICHII_COOP_TELEPORT=0 or LICHII_COOP_TELEPORT_EVENT=0) or " + r.scene +
                       " has no start point in the module's list: give an entrance";
            }
            return {};
        };
        if (plan.route == WarpRoute::map_event) {
            if (const std::string why = ask_map_event(); !why.empty()) return fail("failed", why);
        } else if (plan.route == WarpRoute::event) {
            std::string error;
            const std::string line = "warp " + r.scene + (r.entrance.empty() ? "" : " " + r.entrance);
            if (!played(line, error)) return fail("failed", error);
        } else {
            const auto id = g_control.Send("\"cmd\":\"teleport\",\"to\":" + Quote(r.scene) +
                                           ",\"wait\":true,\"timeout\":" + std::to_string(static_cast<int>(r.timeout_s)) + "}");
            if (!id) return fail("no_control", "HOS_CONTROL unset or unreachable: the free-play warp needs Longshot's teleport");
            std::optional<Value> taken;
            while (!taken && std::chrono::steady_clock::now() < until) {
                if (!gate.Step(2, kHoldWait)) return fail("timeout", "the frames did not run in time");
                taken = g_control.Await(*id, 50);
            }
            if (!taken) {
                return ErrorReply(r.id, gate.Frame(), gate.Held(), "timeout",
                                  "Longshot did not take the teleport within " + std::to_string(static_cast<int>(r.timeout_s)) +
                                      " s (it takes it at Link's next update, in play)");
            }
            if (taken->Bool("ok") != true) {
                const std::string code = taken->Text("error").value_or("failed");
                const std::string message = taken->Text("message").value_or("");
                if (code == "not_found") {
                    // Not in Longshot's list (a boss stage): the module's map change event takes any scene it knows.
                    if (const std::string why = ask_map_event(); !why.empty()) {
                        return fail("not_in_event", r.scene + " is not in Longshot's teleport list (" + message +
                                                        ") and the module's map change event cannot take it: " + why);
                    }
                    plan.route = WarpRoute::map_event;
                } else {
                    return fail("failed", TeleportFailure(code, message));
                }
            }
            if (plan.route != WarpRoute::map_event) {
                const TeleportWait wait = ReadTeleport(*taken, r.scene);
                if (wait.failed) return fail(wait.code, wait.why);
                wait_for = wait.wait_for;
            }
        }
        // Frames run (in steps of 10, held between them) until Link is in that scene with no event or load.
        while (std::chrono::steady_clock::now() < until) {
            if (!gate.Step(10, kHoldWait)) return fail("timeout", "the frames did not run in time");
            auto s = LinkStatus();
            if (s && WarpArrived(&*s, wait_for)) {
                return ok("\"arrived\":true,\"map\":" + Quote(wait_for) + ",\"via\":" + Quote(plan.route == WarpRoute::event ? "event" : plan.route == WarpRoute::map_event ? "map_event" : "teleport") +
                          ",\"state\":" + StateObject(false, std::move(s)));
            }
        }
        return ErrorReply(r.id, gate.Frame(), gate.Held(), "timeout",
                          "not in " + wait_for + " after " + std::to_string(static_cast<int>(r.timeout_s)) +
                              " s; held; the state: " + StateObject());
    }
    if (r.cmd == "flag") {
        std::string error;
        const bool number = IsWholeNumber(r.value);
        if (!number && HashValue(r.value)) {
            // An Enum's value by hash: only one flag_read read from this flag in this run (never a raw number).
            if (const std::string why = EnumHashRefusal(g_enum_values, r.name, r.value); !why.empty()) {
                return fail("bad_request", why);
            }
            const auto hash = *HashValue(r.value);
            bool done = false;
            if (!gate.RunHeld([&] { done = CoopTestInput::SetEnumHash(*g_system, r.name, hash); }, kHoldWait)) {
                return fail("timeout", "the game was not held in time");
            }
            if (!done) return fail("failed", "the enum write was not queued (" + r.name + " unknown, or the queue full)");
            return ok("\"name\":" + Quote(r.name) + ",\"value\":" + Quote(HashText(hash)));
        }
        if (!number && r.value.size() > 1 && r.value[0] == '0' && (r.value[1] == 'x' || r.value[1] == 'X')) {
            return fail("bad_request", EnumHashRefusal(g_enum_values, r.name, r.value));
        }
        const auto was = CoopTestInput::FlagInt(*g_system, r.name);
        const std::string line = (number ? "setflag " : "setenum ") + r.name + " " + r.value;
        if (!played(line, error)) return fail("failed", error);
        const auto now = CoopTestInput::FlagInt(*g_system, r.name);
        return ok("\"name\":" + Quote(r.name) + ",\"was\":" + (was ? std::to_string(*was) : "null") +
                  ",\"value\":" + (number ? (now ? std::to_string(*now) : "null") : Quote(r.value)));
    }
    if (r.cmd == "flag_read") {
        // Read only: nothing is written, so it is not gated (it exists only where the bridge does: a play-test run).
        FlagValue v;
        const bool ran = gate.RunHeld([&] {
            auto got = CoopTestInput::ReadFlag(*g_system, r.name);
            v.kind = std::move(got.kind);
            v.number = got.number;
            v.hash = got.hash;
            v.text = std::move(got.text);
            v.live = got.live;
        }, kHoldWait);
        if (!ran) return fail("timeout", "the game was not held in time");
        if (v.kind == "enum" && v.hash) g_enum_values.Remember(r.name, *v.hash);
        return ok(FlagReadMembers(r.name, v));
    }
    if (r.cmd == "text") {
        // Read only: the texts the game set in its text panes since `since` (coop_talk_text.h; the wraps of
        // patches 0041 and 0042 fill them while the bridge is on). No frame is held: the log is the wrap's, under its own lock.
        bool lost = false;
        const auto texts = CoopTalkText::TheLog().Since(r.since, r.layout, &lost);
        return ok(CoopTalkText::Members(CoopHookshot::HasWrap(HOS_COOP_WRAP_PANE_TEXT),
                                        CoopHookshot::HasWrap(HOS_COOP_WRAP_PANE_BODY), CoopTalkText::TheLog().Last(),
                                        lost, texts));
    }
    if (r.cmd == "give") {
        std::string error;
        // Not while a load runs, and not without a fresh Link sample (a load sends no Link update, so the old sample
        // still says `loading` false): one frame is run to take it, as `state` does; a pause or load stays refused.
        const auto status = LinkStatus();
        if (const std::string why = GiveRefusal(status ? &*status : nullptr); !why.empty()) return fail("not_ready", why);
        if (!played("give " + r.item, error)) return fail("failed", error);
        // Written at once inside the held call (CoopTestInput::Give): the reply says what each flag was and is.
        const std::string result = CoopTestInput::LastGive();
        if (result.find("NOT given") != std::string::npos) return fail("failed", result);
        // PARTLY given: the item is in, a later flag (IsGet, a grant) was not written: ok, with `partial` and the text.
        const bool partial = result.find("PARTLY given") != std::string::npos;
        return ok("\"item\":" + Quote(r.item) + ",\"partial\":" + (partial ? "true" : "false") +
                  ",\"result\":" + Quote(result) +
                  ",\"note\":\"written now: the inventory already shows it (no step needed)\"");
    }
    if (r.cmd == "dump") {
        std::string body, error;
        std::size_t count = 0;
        const bool ran = gate.RunHeld([&] {
            const auto flags = CoopTestInput::SharedFlags(*g_system);
            count = flags.size();
            body = "\"flags\":{";
            for (std::size_t i = 0; i < flags.size(); ++i) {
                body += (i ? "," : "") + Quote(flags[i].first) + ":" + std::to_string(flags[i].second);
            }
            body += "}";
        }, kHoldWait);
        if (!ran) return fail("timeout", "the game was not held in time");
        std::error_code ec;
        std::filesystem::create_directories(g_dir / "dumps", ec);
        const auto path = g_dir / "dumps" / (r.tag + ".json");
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "{\"frame\":" << gate.Frame() << ",\"tag\":" << Quote(r.tag) << ",\"state\":" << StateObject(true) << ","
          << body << "}\n";
        if (!f) return fail("failed", "the dump file could not be written");
        LOG_INFO(Core, "coop: playtest: dump {} at frame {}: {} flags", r.tag, gate.Frame(), count);
        return ok("\"path\":" + Quote(path.generic_string()) + ",\"flags\":" + std::to_string(count));
    }
    if (r.cmd == "shot") {
        std::error_code ec;
        std::filesystem::create_directories(g_dir / "shots", ec);
        const std::string name = r.tag.empty() ? "shot-f" + std::to_string(gate.Frame()) : r.tag;
        const auto path = g_dir / "shots" / (name + ".png");
        const auto id = g_control.Send("\"cmd\":\"shot\",\"path\":" + Quote(path.generic_string()) + "}");
        if (!id) return fail("no_control", "HOS_CONTROL unset or unreachable");
        // A held game presents nothing: let one frame at a time run until the shot is taken (at most 5).
        std::uint32_t advanced = 0;
        std::optional<Value> reply = g_control.Await(*id, gate.Holding() ? 300 : 3000);
        while (!reply && gate.Holding() && advanced < 5) {
            if (!gate.Step(1, kHoldWait)) break;
            ++advanced;
            reply = g_control.Await(*id, 500);
        }
        if (!reply || reply->Bool("ok") != true) return fail("failed", "no shot came back");
        return ok("\"path\":" + Quote(path.generic_string()) + ",\"width\":" + Field(&*reply, "width") +
                  ",\"height\":" + Field(&*reply, "height") + ",\"advanced\":" + std::to_string(advanced));
    }
    return fail("bad_request", "unknown cmd");
}

// ---- the socket ----

void Serve(SocketHandle client) {
    std::string buffer;
    bool skipping = false;  // (a line over 64 KB: the rest of it)
    char chunk[8192];
    while (!g_stop.load()) {
        const int w = WaitReadable(client, 200);
        if (w < 0) break;
        if (w == 0) {
            // (a second client meanwhile: one busy reply and closed)
            if (WaitReadable(g_listener, 0) > 0) {
                const SocketHandle other = accept(g_listener, nullptr, nullptr);
                if (other != kNoSocket) {
                    SendAll(other, ErrorReply(Value{}, g_gate->Frame(), g_gate->Held(), "busy",
                                              "another client has this game's bridge") + "\n");
                    CloseSocket(other);
                }
            }
            continue;
        }
        const auto n = recv(client, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(n));
        for (std::size_t nl; (nl = buffer.find('\n')) != std::string::npos;) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (skipping) {
                skipping = false;
                continue;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string error;
            const auto req = ParseRequest(line, error);
            const std::string reply =
                req ? Handle(*req) : ErrorReply(Value{}, g_gate->Frame(), g_gate->Held(), "bad_request", error);
            if (!SendAll(client, reply + "\n")) return;
        }
        if (buffer.size() > kMaxLine) {
            buffer.clear();
            skipping = true;
            if (!SendAll(client, ErrorReply(Value{}, g_gate->Frame(), g_gate->Held(), "bad_request",
                                            "the line is longer than 64 KB") + "\n")) {
                return;
            }
        }
    }
}

void Run() {
    while (!g_stop.load()) {
        if (WaitReadable(g_listener, 200) <= 0) {
            if (const std::string note = g_gate->TakeEndNote(); !note.empty()) {
                LOG_WARNING(Core, "coop: playtest: the hold ended by itself ({}): the game runs again", note);
            }
            continue;
        }
        const SocketHandle client = accept(g_listener, nullptr, nullptr);
        if (client == kNoSocket) continue;
        LOG_INFO(Core, "coop: playtest: a client connected");
        Serve(client);
        CloseSocket(client);
        // The client left (closed or crashed): a held game goes on (review-01111-b: never frozen by a lost tester).
        const bool was = g_gate->Holding();
        g_gate->Resume();
        LOG_INFO(Core, "coop: playtest: the client left{}", was ? ": the game runs again" : "");
    }
}

}  // namespace

void Start(Core::System& system, const std::filesystem::path& game_dir) {
    const std::string port_text = Env("LICHII_COOP_PLAYTEST_PORT");
    const auto port = PortFrom(port_text);
    if (!port) {
        if (!port_text.empty() && port_text != "0") {
            LOG_WARNING(Core, "coop: playtest: off (LICHII_COOP_PLAYTEST_PORT={} is not a port 1024 to 65535)", port_text);
        }
        return;  // off: nothing listens
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_WARNING(Core, "coop: playtest: off (no Winsock)");
        return;
    }
#endif
    const SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        LOG_WARNING(Core, "coop: playtest: off (no socket)");
        return;
    }
#ifdef _WIN32
    BOOL excl = TRUE;  // (no other program may take the same port beside us)
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(*port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only, never another address
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0) {
        LOG_WARNING(Core, "coop: playtest: off (127.0.0.1:{} is taken)", *port);
        CloseSocket(s);
        return;
    }
    g_listener = s;
    g_system = &system;
    g_dir = game_dir / "playtest";
    g_gate = std::make_unique<Gate>(IdleFrom(Env("LICHII_COOP_PLAYTEST_IDLE")));
    // A held game runs no frame and sends no sample: co-op's network thread sends the last one again, so the
    // others keep a still puppet of this player (docs/playtest.md "Frame control").
    g_gate->OnHeld([](bool held) { CoopHook::SetHeld(held); });
    g_enabled.store(true);
    CoopCamera::SetToolWanted(true);  // `move` steers by the camera when solo or before a session too
    CoopWorldSnapshot::TheStore().Enable(&BridgeFrame);  // the object walk fills the `world` list from now on
    g_verbs_runner = EnvIsOn(Env("LICHII_COOP_PLAYTEST_VERBS"));
    g_verbs_cheats = CoopCheats::On();
    LOG_INFO(Core, "coop: playtest: {}", VerbGateLine(g_verbs_runner, g_verbs_cheats));
    g_thread = std::thread(Run);
    LOG_INFO(Core, "coop: playtest: listening on 127.0.0.1:{} (control {}; dumps and shots in {})", *port,
             Env("HOS_CONTROL").empty() ? "off: HOS_CONTROL unset" : "on", g_dir.generic_string());
}

bool PadMove(std::int32_t stick[4]) {
    if (!g_move.On()) return false;
    const auto camera = CoopCamera::Latest();
    return g_move.Apply(camera ? std::optional(camera->clip_from_world) : std::nullopt, stick);
}

bool On() { return g_enabled.load(std::memory_order_acquire); }

std::uint64_t Frame() { return On() ? BridgeFrame() : 0; }

void AtFrame() {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    g_gate->AtFrame();
}

void Stop() {
    if (!g_enabled.exchange(false)) return;
    g_stop.store(true);
    CoopCamera::SetToolWanted(false);
    CoopWorldSnapshot::TheStore().Disable();
    g_gate->Resume();
    if (g_thread.joinable()) g_thread.join();
    CloseSocket(g_listener);
    g_listener = kNoSocket;
}

}  // namespace CoopPlaytest
