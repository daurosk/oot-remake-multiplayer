// CoopHostMenu (hookshot/src/coop_host_menu.h): Host and Join from the game's
// menu, without the game. The menu's host thread runs the real server; the
// "game" is a real co-op client (coop::net::Client) the connect hook points
// at it, pumped by the test.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "coop/net/client.hpp"
#include "coop_host_menu.h"
#include "nat_codes.hpp"
#include "nat_net.hpp"
#include "stun.hpp"

namespace nat = coop::server::nat;
using CoopHostMenu::Target;

namespace {

struct EnetLife {
    EnetLife() { REQUIRE(enet_initialize() == 0); }
    ~EnetLife() { enet_deinitialize(); }
};

// Answers every STUN Binding request with the sender's own address.
struct FakeStun {
    ENetSocket socket = ENET_SOCKET_NULL;
    FakeStun() {
        socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        ENetAddress local = nat::to_enet(nat::Addr{0x7F000001u, 0});
        REQUIRE(enet_socket_bind(socket, &local) == 0);
        enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1);
    }
    ~FakeStun() { enet_socket_destroy(socket); }
    nat::Addr address() const {
        ENetAddress a{};
        enet_socket_get_address(socket, &a);
        return nat::Addr{0x7F000001u, a.port};
    }
    void pump() {
        std::uint8_t data[512];
        for (;;) {
            ENetAddress from{};
            ENetBuffer in;
            in.data = data;
            in.dataLength = sizeof data;
            const int n = enet_socket_receive(socket, &from, &in, 1);
            if (n < 20) {
                return;
            }
            const nat::Addr who = nat::from_enet(from);
            const auto port = static_cast<std::uint16_t>(who.port ^ 0x2112);
            const std::uint32_t ip = who.ip ^ nat::kStunCookie;
            std::vector<std::uint8_t> out = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
            out.insert(out.end(), data + 8, data + 20);
            const std::vector<std::uint8_t> attr = {
                0x00, 0x20, 0x00, 0x08, 0x00, 0x01, static_cast<std::uint8_t>(port >> 8),
                static_cast<std::uint8_t>(port), static_cast<std::uint8_t>(ip >> 24),
                static_cast<std::uint8_t>(ip >> 16), static_cast<std::uint8_t>(ip >> 8), static_cast<std::uint8_t>(ip)};
            out.insert(out.end(), attr.begin(), attr.end());
            ENetBuffer reply;
            reply.data = out.data();
            reply.dataLength = out.size();
            enet_socket_send(socket, &from, &reply, 1);
        }
    }
};

// The game: what the connect hook was told, and a real client started there.
struct Game {
    std::mutex mutex;
    std::optional<Target> asked;
    int connects = 0;
    std::atomic<int> disconnects{0};
    std::unique_ptr<coop::net::Client> client;

    CoopHostMenu::Hooks hooks() {
        CoopHostMenu::Hooks h;
        h.connect = [this](const Target& t) {
            std::lock_guard lock(mutex);
            asked = t;
            ++connects;
            return true;
        };
        h.disconnect = [this] { ++disconnects; };
        h.default_name = [] { return std::string("Saria"); };
        h.stun_servers = std::vector<nat::Addr>{};  // no internet in tests
        return h;
    }
    std::optional<Target> target() {
        std::lock_guard lock(mutex);
        return asked;
    }
    // Starts the real client where the hook was pointed.
    void start_client() {
        const auto t = target();
        REQUIRE(t.has_value());
        coop::net::ClientConfig config;
        config.host = t->server;
        config.port = t->port;
        config.hello.build_id = "host menu test";
        config.hello.game_build_id = "e0f3ccfaf50065ae";
        config.hello.game_version = "1.1.1";
        config.hello.nickname = t->name;
        config.join_password = t->password;
        client = std::make_unique<coop::net::Client>(std::move(config));
        REQUIRE(client->start());
    }
};

HosCoopStatus status_of(const CoopHostMenu::Menu& menu) {
    HosCoopStatus s{};
    s.size = sizeof s;
    menu.Status(&s);
    return s;
}

// Waits up to `ms` for `done`, pumping what the test owns.
bool wait_for(const std::function<bool()>& done, int ms, const std::function<void()>& pump = {}) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pump) {
            pump();
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

HosCoopHostOptions host_options() {
    HosCoopHostOptions o{};
    o.size = sizeof o;
    o.port = 0;  // 27510, or one of the next few
    return o;
}

} // namespace

