// CoopHostMenu (coop_host_menu.h): the "coop host" thread behind the menu's
// Host and Join.
#include "coop_host_menu.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

#include <enet/enet.h>

#include "coop/protocol/join_password.hpp"
#include "coop/protocol/messages.hpp"
#include "host_service.hpp"
#include "join_helper.hpp"
#include "nat_net.hpp"
#include "options.hpp"
#include "relay.hpp"
#include "relay_link.hpp"
#include "session.hpp"

namespace CoopHostMenu {

std::uint8_t HostMaxPlayers(std::uint8_t asked) {
    if (asked == 0) {
        return coop::server::kDefaultMaxPlayers;
    }
    return asked < coop::protocol::kMaxPlayers ? asked : coop::protocol::kMaxPlayers;
}

namespace nat = coop::server::nat;

namespace {

constexpr std::size_t kMaxName = 16;
constexpr std::uint16_t kPortTries = 5;

// A field the caller's struct covers (its size says how much it set).
#define COOP_HAS(o, T, field) ((o)->size >= offsetof(T, field) + sizeof((o)->field))

std::string Text(const char* s) {
    return s == nullptr ? std::string() : std::string(s);
}

std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

int ProblemOf(coop::server::HostProblem p) {
    using coop::server::HostProblem;
    switch (p) {
    case HostProblem::kNoPassword: return HOS_COOP_PROBLEM_NO_PASSWORD;
    case HostProblem::kPortTaken: return HOS_COOP_PROBLEM_PORT_TAKEN;
    case HostProblem::kUpnp: return HOS_COOP_PROBLEM_UPNP_REFUSED;
    case HostProblem::kRelay: return HOS_COOP_PROBLEM_RELAY_FAILED;
    case HostProblem::kNoInvite: return HOS_COOP_PROBLEM_NO_INVITE_CODE;
    case HostProblem::kSymmetric: return HOS_COOP_PROBLEM_SYMMETRIC_NAT;
    case HostProblem::kNone: break;
    }
    return HOS_COOP_PROBLEM_NONE;
}

// Host thread rounds, ms: short, so Leave and pasted codes are quick.
constexpr std::uint32_t kRoundMs = 5;

} // namespace

void CopyText(char* out, std::size_t size, const std::string& text) {
    if (size == 0) {
        return;
    }
    const std::size_t n = text.size() < size - 1 ? text.size() : size - 1;
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
}

Menu::Menu(Hooks hooks) : hooks_(std::move(hooks)) {
    status_.size = sizeof status_;
    status_.state = HOS_COOP_IDLE;
}

Menu::~Menu() {
    Leave();
}

void Menu::Say(const std::string& text) const {
    if (hooks_.log) {
        hooks_.log(text);
    }
}

int Menu::State() const {
    std::lock_guard lock(mutex_);
    return status_.state;
}

void Menu::Status(HosCoopStatus* out) const {
    if (out == nullptr || out->size < sizeof(std::uint32_t)) {
        return;
    }
    HosCoopStatus copy;
    {
        std::lock_guard lock(mutex_);
        copy = status_;
    }
    const std::uint32_t size = out->size < sizeof copy ? out->size : static_cast<std::uint32_t>(sizeof copy);
    std::memcpy(out, &copy, size);
    out->size = size;
}

bool Menu::Busy(const char* what) {
    std::lock_guard lock(mutex_);
    if (status_.state == HOS_COOP_IDLE || status_.state == HOS_COOP_FAILED) {
        return false;
    }
    status_.problem = HOS_COOP_PROBLEM_ALREADY_RUNNING;
    CopyText(status_.message, sizeof status_.message,
             std::string("already ") + (status_.state == HOS_COOP_HOSTING ? "hosting" : "in a session") +
                 ": leave first, then " + what);
    return true;
}

void Menu::Fail(const std::string& why, int problem) {
    Say("coop: host: " + why);
    std::lock_guard lock(mutex_);
    status_.state = HOS_COOP_FAILED;
    status_.problem = problem;
    CopyText(status_.message, sizeof status_.message, why);
}

std::string Menu::NameOr(const char* given) const {
    std::string name = Trim(Text(given));
    if (name.empty() && hooks_.default_name) {
        name = hooks_.default_name();
    }
    return name.empty() ? std::string("Player") : name;
}

std::vector<nat::Addr> Menu::StunServers() const {
    if (hooks_.stun_servers) {
        return *hooks_.stun_servers;
    }
    std::vector<nat::Addr> out;
    for (const std::string& name : nat::default_stun_servers()) {
        if (auto a = nat::resolve(name, 3478)) {
            out.push_back(*a);
        } else {
            Say("coop: host: stun: cannot find " + name + " (no internet, or the name lookup failed)");
        }
    }
    return out;
}

int Menu::HostStart(const HosCoopHostOptions* o) {
    if (o == nullptr || o->size < sizeof(std::uint32_t)) {
        return 0;
    }
    std::lock_guard calls(calls_);
    if (Busy("host")) {
        return 0;
    }
    StopThread();  // a failed start's thread, if any
    HostJob job;
    job.name = NameOr(COOP_HAS(o, HosCoopHostOptions, name) ? o->name : nullptr);
    job.password = Trim(COOP_HAS(o, HosCoopHostOptions, password) ? Text(o->password) : std::string());
    job.port = COOP_HAS(o, HosCoopHostOptions, port) && o->port != 0 ? o->port : coop::protocol::kDefaultPort;
    job.upnp = COOP_HAS(o, HosCoopHostOptions, upnp) && o->upnp != 0;
    job.invite = COOP_HAS(o, HosCoopHostOptions, invite) && o->invite != 0;
    job.pvp = COOP_HAS(o, HosCoopHostOptions, pvp) && o->pvp != 0;
    job.approve_joins = COOP_HAS(o, HosCoopHostOptions, approve_joins) && o->approve_joins != 0;
    job.same_network_open = !(COOP_HAS(o, HosCoopHostOptions, same_network_password) && o->same_network_password != 0);
    job.relay_via = Trim(COOP_HAS(o, HosCoopHostOptions, relay_via) ? Text(o->relay_via) : std::string());
    job.relay_key = COOP_HAS(o, HosCoopHostOptions, relay_key) ? Text(o->relay_key) : std::string();
    job.relay_room = Trim(COOP_HAS(o, HosCoopHostOptions, relay_room) ? Text(o->relay_room) : std::string());
    job.relay_list = Trim(COOP_HAS(o, HosCoopHostOptions, relay_list) ? Text(o->relay_list) : std::string());
    job.max_players = HostMaxPlayers(COOP_HAS(o, HosCoopHostOptions, max_players) ? o->max_players : 0);
    if (COOP_HAS(o, HosCoopHostOptions, pose_upload_kbps)) {
        job.pose_upload_kbps = o->pose_upload_kbps;
    }
    if (job.name.size() > kMaxName) {
        Fail("the name is longer than 16 characters", HOS_COOP_PROBLEM_BAD_NAME);
        return 0;
    }
    if (job.password.empty()) {
        job.password = nat::make_password();  // every session has one; the invite code carries it
    } else if (const char* why = coop::server::password_problem(job.password)) {
        Fail(std::string("the join password is ") + why, HOS_COOP_PROBLEM_BAD_PASSWORD);
        return 0;
    } else if (job.password.find(' ') != std::string::npos) {
        // The invite code cannot hold one (nat::make_invite), as Host.bat says.
        Fail("the join password has a space; use one without", HOS_COOP_PROBLEM_BAD_PASSWORD);
        return 0;
    }
    {
        std::lock_guard lock(mutex_);
        status_ = HosCoopStatus{};
        status_.size = sizeof status_;
        status_.state = HOS_COOP_STARTING;
        CopyText(status_.password, sizeof status_.password, job.password);
    }
    stop_ = false;
    thread_ = std::thread([this, job = std::move(job)]() mutable { RunHost(std::move(job)); });
    return 1;
}

void Menu::RunHost(HostJob job) {
    try {
        if (enet_initialize() != 0) {
            Fail("the network did not start (ENet)", HOS_COOP_PROBLEM_NETWORK);
            return;
        }
        struct EnetLife {
            ~EnetLife() { enet_deinitialize(); }
        } enet_life;
        coop::server::HostServiceConfig config;
        config.port = job.port;
        config.port_tries = kPortTries;
        config.session.join_password = job.password;
        config.session.pvp = job.pvp;
        config.session.approve_joins = job.approve_joins;
        config.session.same_network_open = job.same_network_open;
        config.session.max_players = job.max_players;
        config.session.pose_upload_bytes_per_second = job.pose_upload_kbps * 125u;
        config.max_connections = static_cast<std::size_t>(job.max_players) + 4;
        config.upnp = job.upnp;
        config.invite = job.invite;
        config.remember_friends = true;   // a friend who rejoins needs no new answer code
        if (job.invite) {
            config.stun_servers = StunServers();
        }
        config.stun_allow_private = hooks_.allow_private;
        config.relay_via = job.relay_via;
        config.relay_key = job.relay_key;
        config.relay_room = job.relay_via.empty() ? std::string() : job.relay_room;
        config.relay_list = config.relay_room.empty() ? std::string() : job.relay_list;   // (the public list: opt-in)
        config.counts = false;  // the game's log need not have them every 5 s
        config.log_invite = false;  // it holds the join password; the menu shows it
        config.log = [this](const std::string& text) { Say("coop: host: " + text); };
        coop::server::HostService host(config);
        std::string why;
        if (!host.start(why)) {
            Fail(why, host.port_failed() ? HOS_COOP_PROBLEM_PORT_TAKEN : HOS_COOP_PROBLEM_NO_PASSWORD);
            return;
        }
        const std::uint16_t port = host.port();
        {
            std::lock_guard lock(mutex_);
            status_.state = HOS_COOP_HOSTING;
            status_.port = port;
        }
        bool connected = false;
        if (hooks_.connect) {
            connected = hooks_.connect(Target{"127.0.0.1", port, job.name, job.password});
            if (!connected) {
                std::lock_guard lock(mutex_);
                status_.problem = HOS_COOP_PROBLEM_CLIENT_FAILED;
                CopyText(status_.message, sizeof status_.message,
                         "the server runs, but this game's co-op did not start (see the log)");
            }
        }
        while (!stop_) {
            host.set_ngplus(hooks_.ngplus_level ? hooks_.ngplus_level() : 0);   // (Navi) New Game+ (Hooks)
            host.service(kRoundMs);
            std::deque<std::string> pasted;
            std::optional<std::string> new_password;
            {
                std::lock_guard lock(mutex_);
                pasted.swap(answers_);
                new_password.swap(new_password_);
            }
            if (new_password) {   // (the host's new join password: new joins need it, players in stay in)
                std::string why;
                const bool ok = host.set_join_password(*new_password, why);
                std::lock_guard lock(mutex_);
                if (ok) {
                    job.password = *new_password;
                    CopyText(status_.password, sizeof status_.password, *new_password);
                    status_.problem = HOS_COOP_PROBLEM_NONE;
                    CopyText(status_.message, sizeof status_.message,
                             "new join password is set");   // (one line of the list)
                } else {
                    status_.problem = HOS_COOP_PROBLEM_BAD_PASSWORD;
                    CopyText(status_.message, sizeof status_.message, "join password not changed: " + why);
                }
            }
            for (const std::string& code : pasted) {
                const nat::CodeError e = host.add_answer(code);
                std::lock_guard lock(mutex_);
                status_.problem = e == nat::CodeError::kOk ? HOS_COOP_PROBLEM_NONE : HOS_COOP_PROBLEM_BAD_CODE;
                CopyText(status_.message, sizeof status_.message,
                         e == nat::CodeError::kOk ? std::string("answer code taken: connecting to your friend")
                                                  : "answer code not used: " + nat::explain(e));
            }
            const coop::server::HostStatus s = host.status();
            std::lock_guard lock(mutex_);
            status_.players = static_cast<std::uint16_t>(s.players);
            status_.stream_kbps = s.stream_kbps;
            status_.invite_done = s.invite_done ? 1 : 0;
            status_.upnp_open = s.upnp_open ? 1 : 0;
            CopyText(status_.invite, sizeof status_.invite, s.invite);
            CopyText(status_.relay_code, sizeof status_.relay_code, s.relay_code);
            CopyText(status_.relay_room, sizeof status_.relay_room, s.relay_room);
            if (!s.message.empty() && status_.message[0] == '\0') {
                CopyText(status_.message, sizeof status_.message, s.message);
                status_.problem = ProblemOf(s.problem);
            }
        }
        // Our own game says goodbye first, while the server still listens.
        if (connected && hooks_.disconnect) {
            hooks_.disconnect();
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (std::chrono::steady_clock::now() < until) {
                host.service(kRoundMs);
            }
        }
        host.stop();
    } catch (const std::exception& e) {
        Fail(std::string("the co-op host stopped: ") + e.what(), HOS_COOP_PROBLEM_INTERNAL);
    }
}

int Menu::Join(const HosCoopJoinOptions* o) {
    if (o == nullptr || o->size < sizeof(std::uint32_t)) {
        return 0;
    }
    std::lock_guard calls(calls_);
    if (Busy("join")) {
        return 0;
    }
    StopThread();
    const std::string name = NameOr(COOP_HAS(o, HosCoopJoinOptions, name) ? o->name : nullptr);
    const std::string target = Trim(COOP_HAS(o, HosCoopJoinOptions, target) ? Text(o->target) : std::string());
    std::string password = Trim(COOP_HAS(o, HosCoopJoinOptions, password) ? Text(o->password) : std::string());
    if (name.size() > kMaxName) {
        Fail("the name is longer than 16 characters", HOS_COOP_PROBLEM_BAD_NAME);
        return 0;
    }
    if (target.empty()) {
        Fail("paste the invite code the host sent you, or type the host's address", HOS_COOP_PROBLEM_NO_TARGET);
        return 0;
    }
    {
        std::lock_guard lock(mutex_);
        status_ = HosCoopStatus{};
        status_.size = sizeof status_;
        status_.state = HOS_COOP_STARTING;
    }
    const nat::InviteRead invite = nat::read_invite(target);
    // Tests host on 127.0.0.1, which an invite code never holds otherwise.
    const bool loopback_ok = hooks_.allow_private && invite.error == nat::CodeError::kNotPublic;
    if (invite.error == nat::CodeError::kOk || loopback_ok) {
        JoinJob job{name, invite.invite.password, invite.invite.host};
        stop_ = false;
        thread_ = std::thread([this, job = std::move(job)]() mutable { RunJoin(std::move(job)); });
        return 1;
    }
    if (invite.error != nat::CodeError::kNotACode) {
        Fail("invite code not used: " + nat::explain(invite.error), HOS_COOP_PROBLEM_BAD_CODE);
        return 0;
    }
    // Through a relay (issue 261): "<join code or room>@relay[:port]". The relay lets this PC's address in
    // (Join.bat's relay-join.ps1 does the same), then the game connects to the relay with the host's password.
    if (auto relay_join = coop::server::relay::parse_relay_join(target)) {
        if (password.empty()) {
            Fail("type the join password the host sent you (every session has one)", HOS_COOP_PROBLEM_NO_PASSWORD);
            return 0;
        }
        if (const char* why = coop::server::password_problem(password)) {
            Fail(std::string("the join password is ") + why, HOS_COOP_PROBLEM_BAD_PASSWORD);
            return 0;
        }
        RelayJoinJob job{name, password, std::move(relay_join->code), std::move(relay_join->room),
                         std::move(relay_join->relay_host), relay_join->relay_port};
        stop_ = false;
        thread_ = std::thread([this, job = std::move(job)]() mutable { RunRelayJoin(std::move(job)); });
        return 1;
    }
    // A plain address: this game connects there itself.
    // The name lookup needs the sockets started (WSAStartup on Windows).
    std::optional<nat::Addr> where;
    if (enet_initialize() == 0) {
        where = nat::resolve(target, coop::protocol::kDefaultPort);
        enet_deinitialize();
    }
    if (!where) {
        Fail("'" + target + "' is neither an invite code nor an address this PC can find", HOS_COOP_PROBLEM_NO_TARGET);
        return 0;
    }
    // A game on this network with no password typed: the same-network key (docs/protocol.md "Same network"), which
    // its host takes from a private address when it lets the same network in without the password.
    if (password.empty() && coop::server::is_same_network(where->ip)) {
        password = std::string(coop::protocol::kSameNetworkPassword);
    }
    if (password.empty()) {
        Fail("type the join password the host sent you (every session has one)", HOS_COOP_PROBLEM_NO_PASSWORD);
        return 0;
    }
    if (const char* why = coop::server::password_problem(password)) {
        Fail(std::string("the join password is ") + why, HOS_COOP_PROBLEM_BAD_PASSWORD);
        return 0;
    }
    const std::string host = coop::server::show_ipv4(where->ip);
    if (!hooks_.connect || !hooks_.connect(Target{host, where->port, name, password})) {
        Fail("this game's co-op did not start (see the log)", HOS_COOP_PROBLEM_CLIENT_FAILED);
        return 0;
    }
    direct_connected_ = true;
    std::lock_guard lock(mutex_);
    status_.state = HOS_COOP_CONNECTED;
    status_.port = where->port;
    return 1;
}

void Menu::RunJoin(JoinJob job) {
    try {
        if (enet_initialize() != 0) {
            Fail("the network did not start (ENet)", HOS_COOP_PROBLEM_NETWORK);
            return;
        }
        struct EnetLife {
            ~EnetLife() { enet_deinitialize(); }
        } enet_life;
        nat::JoinConfig config;
        config.host = job.host;
        config.password = job.password;
        config.stun_servers = StunServers();
        config.game_port = hooks_.helper_port;
        config.direct_ms = hooks_.direct_ms;
        if (hooks_.client_id) {
            if (const std::string id = hooks_.client_id(); !id.empty()) {
                const RejoinId r = RejoinIds(id, job.password, job.host);
                config.nonce = r.nonce;
                config.public_port = r.public_port;
            }
        }
        // The answer code goes to the menu (status.answer), not the log.
        config.log = [this](const std::string& text) {
            if (text.rfind("answer code: ", 0) != 0) {
                Say("coop: join: " + text);
            }
        };
        nat::JoinHelper helper(config);
        std::string why;
        if (!helper.start(why)) {
            // Join.bat's helper window may have the port.
            Fail(why + " (is a Join.bat helper window open? close it)", HOS_COOP_PROBLEM_HELPER_PORT_TAKEN);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            status_.state = HOS_COOP_JOINING;
        }
        bool connected = false;
        while (!stop_) {
            helper.service(kRoundMs);
            const nat::JoinState state = helper.state();
            if (state == nat::JoinState::kOpen && !connected) {
                connected = true;
                const bool ok = hooks_.connect &&
                                hooks_.connect(Target{"127.0.0.1", helper.game_port(), job.name, job.password});
                std::lock_guard lock(mutex_);
                status_.state = ok ? HOS_COOP_CONNECTED : HOS_COOP_FAILED;
                status_.problem = ok ? HOS_COOP_PROBLEM_NONE : HOS_COOP_PROBLEM_CLIENT_FAILED;
                status_.port = job.host.port;
                CopyText(status_.answer, sizeof status_.answer, "");
                CopyText(status_.message, sizeof status_.message,
                         ok ? std::string(helper.direct() ? "the host answered: connecting"
                                                          : "the host's punches came through: connecting")
                            : std::string("this game's co-op did not start (see the log)"));
                if (!ok) {
                    connected = false;
                    break;
                }
            } else if (state == nat::JoinState::kWaitingForHost) {
                std::lock_guard lock(mutex_);
                if (status_.state != HOS_COOP_WAIT_HOST) {
                    status_.state = HOS_COOP_WAIT_HOST;
                    CopyText(status_.answer, sizeof status_.answer, helper.answer_code());
                    CopyText(status_.message, sizeof status_.message,
                             "the host's PC did not answer directly: send the host this ANSWER CODE; they paste "
                             "it into their game");
                }
            }
        }
        if (connected && hooks_.disconnect) {
            hooks_.disconnect();
            // The goodbye goes through the helper: a few more rounds.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (std::chrono::steady_clock::now() < until) {
                helper.service(kRoundMs);
            }
        }
        helper.stop();
    } catch (const std::exception& e) {
        Fail(std::string("the co-op join helper stopped: ") + e.what(), HOS_COOP_PROBLEM_INTERNAL);
    }
}

