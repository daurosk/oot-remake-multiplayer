// CoopHostMenu (coop_host_menu.h): the "coop host" thread behind the menu's
// Host and Join.
#include "coop_host_menu.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <utility>

#include <enet/enet.h>

#include "coop/protocol/messages.hpp"
#include "host_service.hpp"
#include "join_helper.hpp"
#include "nat_net.hpp"
#include "options.hpp"

namespace CoopHostMenu {

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
    CopyText(status_.message, sizeof status_.message,
             std::string("already ") + (status_.state == HOS_COOP_HOSTING ? "hosting" : "in a session") +
                 ": leave first, then " + what);
    return true;
}

void Menu::Fail(const std::string& why) {
    Say("coop: host: " + why);
    std::lock_guard lock(mutex_);
    status_.state = HOS_COOP_FAILED;
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
    job.relay_via = Trim(COOP_HAS(o, HosCoopHostOptions, relay_via) ? Text(o->relay_via) : std::string());
    job.relay_key = COOP_HAS(o, HosCoopHostOptions, relay_key) ? Text(o->relay_key) : std::string();
    if (job.name.size() > kMaxName) {
        Fail("the name is longer than 16 characters");
        return 0;
    }
    if (job.password.empty()) {
        job.password = nat::make_password();  // every session has one; the invite code carries it
    } else if (const char* why = coop::server::password_problem(job.password)) {
        Fail(std::string("the join password is ") + why);
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
            Fail("the network did not start (ENet)");
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
        config.upnp = job.upnp;
        config.invite = job.invite;
        if (job.invite) {
            config.stun_servers = StunServers();
        }
        config.stun_allow_private = hooks_.allow_private;
        config.relay_via = job.relay_via;
        config.relay_key = job.relay_key;
        config.counts = false;  // the game's log need not have them every 5 s
        config.log = [this](const std::string& text) { Say("coop: host: " + text); };
        coop::server::HostService host(config);
        std::string why;
        if (!host.start(why)) {
            Fail(why);
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
                CopyText(status_.message, sizeof status_.message,
                         "the server runs, but this game's co-op did not start (see the log)");
            }
        }
        while (!stop_) {
            host.service(kRoundMs);
            std::deque<std::string> pasted;
            {
                std::lock_guard lock(mutex_);
                pasted.swap(answers_);
            }
            for (const std::string& code : pasted) {
                const nat::CodeError e = host.add_answer(code);
                std::lock_guard lock(mutex_);
                CopyText(status_.message, sizeof status_.message,
                         e == nat::CodeError::kOk ? std::string("answer code taken: connecting to your friend")
                                                  : "answer code not used: " + nat::explain(e));
            }
            const coop::server::HostStatus s = host.status();
            std::lock_guard lock(mutex_);
            status_.players = static_cast<std::uint16_t>(s.players);
            status_.invite_done = s.invite_done ? 1 : 0;
            status_.upnp_open = s.upnp_open ? 1 : 0;
            CopyText(status_.invite, sizeof status_.invite, s.invite);
            CopyText(status_.relay_code, sizeof status_.relay_code, s.relay_code);
            if (!s.message.empty() && status_.message[0] == '\0') {
                CopyText(status_.message, sizeof status_.message, s.message);
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
        Fail(std::string("the co-op host stopped: ") + e.what());
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
        Fail("the name is longer than 16 characters");
        return 0;
    }
    if (target.empty()) {
        Fail("paste the invite code the host sent you, or type the host's address");
        return 0;
    }
    {
        std::lock_guard lock(mutex_);
        status_ = HosCoopStatus{};
        status_.size = sizeof status_;
        status_.state = HOS_COOP_STARTING;
    }
    const nat::InviteRead invite = nat::read_invite(target);
    if (invite.error == nat::CodeError::kOk) {
        JoinJob job{name, invite.invite.password, invite.invite.host};
        stop_ = false;
        thread_ = std::thread([this, job = std::move(job)]() mutable { RunJoin(std::move(job)); });
        return 1;
    }
    if (invite.error != nat::CodeError::kNotACode) {
        Fail("invite code not used: " + nat::explain(invite.error));
        return 0;
    }
    // A plain address: this game connects there itself.
    // The name lookup needs the sockets started (WSAStartup on Windows).
    std::optional<nat::Addr> where;
    if (enet_initialize() == 0) {
        where = nat::resolve(target, coop::protocol::kDefaultPort);
        enet_deinitialize();
    }
    if (!where) {
        Fail("'" + target + "' is neither an invite code nor an address this PC can find");
        return 0;
    }
    if (password.empty()) {
        Fail("type the join password the host sent you (every session has one)");
        return 0;
    }
    if (const char* why = coop::server::password_problem(password)) {
        Fail(std::string("the join password is ") + why);
        return 0;
    }
    const std::string host = coop::server::show_ipv4(where->ip);
    if (!hooks_.connect || !hooks_.connect(Target{host, where->port, name, password})) {
        Fail("this game's co-op did not start (see the log)");
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
            Fail("the network did not start (ENet)");
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
        config.log = [this](const std::string& text) { Say("coop: join: " + text); };
        nat::JoinHelper helper(config);
        std::string why;
        if (!helper.start(why)) {
            // Join.bat's helper window may have the port.
            Fail(why + " (is a Join.bat helper window open? close it)");
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
        Fail(std::string("the co-op join helper stopped: ") + e.what());
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

} // namespace CoopHostMenu
