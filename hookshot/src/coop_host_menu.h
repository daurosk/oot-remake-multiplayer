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
    int Join(const HosCoopJoinOptions* options);
    int Answer(const char* code);
    void Leave();
    void Status(HosCoopStatus* out) const;

    // For tests and the log: the state as HOS_COOP_*.
    int State() const;

private:
    struct HostJob {
        std::string name, password, relay_via, relay_key;
        std::uint16_t port = 0;
        bool upnp = false, invite = false, pvp = false;
        std::uint8_t max_players = 0;  // HostMaxPlayers'
        std::uint32_t pose_upload_kbps = 0;
    };
    struct JoinJob {
        std::string name, password;
        coop::server::nat::Addr host;
    };
    bool Busy(const char* what);
    void StopThread();  // with calls_ held
    void Fail(const std::string& why, int problem);
    void Say(const std::string& text) const;
    void RunHost(HostJob job);
    void RunJoin(JoinJob job);
    std::vector<coop::server::nat::Addr> StunServers() const;
    std::string NameOr(const char* given) const;

    Hooks hooks_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool direct_connected_ = false;  // a plain address: the client only, no thread
    std::mutex calls_;  // one start, join or leave at a time (the menu and the game's exit)
    mutable std::mutex mutex_;
    std::deque<std::string> answers_;  // pasted, for the host thread
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

} // namespace CoopHostMenu

namespace CoopHostMenu {

// The host's "Paste friend's answer" (the pause menu's Host page), checked at once so the player is told plainly what
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