void Menu::RunRelayJoin(RelayJoinJob job_in) {
    namespace relay = coop::server::relay;
    struct {
        std::string name, password;
        relay::RelayJoin join;
    } job{std::move(job_in.name), std::move(job_in.password),
          relay::RelayJoin{std::move(job_in.code), std::move(job_in.room), std::move(job_in.relay_host), job_in.relay_port}};
    try {
        if (enet_initialize() != 0) {
            Fail("the network did not start (ENet)", HOS_COOP_PROBLEM_NETWORK);
            return;
        }
        struct EnetLife {
            ~EnetLife() { enet_deinitialize(); }
        } enet_life;
        {
            std::lock_guard lock(mutex_);
            status_.state = HOS_COOP_JOINING;
            CopyText(status_.message, sizeof status_.message, "asking the relay " + job.join.relay_host + " to let this game in");
        }
        ENetAddress at{};
        if (enet_address_set_host(&at, job.join.relay_host.c_str()) != 0) {
            Fail("cannot find the relay " + job.join.relay_host + " (is the name right, and is this PC online?)",
                 HOS_COOP_PROBLEM_NO_TARGET);
            return;
        }
        at.port = job.join.relay_port;
        const auto result = relay::lookup_on_relay(at, job.join, [this] { return stop_.load(); });
        if (stop_) {
            return;
        }
        Say("coop: join: " + relay::lookup_text(result, job.join));
        if (result != relay::LookupResult::kOk) {
            Fail(relay::lookup_text(result, job.join), HOS_COOP_PROBLEM_RELAY_JOIN);
            return;
        }
        const bool ok = hooks_.connect &&
                        hooks_.connect(Target{relay::address_text(at.host), at.port, job.name, job.password});
        {
            std::lock_guard lock(mutex_);
            status_.state = ok ? HOS_COOP_CONNECTED : HOS_COOP_FAILED;
            status_.problem = ok ? HOS_COOP_PROBLEM_NONE : HOS_COOP_PROBLEM_CLIENT_FAILED;
            status_.port = at.port;
            CopyText(status_.message, sizeof status_.message,
                     ok ? relay::lookup_text(result, job.join) : std::string("this game's co-op did not start (see the log)"));
        }
        if (!ok) {
            return;
        }
        // Joined: this thread keeps the session's sockets started until Leave.
        while (!stop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRoundMs));
        }
        if (hooks_.disconnect) {
            hooks_.disconnect();
        }
    } catch (const std::exception& e) {
        Fail(std::string("the relay join stopped: ") + e.what(), HOS_COOP_PROBLEM_INTERNAL);
    }
}