TEST_CASE("host menu: Host makes a password, runs the server and points this game at it, and the game joins") {
    EnetLife enet;
    Game game;
    CoopHostMenu::Menu menu(game.hooks());
    CHECK(status_of(menu).state == HOS_COOP_IDLE);
    const HosCoopHostOptions o = host_options();
    REQUIRE(menu.HostStart(&o) == 1);
    REQUIRE(wait_for([&] { return status_of(menu).state == HOS_COOP_HOSTING && game.target().has_value(); }, 3000));
    const HosCoopStatus s = status_of(menu);
    CHECK(s.port >= 27510);
    CHECK(s.port < 27515);
    CHECK(nat::is_made_password(s.password));  // a new random one: nobody types it
    const Target t = *game.target();
    CHECK(t.server == "127.0.0.1");
    CHECK(t.port == s.port);
    CHECK(t.name == "Saria");  // coop.ini's, as none was given
    CHECK(t.password == std::string(s.password));

    game.start_client();
    REQUIRE(wait_for([&] { return status_of(menu).players == 1; }, 5000, [&] { game.client->service(1); }));
    CHECK(game.client->state() == coop::net::State::connected);

    // A second Host while hosting is refused, and says why.
    CHECK(menu.HostStart(&o) == 0);
    CHECK(std::string(status_of(menu).message).find("already hosting") != std::string::npos);
    CHECK(status_of(menu).problem == HOS_COOP_PROBLEM_ALREADY_RUNNING);
    // No invite was asked for, so no answer code is taken.
    CHECK(menu.Answer("ABCDE-FGHJK") == 0);

    game.client->stop();
    menu.Leave();
    CHECK(game.disconnects == 1);
    CHECK(status_of(menu).state == HOS_COOP_IDLE);
    CHECK(status_of(menu).password[0] == '\0');
    menu.Leave();  // twice is fine
    CHECK(game.disconnects == 1);
}

TEST_CASE("host menu: Host with a bad password or name is refused before anything listens") {
    Game game;
    CoopHostMenu::Menu menu(game.hooks());
    HosCoopHostOptions o = host_options();
    o.password = "has a space";
    CHECK(menu.HostStart(&o) == 0);
    CHECK(status_of(menu).state == HOS_COOP_FAILED);
    CHECK(std::string(status_of(menu).message).find("join password") != std::string::npos);
    CHECK(status_of(menu).problem == HOS_COOP_PROBLEM_BAD_PASSWORD);
    o.password = nullptr;
    o.name = "a name far longer than sixteen";
    CHECK(menu.HostStart(&o) == 0);
    CHECK(std::string(status_of(menu).message).find("16 characters") != std::string::npos);
    CHECK(status_of(menu).problem == HOS_COOP_PROBLEM_BAD_NAME);
    CHECK(game.connects == 0);
}

TEST_CASE("host menu: an older caller's smaller struct is read only as far as it goes") {
    EnetLife enet;
    Game game;
    CoopHostMenu::Menu menu(game.hooks());
    HosCoopHostOptions o = host_options();
    o.name = "Malon";
    o.upnp = 1;  // past the size below: not read, so no router is asked
    o.size = static_cast<std::uint32_t>(offsetof(HosCoopHostOptions, upnp));
    REQUIRE(menu.HostStart(&o) == 1);
    REQUIRE(wait_for([&] { return game.target().has_value(); }, 3000));
    CHECK(game.target()->name == "Malon");
    CHECK(status_of(menu).upnp_open == 0);
    // A status struct of an older caller is filled only as far as it goes.
    HosCoopStatus older{};  // ("small" is a macro in windows.h)
    std::memset(&older, 0x7f, sizeof older);
    older.size = static_cast<std::uint32_t>(offsetof(HosCoopStatus, invite));
    menu.Status(&older);
    CHECK(older.state == HOS_COOP_HOSTING);
    CHECK(older.invite[0] == 0x7f);  // not touched
    menu.Leave();
}

