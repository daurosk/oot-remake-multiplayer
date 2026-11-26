// Hosting and joining from the game's menu (hos_coop_host.h): the work behind
// the module's hos_coop_host_start / join / host_answer / leave / status
// exports. One "coop host" thread runs the co-op server (HostService, the
// same object coop_server loops around) or the invite code's join helper;
// the menu's calls only start, stop and read it, from any thread. No game code here: the
// module hands in how to connect its own co-op client, so the tests drive
// this with a real server and a fake game.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <thread>
#include <vector>

#include "hos_coop_host.h"
#include "nat_codes.hpp"

namespace CoopHostMenu {

// Where this game's own co-op client connects.
struct Target {
    std::string server;   // "127.0.0.1" for our own server or the join helper
    std::uint16_t port = 0;
    std::string name;
    std::string password;
};

struct Hooks {
    // Connects this game's co-op client; false when it could not start.
    std::function<bool(const Target&)> connect;
    // Says goodbye and stops the client (before our server goes away).
    std::function<void()> disconnect;
    // One line into the game's log ("coop: host: ..." lines).
    std::function<void(const std::string&)> log;
    // The nickname when the menu gives none (coop.ini's).
    std::function<std::string()> default_name;
    // Tests: STUN servers instead of the public ones, 127.0.0.1 allowed as an
    // internet address, the join helper's local port.
    std::optional<std::vector<coop::server::nat::Addr>> stun_servers;
    bool allow_private = false;
    std::uint16_t helper_port = 27512;
    // This PC's own id (coop.ini client_id): with it a join by invite code uses
    // the same helper nonce and port each time (RejoinIds). Empty: random.
    std::function<std::string()> client_id;
    std::uint64_t direct_ms = 3000;
    // (Navi) This game's New Game+ level (coop_ngplus.h), told to our server each round for everyone. Empty: 0.
    std::function<std::uint8_t()> ngplus_level;
    // The host changed the join password while hosting: this game's own client takes it for its reconnects.
    std::function<void(const std::string&)> password_changed;
};

// Most players at once for a server the title menu's Host starts: the options' number (1 to 16, more taken as 16),
// or with 0 the server's own default (coop::server::kDefaultMaxPlayers, as coop_server without --max-players).
std::uint8_t HostMaxPlayers(std::uint8_t asked);

class Menu {
public:
    explicit Menu(Hooks hooks);
    ~Menu();  // Leave()
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    int HostStart(const HosCoopHostOptions* options);
    // The next HostStart only (local co-op's game 1, CoopPause's LocalHostStep): its listener on 127.0.0.1 alone, the
    // session of this PC's games. A later Host from the menu in the same run is a usual host again.
    void LoopbackNextHost() { loopback_next_ = true; }
    int Join(const HosCoopJoinOptions* options);
    int Answer(const char* code);
    // The host's new join password while hosting (the pause menu's Session view): players who join from now on
    // need it, players already in stay in; the invite code is made again. 1: taken (the host thread sets it at its
    // next round; the status' password and message say when); 0: not hosting or not a password (message says why).
    int SetPassword(const char* password);
    void Leave();
    void Status(HosCoopStatus* out) const;

    // For tests and the log: the state as HOS_COOP_*.
    int State() const;

private:
    struct HostJob {
        std::string name, password, relay_via, relay_key, relay_room, relay_list;
        std::uint16_t port = 0;
        bool upnp = false, invite = false, pvp = false, approve_joins = false;
        bool same_network_open = false;   // (players from a private address join without the password: the options' default)
        std::uint8_t max_players = 0;  // HostMaxPlayers'
        std::uint32_t pose_upload_kbps = 0;
        bool public_game = false;   // (HosCoopHostOptions::public_game)
        std::string bans_file;      // (HosCoopHostOptions::bans_file)
        bool loopback = false;      // (LoopbackNextHost: listen on 127.0.0.1 only)
    };
    struct JoinJob {
        std::string name, password;
        coop::server::nat::Addr host;
    };
    // "<join code or room>@relay": the relay lets this PC's address in, then the game connects to the relay.
    // The fields of relay_link.hpp's RelayJoin, which this header does not include: it brings ENet, and with it
    // windows.h's min and max, into every file that includes this one (coop_hookshot.cpp).
    struct RelayJoinJob {
        std::string name, password;
        std::string code, room, relay_host;
        std::uint16_t relay_port = 0;
    };
    bool Busy(const char* what);
    void StopThread();  // with calls_ held
    void Fail(const std::string& why, int problem);
    void Say(const std::string& text) const;
    void RunHost(HostJob job);
    void RunJoin(JoinJob job);
    void RunRelayJoin(RelayJoinJob job);
    std::vector<coop::server::nat::Addr> StunServers() const;
    std::string NameOr(const char* given) const;