int Menu::Answer(const char* code) {
    const std::string text = Trim(Text(code));
    std::lock_guard lock(mutex_);
    if (status_.state != HOS_COOP_HOSTING || text.empty()) {
        CopyText(status_.message, sizeof status_.message,
                 status_.state != HOS_COOP_HOSTING ? "an answer code is for the host's game"
                                                   : "paste the friend's ANSWER CODE");
        return 0;
    }
    if (status_.invite[0] == '\0') {
        CopyText(status_.message, sizeof status_.message, "there is no invite code this session, so no answer code");
        return 0;
    }
    status_.message[0] = '\0';
    answers_.push_back(text);
    return 1;
}

int Menu::SetPassword(const char* password) {
    const std::string text = Trim(Text(password));
    std::lock_guard lock(mutex_);
    const char* why = status_.state != HOS_COOP_HOSTING ? "the join password changes only while hosting"
                      : text.empty()                    ? "type the new join password first"
                      : text.find(' ') != std::string::npos ? "the join password has a space; use one without"
                                                            : coop::server::password_problem(text);
    if (why != nullptr) {
        status_.problem = HOS_COOP_PROBLEM_BAD_PASSWORD;
        CopyText(status_.message, sizeof status_.message,
                 status_.state != HOS_COOP_HOSTING || text.empty() || text.find(' ') != std::string::npos
                     ? std::string(why)
                     : std::string("the join password is ") + why);
        return 0;
    }
    new_password_ = text;
    return 1;
}