TEST_CASE("host menu: Join by address needs the password, and with it the game connects there") {
    Game game;
    CoopHostMenu::Menu menu(game.hooks());
    HosCoopJoinOptions o{};
    o.size = sizeof o;
    o.target = "";
    CHECK(menu.Join(&o) == 0);
    CHECK(std::string(status_of(menu).message).find("invite code") != std::string::npos);
    o.target = "127.0.0.1:27600";
    CHECK(menu.Join(&o) == 0);
    CHECK(std::string(status_of(menu).message).find("join password") != std::string::npos);
    CHECK(status_of(menu).problem == HOS_COOP_PROBLEM_NO_PASSWORD);
    o.password = "pw-direct";
    o.name = "Link";
    REQUIRE(menu.Join(&o) == 1);
    CHECK(status_of(menu).state == HOS_COOP_CONNECTED);
    const Target t = *game.target();
    CHECK(t.server == "127.0.0.1");
    CHECK(t.port == 27600);
    CHECK(t.name == "Link");
    CHECK(t.password == "pw-direct");
    menu.Leave();
    CHECK(game.disconnects == 1);
    CHECK(status_of(menu).state == HOS_COOP_IDLE);
}

TEST_CASE("host menu: a broken invite code is explained, not used as an address") {
    Game game;
    CoopHostMenu::Menu menu(game.hooks());
    HosCoopJoinOptions o{};
    o.size = sizeof o;
    o.target = "ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567";  // the right letters, not a real code
    CHECK(menu.Join(&o) == 0);
    CHECK(status_of(menu).state == HOS_COOP_FAILED);
    CHECK(game.connects == 0);
}

TEST_CASE("host menu: Host with an invite code, and a second game joins with only that code, through its helper") {
    EnetLife enet;
    FakeStun stun;
    Game host_game;
    CoopHostMenu::Hooks hh = host_game.hooks();
    std::mutex log_mutex;
    std::vector<std::string> log;
    hh.log = [&](const std::string& line) {
        std::lock_guard lock(log_mutex);
        log.push_back(line);
    };
    hh.stun_servers = std::vector<nat::Addr>{stun.address()};
    hh.allow_private = true;  // 127.0.0.1 is not an internet address
    CoopHostMenu::Menu host(hh);
    HosCoopHostOptions o = host_options();
    o.invite = 1;
    REQUIRE(host.HostStart(&o) == 1);
    REQUIRE(wait_for([&] { return status_of(host).invite_done != 0; }, 5000, [&] { stun.pump(); }));
    const HosCoopStatus hs = status_of(host);
    REQUIRE(hs.invite[0] != '\0');
    const nat::InviteRead read = nat::read_invite(hs.invite);
    // 127.0.0.1 is not an internet address: read, but refused for a real join.
    REQUIRE(read.error == nat::CodeError::kNotPublic);
    CHECK(read.invite.host.ip == 0x7F000001u);
    CHECK(read.invite.host.port == hs.port);
    CHECK(read.invite.password == std::string(hs.password));
    host_game.start_client();

    Game guest_game;
    CoopHostMenu::Hooks gh = guest_game.hooks();
    gh.log = hh.log;
    gh.stun_servers = std::vector<nat::Addr>{stun.address()};
    gh.helper_port = 0;  // any free one
    gh.allow_private = true;  // the invite holds 127.0.0.1
    CoopHostMenu::Menu guest(gh);
    HosCoopJoinOptions j{};
    j.size = sizeof j;
    j.target = hs.invite;
    j.name = "Navi";
    REQUIRE(guest.Join(&j) == 1);
    // The host's server answers the helper's signed probe: no answer code.
    REQUIRE(wait_for([&] { return status_of(guest).state == HOS_COOP_CONNECTED; }, 8000,
                     [&] {
                         stun.pump();
                         host_game.client->service(0);
                     }));
    CHECK(status_of(guest).answer[0] == '\0');
    const Target t = *guest_game.target();
    CHECK(t.server == "127.0.0.1");
    CHECK(t.port != hs.port);  // the helper's port, not the host's
    CHECK(t.password == std::string(hs.password));
    guest_game.start_client();
    REQUIRE(wait_for([&] { return status_of(host).players == 2; }, 8000, [&] {
        host_game.client->service(0);
        guest_game.client->service(0);
    }));
    CHECK(guest_game.client->state() == coop::net::State::connected);

    guest_game.client->stop();
    guest.Leave();
    host_game.client->stop();
    host.Leave();
    CHECK(guest_game.disconnects == 1);
    CHECK(host_game.disconnects == 1);
    // The game's log never holds the invite code or the join password.
    std::lock_guard lock(log_mutex);
    CHECK_FALSE(log.empty());
    for (const auto& line : log) {
        CHECK(line.find(hs.invite) == std::string::npos);
        CHECK(line.find(hs.password) == std::string::npos);
    }
}

