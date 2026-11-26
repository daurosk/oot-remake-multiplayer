// SPDX-License-Identifier: GPL-3.0-or-later
// Games on this network (hookshot/src/coop_lan.h): the announcement's datagram and the list of games heard.
#include <doctest/doctest.h>

#include "coop_lan.h"

using namespace CoopLan;

TEST_CASE("lan: an announcement reads back as the game it names, and never says the join password") {
    Game g;
    g.name = "Saria";
    g.port = 27510;
    g.players = 2;
    g.max = 4;
    g.room = "forest-gang";
    g.protocol = 14;
    const std::string d = Encode(g);
    CHECK(d.rfind("SQUADALA LAN 1\n", 0) == 0);
    CHECK(d.find("password") == std::string::npos);
    const auto back = Decode(d);
    REQUIRE(back);
    CHECK(back->name == "Saria");
    CHECK(back->port == 27510);
    CHECK(back->players == 2);
    CHECK(back->max == 4);
    CHECK(back->room == "forest-gang");
    CHECK(back->protocol == 14);
}

TEST_CASE("lan: anything else is not a game") {
    CHECK_FALSE(Decode(""));
    CHECK_FALSE(Decode("hello"));
    CHECK_FALSE(Decode("SQUADALA LAN 10\nport=1\nprotocol=14\n"));   // (another format)
    CHECK_FALSE(Decode("SQUADALA LAN 1\nname=x\nprotocol=14\n"));    // (no port)
    CHECK_FALSE(Decode("SQUADALA LAN 1\nport=0\nprotocol=14\n"));
    CHECK_FALSE(Decode("SQUADALA LAN 1\nport=70000\nprotocol=14\n"));
    CHECK_FALSE(Decode("SQUADALA LAN 1\nport=27510\n"));              // (no protocol)
    CHECK_FALSE(Decode(std::string(600, 'x')));
    const auto odd = Decode("SQUADALA LAN 1\nport=27510\nprotocol=14\nlater=thing\nplayers=abc\n");
    REQUIRE(odd);   // (an unknown key and a bad count: left alone)
    CHECK(odd->players == 0);
    CHECK(odd->name == "A game");
}

TEST_CASE("lan: a name loses its control characters and is cut at 32 bytes, on a UTF-8 letter") {
    Game g;
    g.port = 1;
    g.name = "Sa\nria\x01";
    CHECK(Decode(Encode(g))->name == "Saria");
    g.name = std::string(31, 'a') + "\xc3\xa9" + "zz";   // (an e with an accent across the 32nd byte)
    const std::string name = Decode(Encode(g))->name;
    CHECK(name.size() == 31);
    CHECK(name == std::string(31, 'a'));
}

TEST_CASE("lan: the list keeps one row per address and port, drops the quiet ones, and keeps a steady order") {
    List l;
    Game a;
    a.name = "Saria", a.port = 27510, a.protocol = 14, a.max = 4, a.players = 1;
    Game b = a;
    b.name = "Mido";
    l.Add("192.168.1.20", a, 1000);
    l.Add("192.168.1.30", b, 1500);
    a.players = 2;
    l.Add("192.168.1.20", a, 2000);   // (the same game again: one row, its newest word)
    auto now = l.Now(2500);
    REQUIRE(now.size() == 2);
    CHECK(now[0].game.name == "Mido");   // (by name)
    CHECK(now[1].game.players == 2);
    CHECK(l.Now(1500 + kGoneMs).size() == 1);   // (Mido quiet that long: gone)
    CHECK(l.Now(2000 + kGoneMs).empty());
}

TEST_CASE("lan: a game's row says its players, full, or another version, and only those it can join join") {
    Heard h;
    h.game.name = "Saria", h.game.port = 27510, h.game.protocol = 14, h.game.players = 2, h.game.max = 4;
    CHECK(RowText(h, 14) == "Saria's game (2/4)");
    CHECK(Joinable(h, 14));
    h.game.players = 4;
    CHECK(RowText(h, 14) == "Saria's game (full)");
    CHECK_FALSE(Joinable(h, 14));
    h.game.players = 1;
    CHECK(RowText(h, 13) == "Saria's game (other version)");
    CHECK_FALSE(Joinable(h, 13));
}