void Menu::Leave() {
    std::lock_guard calls(calls_);
    StopThread();
}

void Menu::StopThread() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (direct_connected_) {
        direct_connected_ = false;
        if (hooks_.disconnect) {
            hooks_.disconnect();
        }
    }
    std::lock_guard lock(mutex_);
    answers_.clear();
    // A failure is kept until the next start, so the menu can show why.
    if (status_.state != HOS_COOP_FAILED) {
        status_ = HosCoopStatus{};
        status_.size = sizeof status_;
        status_.state = HOS_COOP_IDLE;
    }
}

RejoinId RejoinIds(const std::string& client_id, const std::string& password, coop::server::nat::Addr host) {
    const std::string s = "oot-coop rejoin|" + client_id + "|" + password + "|" + coop::server::nat::show(host);
    const auto h = coop::protocol::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    RejoinId r;
    std::copy_n(h.begin(), r.nonce.size(), r.nonce.begin());
    r.public_port = static_cast<std::uint16_t>(40000 + ((h[8] << 8) | h[9]) % 20000);
    return r;
}

} // namespace CoopHostMenu

namespace CoopHostMenu {

namespace {
// Only spaces, line breaks, and the invisible or no-break spaces a chat copy can hold (UTF-8): U+00A0, U+200B to
// U+200D, U+2060, U+FEFF. Any other character (CJK, an emoji) is something copied, not an empty clipboard.
bool PastedNothing(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            ++i;
        } else if (s.substr(i, 2) == "\xC2\xA0") {
            i += 2;
        } else if (s.substr(i, 3) == "\xE2\x80\x8B" || s.substr(i, 3) == "\xE2\x80\x8C" ||
                   s.substr(i, 3) == "\xE2\x80\x8D" || s.substr(i, 3) == "\xE2\x81\xA0" ||
                   s.substr(i, 3) == "\xEF\xBB\xBF") {
            i += 3;
        } else {
            return false;
        }
    }
    return true;
}
}  // namespace