TEST_CASE("host menu: CopyText cuts to fit and always ends the string") {
    char out[6];
    CoopHostMenu::CopyText(out, sizeof out, "abcdefgh");
    CHECK(std::string(out) == "abcde");
    CoopHostMenu::CopyText(out, sizeof out, "ab");
    CHECK(std::string(out) == "ab");
}

TEST_CASE("host menu: Host takes up to 16 players by default, as coop_server") {
    CHECK(CoopHostMenu::HostMaxPlayers(0) == 16);  // none asked: the server's default (kDefaultMaxPlayers)
    CHECK(CoopHostMenu::HostMaxPlayers(4) == 4);
    CHECK(CoopHostMenu::HostMaxPlayers(16) == 16);
    CHECK(CoopHostMenu::HostMaxPlayers(40) == 16);  // more than the protocol has numbers for
}

TEST_CASE("host menu: the same helper nonce and port for the same invite on this PC, others differ") {
    const coop::server::nat::Addr host{0xCB007105u, 27510};
    const auto a = CoopHostMenu::RejoinIds("pc-one", "PW", host);
    const auto b = CoopHostMenu::RejoinIds("pc-one", "PW", host);
    CHECK(a.nonce == b.nonce);
    CHECK(a.public_port == b.public_port);
    CHECK(a.public_port >= 40000);
    CHECK(a.public_port < 60000);
    CHECK(CoopHostMenu::RejoinIds("pc-two", "PW", host).nonce != a.nonce);   // two friends, one invite
    CHECK(CoopHostMenu::RejoinIds("pc-one", "PW2", host).nonce != a.nonce);  // another session
}

TEST_CASE("host menu: Paste friend's answer says plainly what was wrong with the clipboard") {
    using CoopHostMenu::ReadPastedAnswer;
    namespace nat = coop::server::nat;
    constexpr std::uint64_t now = 1'800'000'000;
    nat::Answer a;
    a.guest = nat::Addr{0xC6336402u, 40001};
    a.nonce = {1, 2, 3, 4, 5, 6, 7, 8};
    a.time = static_cast<std::uint32_t>(now);
    const std::string answer = nat::make_answer(a, "join-pw");
    const std::string invite = nat::make_invite(nat::Invite{nat::Addr{0xCB007105u, 27510}, "join-pw"}).value();

    const auto taken = ReadPastedAnswer("ROXAS \xE2\x80\x94 Today at 9:41 PM\n`" + answer + "`", invite, "join-pw", now);
    CHECK(taken.ok);
    CHECK(taken.line.find("taken") != std::string::npos);

    const auto locked = ReadPastedAnswer(std::nullopt, invite, "join-pw", now);
    CHECK_FALSE(locked.ok);
    CHECK(locked.line.find("could not be read") != std::string::npos);
    for (const std::string empty : {std::string(), std::string(" \r\n"), std::string("\xE2\x80\x8B")}) {
        const auto e = ReadPastedAnswer(empty, invite, "join-pw", now);
        CHECK_FALSE(e.ok);
        CHECK(e.line.find("clipboard is empty") != std::string::npos);
    }
    const auto own = ReadPastedAnswer(invite, invite, "join-pw", now);
    CHECK_FALSE(own.ok);
    CHECK(own.line.find("your own invite code") != std::string::npos);
    const auto own_in_chat = ReadPastedAnswer("me: " + invite, invite, "join-pw", now);
    CHECK(own_in_chat.line.find("your own invite code") != std::string::npos);
    const auto other_invite =
        ReadPastedAnswer(nat::make_invite(nat::Invite{nat::Addr{0xCB007106u, 27510}, "x"}).value(), invite, "join-pw", now);
    CHECK(other_invite.line.find("invite code, not an answer code") != std::string::npos);
    const auto words = ReadPastedAnswer("send me the code pls", invite, "join-pw", now);
    CHECK(words.line.find("No answer code found") != std::string::npos);
    const auto stale = ReadPastedAnswer(answer, invite, "join-pw", now + 3600);
    CHECK(stale.line.find("10 minutes") != std::string::npos);
    const auto old_invite = ReadPastedAnswer(answer, invite, "other-pw", now);
    CHECK(old_invite.line.find("another invite code") != std::string::npos);
}