    Hooks hooks_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> loopback_next_{false};   // (LoopbackNextHost)
    bool direct_connected_ = false;  // a plain address: the client only, no thread
    std::mutex calls_;  // one start, join or leave at a time (the menu and the game's exit)
    mutable std::mutex mutex_;
    std::deque<std::string> answers_;  // pasted, for the host thread
    std::optional<std::string> new_password_;  // the host's new join password, for the host thread
    HosCoopStatus status_{};
};

// The helper nonce and public port for joining `host` with `password` from the PC
// `client_id`: the same each time (a rejoin), different for another PC or invite.
struct RejoinId {
    coop::server::nat::Nonce nonce{};
    std::uint16_t public_port = 0;   // 40000 to 59999
};
RejoinId RejoinIds(const std::string& client_id, const std::string& password, coop::server::nat::Addr host);

// Copies `text` into a fixed C string, cut to fit, always ended.
void CopyText(char* out, std::size_t size, const std::string& text);
// Row 260: this process's memory as one log line: private (commit) bytes, the working set, the process heap's
// allocated and committed bytes, the handle count (Windows); resident and virtual bytes (Linux). Read-only, cheap
// (no heap walk). The host's game logs it every kMemoryLineMs with the server's sizes (HostService::memory_line).
// Row 271: on Windows also every heap of the process summed (HeapSummary each: the driver's and other DLLs' heaps
// too) and the private bytes outside them (taken straight from Windows: Longshot's and the driver's own blocks).
std::string ProcessMemoryText();
// Row 271: every game (solo, guest, host) logs `coop: memory: <ProcessMemoryText>` once per kGameMemoryLineMs from its
// frame hook, so a player's log shows whether private grows in the heaps (allocations, ours among them) or outside.
// True when the line is due at now_ms (then next_ms moves on by kGameMemoryLineMs; the first call is due at once).
bool MemoryLineDue(std::uint64_t now_ms, std::uint64_t& next_ms);
// The autotest's in-game host (tools/autotest hookshot.py --host-in-game, scenario step `menu host`): the game hosts
// through its own Multiplayer > Host (CoopPause::TestHost), on the port and with the password the harness gives in
// LICHII_COOP_TEST_HOST=PORT:PASSWORD (a password of 1 to 64 printable letters). Nothing for any other text. Test
// only: such a host listens on 127.0.0.1 alone (Menu::RunHost) and opens no router port or invite.
inline constexpr const char* kTestHostEnv = "LICHII_COOP_TEST_HOST";
std::optional<std::pair<std::uint16_t, std::string>> ParseTestHost(std::string_view text);
// The autotest's relay room host (private-lobby-fix): with LICHII_COOP_TEST_RELAY=<host:port> (CoopTitleMenu::RelayOk)
// and LICHII_COOP_TEST_RELAY_ROOM=<room> (RoomNameOk) also set, `menu host` holds that private room on that relay
// (a loopback relay in every run), so a guest's `menu paste` joins it as a player's paste does. Test only.
inline constexpr const char* kTestRelayEnv = "LICHII_COOP_TEST_RELAY";
inline constexpr const char* kTestRelayRoomEnv = "LICHII_COOP_TEST_RELAY_ROOM";
// Local co-op's host (the kit's scripts\local-coop.ps1, CoopPause's LocalHostStep): game 1 of 2 to 4 games on one PC
// hosts at its start, OOT_COOP_LOCAL_HOST=PORT:PASSWORD (as ParseTestHost reads it). That host alone listens on
// 127.0.0.1 only (Menu::LoopbackNextHost): the session is this PC's.
inline constexpr const char* kLocalHostEnv = "OOT_COOP_LOCAL_HOST";
inline constexpr std::uint64_t kMemoryLineMs = 30'000;
inline constexpr std::uint64_t kGameMemoryLineMs = 60'000;

} // namespace CoopHostMenu

namespace CoopHostMenu {

// The host's "Paste answer code" (the pause menu's Host page), checked at once so the player is told plainly what
// was wrong: nothing when the clipboard could not be read. The answer code may sit anywhere in what was copied (a
// whole chat message: name, time, "code:", quotes, line breaks; nat::find_codes).
struct PastedAnswer {
    bool ok = false;      // an answer code for this session: hand `text` to Menu::Answer
    std::string text;     // what was pasted
    std::string line;     // one plain line for the menu: why not, or that it was taken
};
PastedAnswer ReadPastedAnswer(const std::optional<std::string>& clipboard, std::string_view own_invite,
                              std::string_view password, std::uint64_t unix_now);

}  // namespace CoopHostMenu