PastedAnswer ReadPastedAnswer(const std::optional<std::string>& clipboard, std::string_view own_invite,
                              std::string_view password, std::uint64_t unix_now) {
    PastedAnswer out;
    if (!clipboard) {
#ifdef _WIN32
        out.line = "The clipboard could not be read (another program had it open): press this again.";
#else
        out.line = "The clipboard could not be read: no clipboard tool. Press Ctrl+V here, or install wl-clipboard "
                   "(Wayland) or xclip (X11), then press this again.";
#endif
        return out;
    }
    out.text = *clipboard;
    if (PastedNothing(out.text)) {
        out.line = "Your clipboard is empty: copy your friend's answer code first, then press this again.";
        return out;
    }
    const nat::AnswerRead read = nat::read_answer(out.text, password, unix_now);
    if (read.error == nat::CodeError::kOk) {
        out.ok = true;
        out.line = nat::explain_answer(read.error);
        return out;
    }
    if (read.error == nat::CodeError::kWrongKind && !own_invite.empty()) {
        const auto own = nat::clean_letters(own_invite);
        for (const std::string& code : nat::find_codes(out.text)) {
            if (own && code == *own) {
                out.line = "That is your own invite code: copy the answer code your friend sends back to you.";
                return out;
            }
        }
    }
    out.line = nat::explain_answer(read.error);
    return out;
}

} // namespace CoopHostMenu