TEST_CASE("lan: a game on this PC heard at 127.0.0.1 and at its network address is one row") {
    List l;
    Game g;
    g.name = "Saria", g.port = 27510, g.protocol = 14;
    l.Add("127.0.0.1", g, 1000);
    CHECK(l.Now(1100).size() == 1);   // (only the loopback: kept)
    l.Add("192.168.1.20", g, 1050);
    const auto now = l.Now(1100);
    REQUIRE(now.size() == 1);
    CHECK(now[0].address == "192.168.1.20");
}

TEST_CASE("lan: a game heard from each of its networks is one row, at the address first heard") {
    List l;
    Game g;
    g.name = "Saria", g.port = 27510, g.protocol = 14, g.id = "0123456789abcdef";
    l.Add("192.168.1.20", g, 1000);
    l.Add("172.20.0.1", g, 1010);     // (a virtual switch on the host's PC)
    l.Add("100.64.0.7", g, 1020);     // (a VPN)
    l.Add("127.0.0.1", g, 1030);
    g.players = 2;
    l.Add("172.20.0.1", g, 1040);
    const auto now = l.Now(1100);
    REQUIRE(now.size() == 1);
    CHECK(now[0].address == "192.168.1.20");
    CHECK(now[0].game.players == 2);
    CHECK(l.Now(1040 + kGoneMs - 1).size() == 1);   // (each word keeps the row)
}

TEST_CASE("lan: two games with their own ids are two rows, and an id a loopback row heard first moves to the network") {
    List l;
    Game a, b;
    a.name = b.name = "Saria", a.port = b.port = 27510, a.protocol = b.protocol = 14;
    a.id = "0123456789abcdef", b.id = "fedcba9876543210";
    l.Add("127.0.0.1", a, 1000);
    l.Add("192.168.1.20", a, 1010);
    l.Add("192.168.1.30", b, 1020);
    const auto now = l.Now(1100);
    REQUIRE(now.size() == 2);
    CHECK(now[0].address == "192.168.1.20");
    CHECK(now[1].address == "192.168.1.30");
}

TEST_CASE("lan: the id goes through the datagram, and a new one is 16 hex digits and different each time") {
    Game g;
    g.name = "Saria", g.port = 27510, g.protocol = 14;
    g.id = NewId();
    CHECK(g.id.size() == kIdLength);
    CHECK(g.id.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(NewId() != NewId());
    auto back = Decode(Encode(g));
    REQUIRE(back);
    CHECK(back->id == g.id);
    back = Decode("SQUADALA LAN 1\nport=27510\nprotocol=14\nid=NOT-AN-ID\n");   // (not one: no id, still a game)
    REQUIRE(back);
    CHECK(back->id.empty());
}

TEST_CASE("lan: join_lan picks the first game that can be joined, or the named host's") {
    const int mine = 13;
    const auto game = [&](std::string name, std::string address, int players, int max, int protocol) {
        Heard h;
        h.address = std::move(address);
        h.game.name = std::move(name);
        h.game.port = 27510;
        h.game.players = players;
        h.game.max = max;
        h.game.protocol = protocol;
        return h;
    };
    const std::vector<Heard> heard = {game("Saria", "192.168.1.20", 4, 4, mine),      // (full)
                                      game("Mido", "192.168.1.21", 1, 4, mine + 1),   // (another version)
                                      game("Link", "192.168.1.22", 1, 4, mine),
                                      game("Darunia", "192.168.1.23", 2, 4, mine)};
    auto p = PickToJoin(heard, "first", mine);
    REQUIRE(p);
    CHECK(p->game.name == "Link");   // (the first that can be joined: not full, the same version)
    p = PickToJoin(heard, "", mine);
    REQUIRE(p);
    CHECK(p->game.name == "Link");
    p = PickToJoin(heard, "darunia", mine);   // (a host's name, any case)
    REQUIRE(p);
    CHECK(p->address == "192.168.1.23");
    CHECK_FALSE(PickToJoin(heard, "Saria", mine));   // (that host's game is full: none)
    CHECK_FALSE(PickToJoin(heard, "Ruto", mine));
    CHECK_FALSE(PickToJoin({}, "first", mine));
}
