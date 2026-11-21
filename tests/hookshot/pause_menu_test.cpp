// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab of Hookshot's pause menu (hookshot/src/coop_pause_menu.h): its pages, what a press asks the
// module to do, and what Hookshot is given to draw.
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "coop_colors.h"
#include "coop_pause_menu.h"
#include "hos_coop_menu.h"

using namespace CoopPauseMenu;
using State = Session::State;

namespace {

std::vector<std::string> Names(const Menu& m) {
    std::vector<std::string> out;
    for (const auto& r : m.view().rows) {
        out.push_back(r.name);
    }
    return out;
}

std::string Selected(const Menu& m) {
    const View v = m.view();
    return v.selected >= 0 ? v.rows[static_cast<std::size_t>(v.selected)].name : std::string();
}

std::string SelectedValue(const Menu& m) {
    const View v = m.view();
    return v.selected >= 0 ? v.rows[static_cast<std::size_t>(v.selected)].value : std::string();
}

void SelectRow(Menu& m, const std::string& name) {
    for (int i = 0; i < 20 && Selected(m) != name; ++i) {
        m.Press(Input::kDown);
    }
    REQUIRE(Selected(m) == name);
}

Session Hosting(int players, std::string code = "ABCDE-FGHJK") {
    Session s;
    s.state = State::kHosting;
    s.hosting = true;
    s.players = players;
    s.code = std::move(code);
    s.lines = {CoopTitleMenu::HostHeadline(true), s.code};
    return s;
}

Session Joined() {
    Session s;
    s.state = State::kJoined;
    s.hosting = false;
    s.players = 2;
    s.lines = {"Connected! Choose your file and play."};
    return s;
}

}  // namespace

TEST_CASE("pause tab: the row kinds are hos_coop_menu.h's numbers") {
    CHECK(static_cast<int>(RowKind::kAction) == HOS_COOP_MENU_ACTION);
    CHECK(static_cast<int>(RowKind::kChoice) == HOS_COOP_MENU_CHOICE);
    CHECK(static_cast<int>(RowKind::kToggle) == HOS_COOP_MENU_TOGGLE);
    CHECK(static_cast<int>(RowKind::kHeading) == HOS_COOP_MENU_HEADING);
    CHECK(static_cast<int>(RowKind::kNote) == HOS_COOP_MENU_NOTE);
}

TEST_CASE("pause tab: the first page is Host, Join, Local co-op, Settings and Commands, Back there closes the menu") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    CHECK(m.page() == Page::kMain);
    CHECK(Names(m) == std::vector<std::string>{"Host a game", "Join a game", "Local co-op", "Settings", "Customize Link", "Commands"});
    CHECK(Selected(m) == "Host a game");
    m.Press(Input::kUp);   // round the ends
    CHECK(Selected(m) == "Commands");
    m.Press(Input::kDown);
    CHECK(Selected(m) == "Host a game");
    const View v = m.view();
    CHECK(v.help_title == "Multiplayer");
    CHECK(v.confirm == "Open");
    CHECK(v.back == "Close");
    CHECK_FALSE(m.Press(Input::kBack));   // nothing to go back to: Hookshot closes the menu
    CHECK_FALSE(m.TakeRequest().has_value());
}

TEST_CASE("pause tab: Host hosts in one press, then the page shows the code, the players and Stop") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kHost);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kHost);
    Session starting;
    starting.state = State::kStarting;
    starting.hosting = true;
    m.SetSession(starting);
    CHECK(m.view().gold == "Starting your game...");
    m.SetSession(Hosting(1));
    CHECK(Names(m) == std::vector<std::string>{"Copy the code again", "Paste friend's answer (or Ctrl+V)",
                                               "Same network", "VPN", "Port", "Stop hosting"});
    // Ctrl+V (or Shift+Insert) on the Host page is Paste friend's answer, wherever the cursor is.
    CHECK(m.Press(Input::kPaste));
    const auto paste = m.TakeRequest();
    REQUIRE(paste.has_value());
    CHECK(paste->kind == Request::Kind::kPasteAnswer);
    View v = m.view();
    CHECK(v.hint == "ABCDE-FGHJK");
    CHECK(v.gold == "Hosting: waiting for friends");
    CHECK(v.help.find("Invite code copied to your clipboard") == 0);   // what the host does, in place of the row's help
    m.SetSession(Hosting(3));
    CHECK(m.view().gold == "Hosting: 3 players");
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kCopyInvite);
    m.Press(Input::kDown);
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kPasteAnswer);
    m.Press(Input::kDown);
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kCopyLan);
    m.Press(Input::kDown);
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kCopyVpn);
    // Port row, then Stop hosting
    m.Press(Input::kDown);
    REQUIRE(Selected(m) == "Port");
    m.Press(Input::kDown);
    REQUIRE(Selected(m) == "Stop hosting");
    m.Press(Input::kDecide);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.view().rows[5].value == "Press again to confirm");
    m.Press(Input::kUp);   // anything else: not confirmed
    m.Press(Input::kDown);
    CHECK(m.view().rows[5].value == "Stop");
    m.Press(Input::kDecide);
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kLeave);
    // the first page says it is hosting, and Join waits
    m.Press(Input::kBack);
    CHECK(m.page() == Page::kMain);
    CHECK(Selected(m) == "Host a game");
    CHECK(m.view().rows[0].value == "Hosting");
    CHECK(m.view().rows[1].fixed);
    m.Press(Input::kDown);
    const std::string hover = m.view().help;   // what selecting the dim row shows already
    m.Press(Input::kDecide);   // a fixed row says why, and does nothing else
    CHECK(m.page() == Page::kMain);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.view().help.find("stop yours first") != std::string::npos);
    CHECK(m.view().help.rfind("Not now.", 0) == 0);   // the press is seen: the help changes
    CHECK(m.view().help != hover);
    m.Press(Input::kUp);
    m.Press(Input::kDown);
    m.Click(1, 0);   // a click on it: the same answer
    CHECK(m.page() == Page::kMain);
    CHECK(m.view().help.rfind("Not now.", 0) == 0);
}

TEST_CASE("pause tab: the host's player count goes round 2 to 16 and is saved") {
    Menu m;
    CoopTitleMenu::Options o;
    o.max_players = 4;
    m.Show(o);
    m.Open(Page::kHost);
    REQUIRE(m.TakeRequest()->kind == Request::Kind::kHost);
    m.SetSession(Session{});   // (it did not start: the Start row and the count)
    SelectRow(m, "Players");
    m.Press(Input::kLeft);
    m.Press(Input::kLeft);
    CHECK(m.options().max_players == 2);
    m.Press(Input::kLeft);
    CHECK(m.options().max_players == 16);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    m.Press(Input::kRight);
    CHECK(m.options().max_players == 2);
}

TEST_CASE("pause tab: Join: a room name or an address, its port and the password, then Join (no friend's code)") {
    Menu m;
    CoopTitleMenu::Options o;
    o.relay = "relay.example.net:27530";
    m.Show(o);
    SelectRow(m, "Join a game");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kJoin);
    CHECK(Names(m) == std::vector<std::string>{"Browse games", "Room or address", "Port", "Password", "Join"});
    CHECK(m.view().rows[1].value == "Type");
    CHECK(m.view().rows[2].value == "27510");
    CHECK(m.view().rows[4].fixed);   // nothing to join yet
    const auto type = [&](const std::string& row, const std::string& text) {
        SelectRow(m, row);
        m.Press(Input::kDecide);
        REQUIRE(m.typing());
        for (const char c : text) m.Char(static_cast<unsigned char>(c));
        m.Press(Input::kDecide);
    };
    // A room name: through the relay, with the password
    type("Room or address", "forest-gang");
    type("Password", "kokiri");
    CHECK(m.join_text() == "forest-gang@relay.example.net:27530 kokiri");
    CHECK(m.view().rows[3].value == "******");
    SelectRow(m, "Join");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "forest-gang@relay.example.net:27530 kokiri");
    // An address: directly, with the password
    Menu d;
    d.Show(CoopTitleMenu::Options{});
    d.Open(Page::kJoin);
    SelectRow(d, "Room or address");
    d.Press(Input::kDecide);
    for (const char c : std::string("192.168.1.20")) d.Char(static_cast<unsigned char>(c));
    d.Press(Input::kDecide);
    SelectRow(d, "Password");
    d.Press(Input::kDecide);
    for (const char c : std::string("kokiri")) d.Char(static_cast<unsigned char>(c));
    d.Press(Input::kDecide);
    CHECK(d.join_text().rfind("192.168.1.20", 0) == 0);
    CHECK(d.join_text().find("kokiri") != std::string::npos);
    CHECK(d.join_text().find('@') == std::string::npos);
}

TEST_CASE("pause tab: Join offers the last game again first, one press") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);
    m.SetRejoin("Saria", "ABCDE-FGHJK");
    CHECK(Names(m) == std::vector<std::string>{"Join Saria again", "Browse games", "Room or address", "Port", "Password", "Join"});
    CHECK(Selected(m) == "Join Saria again");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "ABCDE-FGHJK");
    m.SetRejoin("", "ABCDE-FGHJK");
    CHECK(m.view().rows[0].name == "Join your friend again");
    m.SetRejoin("Saria", "");
    CHECK(m.view().rows[0].name == "Browse games");
}

TEST_CASE("pause tab: Find a game lists what is heard, searches the relay's rooms, and a pick fills the Join page") {
    Menu m;
    CoopTitleMenu::Options o;
    o.relay = "relay.example.net:27530";
    m.Show(o);
    m.Open(Page::kJoin);
    SelectRow(m, "Browse games");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kFind);
    CHECK(m.find_online());
    CHECK(Names(m) == std::vector<std::string>{"List", "Search", "Show", "Sort", "Refresh", "Asking the relay..."});
    m.SetFound({{"kokiri (Saria)", "1/4", "kokiri", 0, true, "Saria", 1, 4, true},
                {"forest (Mido)", "Full", "forest", 0, false, "Mido", 4, 4, true}}, "");
    // (the most players first)
    CHECK(Names(m) == std::vector<std::string>{"List", "Search", "Show", "Sort", "Refresh", "forest (Mido)", "kokiri (Saria)"});
    CHECK(m.view().rows[5].fixed);   // (full: not joinable)
    SelectRow(m, "Show");   // (only the ones this game can join)
    m.Press(Input::kRight);
    CHECK(Names(m) == std::vector<std::string>{"List", "Search", "Show", "Sort", "Refresh", "kokiri (Saria)"});
    m.Press(Input::kLeft);
    SelectRow(m, "Sort");   // (by name)
    m.Press(Input::kLeft);
    CHECK(Names(m).at(5) == "forest (Mido)");
    m.Press(Input::kRight);
    // The search: only the rooms that hold it, any case
    SelectRow(m, "Search");
    m.Press(Input::kDecide);
    REQUIRE(m.typing());
    for (const char c : std::string("MID")) m.Char(static_cast<unsigned char>(c));
    m.Press(Input::kDecide);
    CHECK(Names(m) == std::vector<std::string>{"List", "Search", "Show", "Sort", "Refresh", "forest (Mido)"});
    SelectRow(m, "Search");
    m.Press(Input::kDecide);
    for (const char c : std::string("xyz")) m.Char(static_cast<unsigned char>(c));
    m.Press(Input::kDecide);
    CHECK(Names(m).back() == "None match the search or Show");
    SelectRow(m, "Search");   // (Esc: as it was)
    m.Press(Input::kDecide);
    m.Press(Input::kErase);
    m.Press(Input::kBack);
    CHECK(m.find_search() == "MIDxyz");
    SelectRow(m, "Search");
    m.Press(Input::kDecide);
    for (int i = 0; i < 6; ++i) m.Press(Input::kErase);
    m.Press(Input::kDecide);
    CHECK(m.find_search().empty());
    // A room picked: the Join page with its name, through the relay, the password next
    SelectRow(m, "kokiri (Saria)");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kJoin);
    CHECK(Selected(m) == "Password");
    CHECK(m.join_text() == "kokiri@relay.example.net:27530");
    // This network's games: an address and its port, Join next; B goes back to the Join page
    SelectRow(m, "Browse games");
    m.Press(Input::kDecide);
    SelectRow(m, "List");
    m.Press(Input::kRight);
    CHECK_FALSE(m.find_online());
    CHECK(Names(m) == std::vector<std::string>{"List", "Search", "Show", "Sort", "Refresh", "Looking for games..."});
    m.SetFound({{"Saria's game", "1/4", "192.168.1.20", 27511, true}}, "");
    SelectRow(m, "Saria's game");
    m.Press(Input::kDecide);
    CHECK(Selected(m) == "Join");
    CHECK(m.join_text() == "192.168.1.20:27511");
    SelectRow(m, "Browse games");
    m.Press(Input::kDecide);
    m.Press(Input::kBack);
    CHECK(m.page() == Page::kJoin);
    CHECK(Selected(m) == "Browse games");
}

TEST_CASE("pause tab: a guest sees the friend's game and Leave, a blocked way in offers the answer code") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);
    m.Paste("ABCDE-FGHJK");
    m.Press(Input::kDecide);
    REQUIRE(m.TakeRequest()->kind == Request::Kind::kJoin);
    Session wait;
    wait.state = State::kWaitHost;
    wait.code = "ANSWR-CODE1";
    wait.lines = {"Your friend's router blocks the way in."};
    m.SetSession(wait);
    CHECK(Names(m) == std::vector<std::string>{"Copy your answer code", "Leave the game"});
    CHECK(m.view().hint == "ANSWR-CODE1");
    CHECK(m.view().gold == "Joining...");
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kCopyAnswer);
    m.SetSession(Joined());
    CHECK(Names(m) == std::vector<std::string>{"In your friend's game", "Leave the game"});   // (the title: Back under them)
    CHECK(m.view().gold == "In a friend's game");
    m.Press(Input::kDecide);
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kLeave);
    m.Press(Input::kBack);
    CHECK(m.view().rows[1].value == "Joined");
    CHECK(m.view().rows[0].fixed);   // Host waits until the guest leaves
}

TEST_CASE("pause tab: co-op that came with the game's start is shown, not started or stopped here") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    Session s;
    s.by_launcher = true;
    s.state = State::kJoined;   // (the harness's and Join.bat's: connected from the start)
    m.SetSession(s);
    // Host and Join are not dim: a press opens their page, which says what is on (the owner, 2026-11-19: "Clicking
    // on Host Game or Join Game doesn't do anything" with co-op from the game's start)
    CHECK_FALSE(m.view().rows[0].fixed);
    CHECK_FALSE(m.view().rows[1].fixed);
    CHECK(m.view().rows[0].value == "Co-op is on");
    CHECK(m.view().rows[1].value == "Co-op is on");
    CHECK(m.view().gold == "Co-op is on");
    CHECK(m.view().help.find("Host.bat") != std::string::npos);
    s.players = 3;
    m.SetSession(s);
    CHECK(m.view().gold == "Co-op is on: 3 players");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kHost);
    CHECK_FALSE(m.TakeRequest().has_value());   // nothing is started
    const std::vector<std::string> launcher{"Co-op is on: 3 players",
                                            "Co-op started with the game: Host.bat, Join.bat or coop.ini.",
                                            "Who is in the game: the Lobby tab.",
                                            "To host or join another game, start the game again."};
    CHECK(Names(m) == launcher);
    CHECK(m.view().selected == -1);   // notes only: nothing to press
    CHECK(m.view().help.find("Host.bat") != std::string::npos);
    m.Press(Input::kDecide);
    m.Click(0, 0);
    CHECK(m.page() == Page::kHost);
    CHECK_FALSE(m.TakeRequest().has_value());
    m.Press(Input::kBack);
    CHECK(m.page() == Page::kMain);
    CHECK(Selected(m) == "Host a game");
    m.Press(Input::kDown);
    m.Click(1, 0);   // a click opens Join's page the same way
    CHECK(m.page() == Page::kJoin);
    CHECK(Names(m) == launcher);
    CHECK_FALSE(m.TakeRequest().has_value());
    m.Press(Input::kBack);
    SelectRow(m, "Settings");   // the settings still open
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kSettings);
}

TEST_CASE("pause tab: Local co-op lists the experimental split screen steps") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kLocal);
    CHECK(m.page() == Page::kLocal);
    const View v = m.view();
    REQUIRE(v.rows.size() == 5);
    for (const auto& row : v.rows) {
        CHECK(row.kind == RowKind::kNote);
    }
    CHECK(v.rows[0].name.find("experimental") != std::string::npos);
    CHECK(v.rows[2].name.find("Settings > Controller") != std::string::npos);
    CHECK(v.rows[4].name.find("PLAY-LAN.md") != std::string::npos);
    CHECK(v.selected == -1);   // nothing to choose
    CHECK(v.help.find("10 GB") != std::string::npos);   // the help box says what it costs
    CHECK(v.back == "Back");
    CHECK(m.Press(Input::kBack));
    CHECK(Selected(m) == "Local co-op");
}

TEST_CASE("pause tab: Commands lists the chat key and every chat command, each with its help") {
    CoopTitleMenu::Options o;
    o.chat_key = "Y";
    Menu m;
    m.Show(o);
    SelectRow(m, "Commands");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kCommands);
    View v = m.view();
    CHECK(v.help_title == "Commands");
    REQUIRE(v.rows.size() == 1 + Commands().size());
    CHECK(v.rows[0].name == "Open the chat");
    CHECK(v.rows[0].value == "Y");
    CHECK(v.help.find("Press Y") != std::string::npos);
    CHECK(v.help.find("Settings, Chat key") != std::string::npos);
    CHECK(v.help.find("LICHII_") == std::string::npos);   // plain words, no file names
    CHECK(v.confirm.empty());   // read only: nothing happens on a press
    for (std::size_t i = 0; i < Commands().size(); ++i) {
        CHECK(v.rows[i + 1].name == Commands()[i].typed);
    }
    const auto names = Names(m);
    for (const char* typed : {"/teleport <player>", "/tp <player>", "/pvp"}) {
        CHECK(std::find(names.begin(), names.end(), std::string(typed)) != names.end());
    }
    m.Press(Input::kDown);
    CHECK(Selected(m) == "/teleport");
    CHECK(m.view().help.find("Lists the other players") != std::string::npos);
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kCommands);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.Press(Input::kBack));
    CHECK(Selected(m) == "Commands");
    // no chat key
    CHECK(ChatKeyWords("off") == "Off");
    CHECK(ChatKeyWords("") == "T");
    o.chat_key = "off";
    m.Show(o);
    m.Open(Page::kCommands);
    v = m.view();
    CHECK(v.rows[0].value == "Off");
    CHECK(v.help.find("Settings, Chat key") != std::string::npos);
    CHECK(v.help.find("LICHII_") == std::string::npos);
}

TEST_CASE("pause tab: Settings: name, Link's look, players and voice chat (off unless turned on)") {
    Menu m;
    CoopTitleMenu::Options o;
    o.name = "Saria";
    m.Show(o);
    m.Open(Page::kSettings);
    CHECK(Names(m) == std::vector<std::string>{"Your name", "Link's look", "Players when you host",
                                               "Port when you host", "Relay", "Room name", "List my room", "Auto-rejoin",
                                               "Password on this network",
                                               "Proximity voice chat", "Chat key", "Show players on map", "Names on map", "Player list", "New Game+",
                                               "Saves", "Original game"});   // (no patch 0015: no input rows)
    View v = m.view();
    CHECK(v.rows[0].value == "Saria");
    CHECK(v.rows[1].value == "By player number");
    CHECK(v.rows[3].value == "27510");
    CHECK(v.rows[4].value == "Squadala relay");   // (the relay: the built-in one)
    CHECK(v.rows[5].value == "None");   // (no room name)
    CHECK_FALSE(v.rows[6].on);   // (List my room: off unless turned on)
    CHECK_FALSE(v.rows[7].on);   // (Auto-rejoin too)
    CHECK(v.rows[8].value == "Not needed");   // (the password on this network)
    CHECK(v.rows[9].kind == RowKind::kToggle);
    CHECK_FALSE(v.rows[9].on);   // the default
    // the name, typed
    m.Press(Input::kDecide);
    CHECK(m.typing());
    CHECK(m.view().confirm == "Done");
    CHECK(m.view().back == "Cancel");
    m.Char('!');
    CHECK(m.view().rows[0].value == "Saria!_");
    m.Press(Input::kDecide);
    CHECK(m.options().name == "Saria!");
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    // Back while typing: the name as it was
    m.Press(Input::kDecide);
    m.Char('?');
    m.Press(Input::kBack);
    CHECK(m.options().name == "Saria!");
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.page() == Page::kSettings);
    // Link's look: each change asks for it
    m.Press(Input::kDown);
    m.Press(Input::kRight);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kLook);
    CHECK(r->text == m.options().colour);
    CHECK_FALSE(r->text.empty());
    // voice: Right on, Left off, each saved
    SelectRow(m, "Proximity voice chat");
    m.Press(Input::kRight);
    CHECK(m.options().voice);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    m.Press(Input::kLeft);
    CHECK_FALSE(m.options().voice);
    m.Press(Input::kDecide);   // A flips it
    CHECK(m.options().voice);
}

TEST_CASE("pause tab: Settings: voice on shows Voice mode, open mic first, push to talk the other choice") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Proximity voice chat");
    m.Press(Input::kRight);
    m.TakeRequest();
    auto names = Names(m);
    auto at = std::find(names.begin(), names.end(), "Voice mode");
    REQUIRE(at != names.end());
    CHECK(at[-1] == "Proximity voice chat");   // right under it
    const auto row = static_cast<std::size_t>(at - names.begin());
    CHECK(m.view().rows[row].value == "Open mic");
    CHECK(m.view().rows[row].kind == RowKind::kChoice);
    SelectRow(m, "Voice mode");
    m.Press(Input::kRight);
    CHECK(m.options().voice_ptt);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(m.view().rows[row].value == "Push to talk (V)");
    m.Press(Input::kLeft);
    CHECK_FALSE(m.options().voice_ptt);
    // voice off: the row goes, the choice stays for next time
    m.Press(Input::kRight);
    SelectRow(m, "Proximity voice chat");
    m.Press(Input::kLeft);
    names = Names(m);
    CHECK(std::find(names.begin(), names.end(), "Voice mode") == names.end());
    CHECK(m.options().voice_ptt);
}

TEST_CASE("pause tab: Settings: players on the map and their names, on unless turned off, each saved") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    View v = m.view();
    REQUIRE(v.rows.size() == 17);   // (the port, relay, room, network password and chat key rows before them, New Game+, Saves and Original game after)
    CHECK(v.rows[11].kind == RowKind::kToggle);
    CHECK(v.rows[11].on);
    CHECK(v.rows[12].on);
    SelectRow(m, "Show players on map");
    m.Press(Input::kLeft);
    CHECK_FALSE(m.options().map_players);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    m.Press(Input::kDecide);
    CHECK(m.options().map_players);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    SelectRow(m, "Names on map");
    m.Press(Input::kDecide);
    CHECK_FALSE(m.options().map_names);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    m.Press(Input::kRight);
    CHECK(m.options().map_names);
}

TEST_CASE("pause tab: Settings: the password on this network, not needed unless chosen, saved") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Password on this network");
    CHECK(m.view().rows[static_cast<std::size_t>(m.view().selected)].value == "Not needed");
    m.Press(Input::kDecide);
    CHECK(m.options().same_network_password);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(m.view().rows[static_cast<std::size_t>(m.view().selected)].value == "Needed");
    m.Press(Input::kLeft);   // (two choices: either way flips it)
    CHECK_FALSE(m.options().same_network_password);
    // coop-options.ini: same_network_password = on while it is needed, no line otherwise.
    CoopTitleMenu::Options o;
    o.same_network_password = true;
    std::string coop_ini, options_ini;
    o.Write(coop_ini, options_ini);
    CHECK(options_ini.find("same_network_password = on") != std::string::npos);
    CHECK(CoopTitleMenu::Options::Read(coop_ini, options_ini).same_network_password);
    o.same_network_password = false;
    o.Write(coop_ini, options_ini);
    CHECK_FALSE(CoopTitleMenu::Options::Read(coop_ini, options_ini).same_network_password);
}

TEST_CASE("pause tab: a name the server would refuse is not kept") {
    Menu m;
    CoopTitleMenu::Options o;
    o.name = "Saria";
    m.Show(o);
    m.Open(Page::kSettings);
    m.Press(Input::kDecide);
    for (int i = 0; i < 5; ++i) {
        m.Press(Input::kErase);
    }
    m.Press(Input::kDecide);   // empty
    CHECK(m.options().name == "Saria");
    CHECK(m.view().help.find("1 to 16") != std::string::npos);
    m.Press(Input::kDecide);
    for (int i = 0; i < 30; ++i) {
        m.Char('a');
    }
    m.Press(Input::kDecide);
    CHECK(m.options().name == "Sariaaaaaaaaaaaa");   // stops at 16 characters
}

TEST_CASE("pause tab: the mouse selects a row and clicks a value's halves") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    m.Select(9);
    CHECK(Selected(m) == "Proximity voice chat");
    m.Click(9, 1);   // the On half
    CHECK(m.options().voice);
    m.Click(9, -1);
    CHECK_FALSE(m.options().voice);
    m.Click(9, 0);   // the name: selected only
    CHECK_FALSE(m.options().voice);
    m.Click(0, 0);   // an action: as A
    CHECK(m.typing());
    m.Select(2);     // no selecting away while typing
    CHECK(Selected(m) == "Your name");
    m.Click(2, 0);   // a click elsewhere ends the typing first
    CHECK_FALSE(m.typing());
    m.Select(99);    // past the rows: nothing
    CHECK(Selected(m) == "Your name");
}

TEST_CASE("pause tab: the title's rows open their page, Host hosts at once") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kHost);
    CHECK(m.page() == Page::kHost);
    CHECK(m.TakeRequest()->kind == Request::Kind::kHost);
    m.SetSession(Hosting(1));
    m.Open(Page::kHost);   // hosting already: the page only
    CHECK(m.page() == Page::kHost);
    CHECK_FALSE(m.TakeRequest().has_value());
    m.Open(Page::kSettings);
    CHECK(m.page() == Page::kSettings);
    m.Open(Page::kMain);
    CHECK(m.page() == Page::kMain);
    CHECK(Selected(m) == "Host a game");
}

TEST_CASE("pause tab: hiding it ends the typing and drops a half-typed name, the page stays") {
    Menu m;
    CoopTitleMenu::Options o;
    o.name = "Saria";
    m.Show(o);
    m.Open(Page::kSettings);
    m.Press(Input::kDecide);
    m.Char('x');
    m.Hide();
    CHECK_FALSE(m.typing());
    CHECK(m.options().name == "Saria");
    CHECK_FALSE(m.Char('y'));   // no field
    m.Show(o);
    CHECK(m.page() == Page::kSettings);
}

TEST_CASE("pause tab: a session that changes keeps the same row selected") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    SelectRow(m, "Settings");
    m.SetSession(Hosting(2));
    CHECK(Selected(m) == "Settings");
    m.SetSession(Session{});
    CHECK(Selected(m) == "Settings");
}

TEST_CASE("pause tab: Settings: the chat key goes round the keys, Off among them, and is saved") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Chat key");
    CHECK(m.view().rows[static_cast<std::size_t>(m.view().selected)].value == "T");
    const auto& keys = CoopTitleMenu::ChatKeys();
    m.Press(Input::kRight);
    CHECK(m.options().chat_key == keys[1]);
    REQUIRE(m.TakeRequest().has_value());
    m.Press(Input::kLeft);
    m.Press(Input::kLeft);
    CHECK(m.options().chat_key == "off");   // (round the end: the last one)
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kSave);
    CHECK(m.view().rows[static_cast<std::size_t>(m.view().selected)].value == "Off");
    m.Press(Input::kDecide);   // A goes on, as on the other choices
    CHECK(m.options().chat_key == "T");
    // the Commands page follows
    m.Open(Page::kCommands);
    CHECK(m.view().rows[0].value == "T");
}

TEST_CASE("pause tab: Settings: Controller and Keyboard rows when this Hookshot can choose (patch 0015)") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Chat key");
    m.SetInputRows("Controller: all", "Keyboard + mouse: window in front");
    CHECK(Selected(m) == "Chat key");   // (the rows came under it: the same row stays chosen)
    CHECK(Names(m) == std::vector<std::string>{"Your name", "Link's look", "Players when you host",
                                               "Port when you host", "Relay", "Room name", "List my room", "Auto-rejoin",
                                               "Password on this network",
                                               "Proximity voice chat", "Chat key", "Controller",
                                               "Keyboard and mouse", "Show players on map", "Names on map", "Player list",
                                               "New Game+", "Saves", "Original game"});
    SelectRow(m, "Controller");
    View v = m.view();
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].value == "all");
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].kind == RowKind::kChoice);
    CHECK(v.help.find("Which controller") != std::string::npos);
    for (const Input in : {Input::kRight, Input::kLeft, Input::kDecide}) {   // each asks the module for the next one
        m.Press(in);
        const auto r = m.TakeRequest();
        REQUIRE(r.has_value());
        CHECK(r->kind == Request::Kind::kController);
    }
    m.SetInputRows("Controller: 2 - Xbox Wireless Controller", "Keyboard + mouse: window in front");
    v = m.view();
    CHECK(Selected(m) == "Controller");
    CHECK(v.rows[static_cast<std::size_t>(v.selected)].value == "2 - Xbox Wireless Controller");
    m.Press(Input::kDown);
    CHECK(Selected(m) == "Keyboard and mouse");
    CHECK(m.view().rows[static_cast<std::size_t>(m.view().selected)].value == "window in front");
    m.Press(Input::kRight);
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kKeyboard);
    // gone again (the module found no patch 0015): the selection stays on a row there is
    m.SetInputRows({}, {});
    CHECK(Names(m).size() == 17);   // (with the port, the relay and room rows, the network password, two map rows, New Game+, Saves and Original game)
    CHECK_FALSE(Selected(m).empty());
}

TEST_CASE("pause tab: Customize Link opens the customizer, its changes are asked of the module, Back comes back") {
    Menu m;
    CoopTitleMenu::Options o;
    m.Show(o);
    CoopLook::Look worn;
    worn[CoopLook::Part::boots] = 0x112233u;
    m.SetLook(worn);
    CHECK(m.Picture().empty());   // the first page has no picture
    SelectRow(m, "Customize Link");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kCustomize);
    View v = m.view();
    CHECK(v.rows[0].name == "Your name");   // (the name first, then the parts)
    CHECK(v.rows[1].kind == RowKind::kHeading);
    CHECK(v.rows[1].name == "Tunic");
    CHECK(Selected(m) == "Kokiri Tunic");
    CHECK_FALSE(m.Picture().empty());
    CHECK_FALSE(m.TakeRequest());
    m.Press(Input::kRight);   // the tunic: red
    const auto r = m.TakeRequest();
    REQUIRE(r);
    CHECK(r->kind == Request::Kind::kLookParts);
    CHECK(m.look()[CoopLook::Part::tunic] == CoopColors::Parse("red"));
    CHECK(m.look()[CoopLook::Part::boots] == 0x112233u);   // the rest as it was worn
    CHECK(m.options().colour == "red");                     // the Settings page's row agrees
    // The mouse: a click on a part's right half.
    m.Click(3, 1);   // Goron Tunic (Your name, Tunic, Kokiri Tunic above it)
    CHECK(m.look()[CoopLook::Part::goron_tunic] == CoopColors::Parse("red"));
    m.Press(Input::kBack);
    CHECK(m.page() == Page::kMain);
    CHECK(Selected(m) == "Customize Link");
    CHECK(m.Picture().empty());
}

TEST_CASE("pause tab: Settings: the Microphone row, Default and the microphones listed, saved by name") {
    Menu m;
    CoopTitleMenu::Options o;
    o.voice = true;
    m.Show(o);
    m.Open(Page::kSettings);
    auto names = Names(m);
    CHECK(std::find(names.begin(), names.end(), "Microphone") == names.end());   // no list: this game cannot choose
    m.SetMicrophones(std::vector<std::string>{"Headset Microphone (USB)", "Webcam"});
    names = Names(m);
    auto at = std::find(names.begin(), names.end(), "Microphone");
    REQUIRE(at != names.end());
    CHECK(at[-1] == "Proximity voice chat");   // right under it
    CHECK(at[1] == "Voice mode");
    const auto row = static_cast<std::size_t>(at - names.begin());
    CHECK(m.view().rows[row].value == "Default");
    CHECK(m.view().rows[row].kind == RowKind::kChoice);
    SelectRow(m, "Microphone");
    m.Press(Input::kRight);
    CHECK(m.options().voice_mic == "Headset Microphone (USB)");
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(m.view().rows[row].value == "Headset Microphone (USB)");
    m.Press(Input::kRight);
    CHECK(m.options().voice_mic == "Webcam");
    m.Press(Input::kRight);   // round to Default
    CHECK(m.options().voice_mic.empty());
    m.Press(Input::kLeft);   // and back the other way
    CHECK(m.options().voice_mic == "Webcam");
    // the chosen one unplugged: kept by its name, shown as not connected, still one of the choices
    m.SetMicrophones(std::vector<std::string>{"Headset Microphone (USB)"});
    CHECK(Selected(m) == "Microphone");
    CHECK(m.view().rows[row].value == "Webcam (not connected)");
    m.Press(Input::kRight);   // after it in the list: round to Default
    CHECK(m.options().voice_mic.empty());
    m.Press(Input::kLeft);   // once another is chosen the unplugged one is gone from the choices (as the Insert menu's)
    CHECK(m.options().voice_mic == "Headset Microphone (USB)");
    m.Press(Input::kRight);
    CHECK(m.options().voice_mic.empty());
    m.Press(Input::kRight);
    CHECK(m.options().voice_mic == "Headset Microphone (USB)");
    // voice off: no row, the choice kept
    SelectRow(m, "Proximity voice chat");
    m.Press(Input::kLeft);
    names = Names(m);
    CHECK(std::find(names.begin(), names.end(), "Microphone") == names.end());
    CHECK(m.options().voice_mic == "Headset Microphone (USB)");
}

TEST_CASE("pause tab: the Microphone row's choices and words") {
    using CoopPauseMenu::MicChoices;
    using CoopPauseMenu::MicWords;
    CHECK(MicChoices({}, "") == std::vector<std::string>{""});
    CHECK(MicChoices({"A", "B", "A", ""}, "") == std::vector<std::string>{"", "A", "B"});   // each name once
    CHECK(MicChoices({"A"}, "Gone") == std::vector<std::string>{"", "A", "Gone"});
    CHECK(MicWords("", {"A"}) == "Default");
    CHECK(MicWords("A", {"A"}) == "A");
    CHECK(MicWords("A", {}) == "A (not connected)");
    const std::string longer = "Microphone (Realtek(R) High Definition Audio)";
    CHECK(MicWords(longer, {longer}) == "Microphone (Realtek(R) Hi...");
    CHECK(CoopTitleMenu::CodePoints(MicWords(longer, {longer})) == CoopPauseMenu::kMicChars);
    const std::string wide = "Mikrofon \xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4"
                             "\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4";
    const std::string cut = MicWords(wide, {wide});   // cut at a character's start, not inside one
    CHECK(CoopTitleMenu::CodePoints(cut) == CoopPauseMenu::kMicChars);
    CHECK(cut.substr(cut.size() - 5) == "\xC3\xA4...");
}

TEST_CASE("pause tab: Join accepts manual IP address and port, with default port replacement and validation") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);

    SelectRow(m, "Room or address");
    CHECK(m.view().confirm == "Type");
    m.Press(Input::kDecide);
    CHECK(m.typing());
    for (const char c : std::string("192.168.1.50")) {
        CHECK(m.Char(static_cast<unsigned char>(c)));
    }
    // Moving Down commits the IP and steps to Port cleanly rather than jumping straight to Join.
    m.Press(Input::kDown);
    CHECK_FALSE(m.typing());
    CHECK(m.join_ip() == "192.168.1.50");
    CHECK(m.join_text() == "192.168.1.50");
    CHECK(Selected(m) == "Port");

    // Typing a digit replaces the shown default 27510 cleanly.
    m.Press(Input::kDecide);
    CHECK(m.typing());
    for (const char c : std::string("28000")) {
        CHECK(m.Char(static_cast<unsigned char>(c)));
    }
    m.Press(Input::kDecide);
    CHECK_FALSE(m.typing());
    CHECK(m.join_port() == "28000");
    CHECK(m.join_text() == "192.168.1.50:28000");
    CHECK(Selected(m) == "Join");

    m.Press(Input::kDecide);
    const auto req = m.TakeRequest();
    REQUIRE(req.has_value());
    CHECK(req->kind == Request::Kind::kJoin);
    CHECK(req->text == "192.168.1.50:28000");

    // Backspace edits the current port digit-by-digit instead of clearing on first keystroke.
    SelectRow(m, "Port");
    m.Press(Input::kDecide);
    m.Press(Input::kErase);
    m.Char('5');
    m.Press(Input::kDecide);
    CHECK(m.join_port() == "28005");
    CHECK(m.join_text() == "192.168.1.50:28005");

    // Invalid IP or out-of-range port shows a clear note and keeps the prior value.
    SelectRow(m, "Room or address");
    m.Press(Input::kDecide);
    m.Char(' ');
    m.Char('!');
    m.Press(Input::kDecide);
    CHECK(m.join_ip() == "192.168.1.50");
    CHECK(m.view().help.find("valid IP address") != std::string::npos);

    SelectRow(m, "Port");
    m.Press(Input::kDecide);
    for (const char c : std::string("70000")) {
        m.Char(static_cast<unsigned char>(c));
    }
    m.Press(Input::kDecide);
    CHECK(m.join_port() == "28005");
    CHECK(m.view().help.find("1 to 65535") != std::string::npos);

    // Pasting a Same network / VPN line mirrors into IP address and Port as well.
    m.Paste("25.1.2.3:27512 ABCDEF\n");
    CHECK(m.join_ip() == "25.1.2.3");
    CHECK(m.join_port() == "27512");
    CHECK(m.join_text() == "25.1.2.3:27512 ABCDEF");
    // Editing Port keeps any password parsed from that line.
    SelectRow(m, "Port");
    m.Press(Input::kDecide);
    for (const char c : std::string("29000")) {
        m.Char(static_cast<unsigned char>(c));
    }
    m.Press(Input::kDecide);
    CHECK(m.join_text() == "25.1.2.3:29000 ABCDEF");

    // Typing host:port in IP address splits both cleanly.
    SelectRow(m, "Room or address");
    m.Press(Input::kDecide);
    for (int i = 0; i < 8; ++i) {
        m.Press(Input::kErase);
    }
    for (const char c : std::string("10.0.0.5:27777")) {
        m.Char(static_cast<unsigned char>(c));
    }
    m.Press(Input::kDecide);
    CHECK(m.join_ip() == "10.0.0.5");
    CHECK(m.join_port() == "27777");
    CHECK(m.join_text() == "10.0.0.5:27777");
}

TEST_CASE("pause tab: Join's Password row: typed or pasted, shown as stars, sent after the address, no default") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);
    SelectRow(m, "Room or address");
    m.Press(Input::kDecide);
    for (const char c : std::string("192.168.1.50:28000")) {
        m.Char(static_cast<unsigned char>(c));
    }
    m.Press(Input::kDecide);
    CHECK(m.join_text() == "192.168.1.50:28000");   // no password is made up ("coop" is gone)
    SelectRow(m, "Password");
    CHECK(m.view().confirm == "Type");
    m.Press(Input::kDecide);
    CHECK(m.typing());
    for (const char c : std::string("Secret7")) {
        m.Char(static_cast<unsigned char>(c));
    }
    CHECK(SelectedValue(m) == "*******_");
    m.Press(Input::kDecide);
    CHECK_FALSE(m.typing());
    CHECK(Selected(m) == "Join");
    CHECK(m.join_text() == "192.168.1.50:28000 Secret7");
    SelectRow(m, "Password");
    CHECK(SelectedValue(m) == "*******");
    SelectRow(m, "Join");
    m.Press(Input::kDecide);
    const auto req = m.TakeRequest();
    REQUIRE(req.has_value());
    CHECK(req->text == "192.168.1.50:28000 Secret7");

    // a space is refused, Esc keeps the old one, a paste goes into the field
    SelectRow(m, "Password");
    m.Press(Input::kDecide);
    m.Char('a');
    m.Char(' ');
    m.Char('b');
    m.Press(Input::kDecide);
    CHECK(m.join_text() == "192.168.1.50:28000 Secret7");
    CHECK(m.view().help.find("no spaces") != std::string::npos);
    m.Press(Input::kDecide);
    m.Char('y');
    m.Press(Input::kBack);
    CHECK(m.join_text() == "192.168.1.50:28000 Secret7");
    m.Press(Input::kDecide);
    CHECK(m.Paste("Other9\n"));
    CHECK_FALSE(m.typing());
    CHECK(m.join_text() == "192.168.1.50:28000 Other9");
    CHECK(Selected(m) == "Join");

    // emptied: no password again
    SelectRow(m, "Password");
    m.Press(Input::kDecide);
    for (int i = 0; i < 6; ++i) {
        m.Press(Input::kErase);
    }
    m.Press(Input::kDecide);
    CHECK(m.join_text() == "192.168.1.50:28000");
}

TEST_CASE("pause tab: Host Port and Settings Port when you host validate, save, and restart hosting when active") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Port when you host");
    CHECK(m.view().confirm == "Type");
    m.Press(Input::kDecide);
    for (const char c : std::string("28500")) {
        CHECK(m.Char(static_cast<unsigned char>(c)));
    }
    m.Press(Input::kDecide);
    CHECK(m.options().host_port == 28500);
    REQUIRE(m.TakeRequest()->kind == Request::Kind::kSave);

    // Back cancels without saving.
    m.Press(Input::kDecide);
    m.Char('1');
    m.Press(Input::kBack);
    CHECK(m.options().host_port == 28500);
    CHECK_FALSE(m.TakeRequest().has_value());

    // Host page while stopped saves without starting host; while hosting, changing port restarts hosting.
    m.Open(Page::kHost);
    REQUIRE(m.TakeRequest()->kind == Request::Kind::kHost);
    m.SetSession(Session{});
    CHECK(Names(m) == std::vector<std::string>{"Start hosting", "Players", "Port"});
    SelectRow(m, "Port");
    m.Press(Input::kDecide);
    for (const char c : std::string("28600")) {
        m.Char(static_cast<unsigned char>(c));
    }
    m.Press(Input::kDecide);
    CHECK(m.options().host_port == 28600);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);

    m.SetSession(Hosting(1));
    SelectRow(m, "Port");
    m.Press(Input::kDecide);
    for (const char c : std::string("28700")) {
        m.Char(static_cast<unsigned char>(c));
    }
    m.Press(Input::kDecide);
    CHECK(m.options().host_port == 28700);
    CHECK(m.TakeRequest()->kind == Request::Kind::kHost);

    // Unchanged or out-of-range port does not restart hosting.
    m.Press(Input::kDecide);
    m.Press(Input::kDecide);
    CHECK_FALSE(m.TakeRequest().has_value());
    m.Press(Input::kDecide);
    m.Char('0');
    m.Press(Input::kDecide);
    CHECK(m.options().host_port == 28700);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.view().help.find("1 to 65535") != std::string::npos);
}

TEST_CASE("pause tab: q and e type into the code and the password, the page stays (Linux report)") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);
    SelectRow(m, "Room or address");
    m.Press(Input::kDecide);
    CHECK(m.typing());
    for (const char c : std::string("queue")) {
        CHECK(m.Char(static_cast<unsigned char>(c)));
    }
    CHECK(m.typing());
    CHECK(m.page() == Page::kJoin);
    m.Press(Input::kDecide);
    CHECK_FALSE(m.typing());
    // (a room name: through the built-in relay)
    CHECK(m.join_text() == std::string("queue@") + CoopTitleMenu::kDefaultRelay);
    // the Password row: the same
    Menu p;
    p.Show(CoopTitleMenu::Options{});
    p.Open(Page::kJoin);
    SelectRow(p, "Room or address");
    p.Press(Input::kDecide);
    for (const char c : std::string("10.0.0.2:28000")) {
        p.Char(static_cast<unsigned char>(c));
    }
    p.Press(Input::kDecide);
    SelectRow(p, "Password");
    p.Press(Input::kDecide);
    for (const char c : std::string("qEeQue")) {
        CHECK(p.Char(static_cast<unsigned char>(c)));
    }
    CHECK(p.typing());
    CHECK(p.page() == Page::kJoin);
    CHECK(SelectedValue(p) == "******_");
    p.Press(Input::kDecide);
    CHECK(p.join_text() == "10.0.0.2:28000 qEeQue");
}

// Owner photo 2026-11-19: the Multiplayer tab's value column was empty. Every page row says where it stands and
// carries the page arrow (right), so the cell is never blank.
TEST_CASE("pause tab: every first-page row has a value and the page arrow") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    const View v = m.view();
    REQUIRE(v.rows.size() == 6);
    for (const Row& r : v.rows) {
        CHECK_FALSE(r.value.empty());
        CHECK(r.right);
    }
    CHECK(v.rows[0].value == "Off");
    CHECK(v.rows[1].value == "Off");
}

TEST_CASE("pause tab: a long name is cut on the Settings row so the page arrow keeps its room") {
    Menu m;
    CoopTitleMenu::Options o;
    o.name = "AVeryLongPlayerName";
    m.Show(o);
    CHECK(m.view().rows[3].value == "AVeryLong...");
}

TEST_CASE("pause tab: the Settings row's name is cut on a character, not in the middle of one") {
    Menu m;
    CoopTitleMenu::Options o;
    o.name = "Ab\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";   // 2 + 8 two-byte letters: byte 9 is mid-letter
    m.Show(o);
    const std::string v = m.view().rows[3].value;
    CHECK(v == "Ab\xC3\xA9\xC3\xA9\xC3\xA9...");   // (byte 9 is inside the fourth letter: the cut steps back to its start)
}

TEST_CASE("pause tab: Settings: the relay and the room name, typed, checked and saved") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Relay");
    m.Press(Input::kDecide);
    CHECK(m.typing());
    for (const char c : std::string("relay.example net:27530")) m.Char(static_cast<unsigned char>(c));   // (no space)
    m.Press(Input::kDecide);
    CHECK(m.options().relay == "relay.examplenet:27530");
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    SelectRow(m, "Room name");
    m.Press(Input::kDecide);
    for (const char c : std::string("forest gang!")) m.Char(static_cast<unsigned char>(c));   // (letters, digits, - _)
    m.Press(Input::kDecide);
    CHECK(m.options().relay_room == "forestgang");
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    m.Press(Input::kDecide);   // (too short: not kept)
    m.Press(Input::kErase);
    for (int i = 0; i < 9; ++i) m.Press(Input::kErase);
    m.Char('a');
    m.Press(Input::kDecide);
    CHECK(m.options().relay_room == "forestgang");
    CHECK_FALSE(m.TakeRequest().has_value());
}

TEST_CASE("pause tab: Customize Link: Your name first, typed on its row, saved, Up and Down round it") {
    Menu m;
    CoopTitleMenu::Options o;
    o.name = "Link";
    m.Show(o);
    m.Open(Page::kCustomize);
    REQUIRE(m.page() == Page::kCustomize);
    CHECK(Selected(m) == "Kokiri Tunic");
    // Up from the first part: Your name; Up again: round to the last part; Down from there: the name again.
    m.Press(Input::kUp);
    CHECK(Selected(m) == "Your name");
    CHECK(m.view().rows[0].value == "Link");
    CHECK(m.view().help_title == "Your name");
    m.Press(Input::kDown);
    CHECK(Selected(m) == "Kokiri Tunic");
    m.Press(Input::kUp);
    m.Press(Input::kUp);
    CHECK(Selected(m) != "Your name");
    CHECK(Selected(m) != "Kokiri Tunic");
    m.Press(Input::kDown);   // (the last part's Down: round to the list's top, its own way)
    m.Press(Input::kUp);
    // A on Your name: typing; Erase and characters; Enter keeps it and asks for a save.
    while (Selected(m) != "Your name") m.Press(Input::kUp);
    m.Press(Input::kDecide);
    CHECK(m.typing());
    CHECK(m.view().typing);
    CHECK(m.view().selected == 0);
    m.Press(Input::kErase);
    m.Char('k');
    m.Char('2');
    CHECK(m.view().rows[0].value == "Link2");
    m.Press(Input::kDecide);
    CHECK_FALSE(m.typing());
    CHECK(m.options().name == "Link2");
    const auto saved = m.TakeRequest();
    REQUIRE(saved);
    CHECK(saved->kind == Request::Kind::kSave);
    // Esc (Back) while typing: the name as it was, no save.
    m.Press(Input::kDecide);
    m.Char('x');
    m.Press(Input::kBack);
    CHECK_FALSE(m.typing());
    CHECK(m.options().name == "Link2");
    CHECK_FALSE(m.TakeRequest());
    CHECK(m.page() == Page::kCustomize);   // (Back ended the typing only)
    // An empty name is not taken.
    m.Press(Input::kDecide);
    for (int i = 0; i < 10; ++i) m.Press(Input::kErase);
    m.Press(Input::kDecide);
    CHECK(m.options().name == "Link2");
    // The mouse: a click on row 0 types the name; a click on a part ends it and picks that part.
    m.Click(0, 0);
    CHECK(m.typing());
    m.Click(2, 0);
    CHECK_FALSE(m.typing());
    m.Select(2);
    CHECK(Selected(m) == "Kokiri Tunic");
    m.Select(0);
    CHECK(Selected(m) == "Your name");
    // In a colour's mixer there is no name row; Back from the list leaves the page.
    m.Select(2);
    m.Press(Input::kDecide);   // (A on a part: its mixer)
    CHECK(m.view().rows[0].name != "Your name");
    m.Press(Input::kBack);     // (the mixer shut)
    CHECK(m.view().rows[0].name == "Your name");
    m.Press(Input::kBack);
    CHECK(m.page() == Page::kMain);
}

namespace {

// A row the page draws: its name at most 30 characters, its value at most 16 (bytes: the words here are ASCII).
bool Fits(const Row& r) {
    return r.name.size() <= 30 && r.value.size() <= 16;
}

const Row* RowNamed(const View& v, const std::string& name) {
    for (const Row& r : v.rows) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("pause tab: Settings: List my room and Auto-rejoin, under Room name, off unless turned on, each saved") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    const auto names = Names(m);
    const auto room = std::find(names.begin(), names.end(), "Room name");
    REQUIRE(room != names.end());
    REQUIRE(room + 2 < names.end());
    CHECK(room[1] == "List my room");   // (next to the room it lists)
    CHECK(room[2] == "Auto-rejoin");
    const View v = m.view();
    for (const char* name : {"List my room", "Auto-rejoin"}) {
        const Row* r = RowNamed(v, name);
        REQUIRE(r != nullptr);
        CHECK(r->kind == RowKind::kToggle);
        CHECK_FALSE(r->on);
        CHECK(Fits(*r));
    }
    SelectRow(m, "List my room");
    CHECK(m.view().help.find("public list") != std::string::npos);
    m.Press(Input::kDecide);   // A flips it
    CHECK(m.options().list_room);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(RowNamed(m.view(), "List my room")->on);
    m.Press(Input::kLeft);   // Left off, Right on
    CHECK_FALSE(m.options().list_room);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    m.Press(Input::kRight);
    CHECK(m.options().list_room);
    SelectRow(m, "Auto-rejoin");
    CHECK(m.view().help.find("by itself") != std::string::npos);
    m.Press(Input::kDecide);
    CHECK(m.options().auto_rejoin);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    m.Press(Input::kLeft);
    CHECK_FALSE(m.options().auto_rejoin);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    // As the files have them; written back as coop-options.ini's lines.
    CoopTitleMenu::Options o;
    o.list_room = true;
    o.auto_rejoin = true;
    Menu n;
    n.Show(o);
    n.Open(Page::kSettings);
    CHECK(RowNamed(n.view(), "List my room")->on);
    CHECK(RowNamed(n.view(), "Auto-rejoin")->on);
    std::string coop_ini, options_ini;
    n.options().Write(coop_ini, options_ini);
    CHECK(options_ini.find("list_room = on") != std::string::npos);
    CHECK(options_ini.find("auto_rejoin = on") != std::string::npos);
}

TEST_CASE("pause tab: Host: the join password is hidden until A, shown in groups of five, A again hides it") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kHost);
    REQUIRE(m.TakeRequest()->kind == Request::Kind::kHost);
    Session s = Hosting(1);
    m.SetSession(s);   // (no password: no row)
    CHECK(Names(m) == std::vector<std::string>{"Copy the code again", "Paste friend's answer (or Ctrl+V)",
                                               "Same network", "VPN", "Port", "Stop hosting"});
    s.password = "abcdefghijklmnopqrst";
    m.SetSession(s);
    CHECK(Names(m) == std::vector<std::string>{"Copy the code again", "Paste friend's answer (or Ctrl+V)",
                                               "Same network", "VPN", "Join password", "Change join password",
                                               "Port", "Stop hosting"});
    SelectRow(m, "Join password");
    CHECK(SelectedValue(m) == "Hidden");
    CHECK(m.view().confirm == "Choose");
    CHECK(Fits(*RowNamed(m.view(), "Join password")));
    CHECK(Fits(*RowNamed(m.view(), "Change join password")));
    m.Press(Input::kDecide);   // shown: ten a line, in fives
    CHECK_FALSE(m.TakeRequest().has_value());
    View v = m.view();
    CHECK(v.rows[4].name == "Join password");
    CHECK(v.rows[4].value == "abcde fghij");
    CHECK(v.rows[5].name.empty());
    CHECK(v.rows[5].value == "klmno pqrst");
    CHECK(v.rows[6].name == "Change join password");
    CHECK(Selected(m) == "Join password");
    m.Press(Input::kDown);   // A on its second line hides it too, back on the first
    CHECK(SelectedValue(m) == "klmno pqrst");
    m.Press(Input::kDecide);
    CHECK(Selected(m) == "Join password");
    CHECK(SelectedValue(m) == "Hidden");
    // A long one: three lines, the third cut
    s.password = std::string(40, 'x');
    m.SetSession(s);
    m.Press(Input::kDecide);
    v = m.view();
    CHECK(v.rows[4].value == "xxxxx xxxxx");
    CHECK(v.rows[6].value == "xxxxx xxxxx...");
    CHECK(v.rows[7].name == "Change join password");
    for (std::size_t i = 4; i <= 7; ++i) CHECK(Fits(v.rows[i]));
    // Another page, or the menu closed: hidden again
    m.Press(Input::kBack);
    m.Open(Page::kHost);
    CHECK(RowNamed(m.view(), "Join password")->value == "Hidden");
    SelectRow(m, "Join password");
    m.Press(Input::kDecide);
    m.Hide();
    m.Show(CoopTitleMenu::Options{});
    CHECK(RowNamed(m.view(), "Join password")->value == "Hidden");
    // Not while the server starts, nor for a guest
    Session starting;
    starting.state = State::kStarting;
    starting.hosting = true;
    starting.password = "abcde";
    m.SetSession(starting);
    CHECK(RowNamed(m.view(), "Join password") == nullptr);
    CHECK(RowNamed(m.view(), "Change join password") == nullptr);
    Session guest = Joined();
    guest.password = "abcde";
    m.SetSession(guest);
    m.Open(Page::kJoin);
    CHECK(RowNamed(m.view(), "Join password") == nullptr);
}

TEST_CASE("pause tab: Host: Change join password is typed as stars and handed to the host on Enter only") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kHost);
    m.TakeRequest();
    Session s = Hosting(1);
    s.password = "abcdefghijklmnopqrst";
    m.SetSession(s);
    SelectRow(m, "Change join password");
    CHECK(SelectedValue(m) == "Type");
    CHECK(m.view().confirm == "Type");
    m.Press(Input::kDecide);
    REQUIRE(m.typing());
    CHECK(m.view().confirm == "Done");
    for (const char c : std::string("new pass")) m.Char(static_cast<unsigned char>(c));   // (the space is not typed)
    CHECK(SelectedValue(m) == "*******_");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kSetPassword);
    CHECK(r->text == "newpass");
    CHECK_FALSE(m.typing());
    CHECK(SelectedValue(m) == "Type");   // (not kept in the menu)
    s.password = "newpass";   // (the host took it: shown, so the host can give it out)
    m.SetSession(s);
    CHECK(RowNamed(m.view(), "Join password")->value == "newpa ss");
    // Esc, Up or a click elsewhere: dropped, nothing handed
    SelectRow(m, "Change join password");
    m.Press(Input::kDecide);
    m.Char('x');
    m.Press(Input::kBack);
    CHECK_FALSE(m.typing());
    CHECK_FALSE(m.TakeRequest().has_value());
    m.Press(Input::kDecide);
    m.Char('y');
    m.Press(Input::kUp);
    CHECK_FALSE(m.typing());
    CHECK_FALSE(m.TakeRequest().has_value());
    SelectRow(m, "Change join password");
    m.Press(Input::kDecide);
    m.Char('z');
    m.Click(0, 0);
    CHECK_FALSE(m.typing());
    CHECK_FALSE(m.TakeRequest().has_value());
    SelectRow(m, "Change join password");
    CHECK(SelectedValue(m) == "Type");   // (the half-typed one is gone)
    // Enter on nothing: the note says so
    m.Press(Input::kDecide);
    m.Press(Input::kDecide);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.view().help.find("type the new one first") != std::string::npos);
    // Pasted: in place of what was typed, handed on Enter; one with a space is refused
    m.Press(Input::kDecide);
    m.Char('q');
    CHECK(m.Paste("pasted-pw\n"));
    CHECK(m.typing());
    m.Press(Input::kDecide);
    r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->text == "pasted-pw");
    m.Press(Input::kDecide);
    m.Paste("two words");
    m.Press(Input::kDecide);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.view().help.find("no spaces") != std::string::npos);
    // Hosting stops while it is typed: dropped
    m.Press(Input::kDecide);
    m.Char('w');
    m.SetSession(Session{});
    CHECK_FALSE(m.typing());
    CHECK_FALSE(m.TakeRequest().has_value());
}

TEST_CASE("pause tab: Host: the player asking to join is let in or refused from the page, the oldest first") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kHost);
    m.TakeRequest();
    Session s = Hosting(1);
    m.SetSession(s);
    CHECK(Selected(m) == "Copy the code again");
    s.asking = {"Saria"};
    m.SetSession(s);
    CHECK(Names(m) == std::vector<std::string>{"Let in Saria", "Refuse Saria", "Copy the code again",
                                               "Paste friend's answer (or Ctrl+V)", "Same network", "VPN", "Port",
                                               "Stop hosting"});
    CHECK(Selected(m) == "Copy the code again");   // (the rows came above it: the same row stays chosen)
    CHECK(Fits(m.view().rows[0]));
    CHECK(Fits(m.view().rows[1]));
    SelectRow(m, "Let in Saria");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kAllow);
    CHECK(r->text == "Saria");
    SelectRow(m, "Refuse Saria");
    m.Press(Input::kDecide);
    r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kDeny);
    CHECK(r->text == "Saria");
    // More than one: the oldest, and how many more
    s.asking = {"Saria", "Mido", "Darunia"};
    m.SetSession(s);
    const View v = m.view();
    CHECK(v.rows[0].name == "Let in Saria");
    CHECK(v.rows[1].name == "Refuse Saria");
    CHECK(v.rows[2].name == "+2 more ask");
    CHECK(v.rows[2].kind == RowKind::kNote);
    // Nobody asks any more: the rows go
    s.asking.clear();
    m.SetSession(s);
    CHECK(Names(m).front() == "Copy the code again");
    // Not while the server starts
    Session starting;
    starting.state = State::kStarting;
    starting.hosting = true;
    starting.asking = {"Saria"};
    m.SetSession(starting);
    CHECK(RowNamed(m.view(), "Let in Saria") == nullptr);
}

TEST_CASE("pause tab: Browse games: Refresh asks for the list again and empties it meanwhile") {
    Menu m;
    CoopTitleMenu::Options o;
    o.relay = "relay.example.net:27530";
    m.Show(o);
    m.Open(Page::kFind);
    REQUIRE(m.page() == Page::kFind);
    m.SetFound({{"kokiri (Saria)", "1/4", "kokiri", 0, true, "Saria", 1, 4, true}}, "");
    CHECK(Names(m) == std::vector<std::string>{"List", "Search", "Show", "Sort", "Refresh", "kokiri (Saria)"});
    SelectRow(m, "Refresh");
    CHECK(SelectedValue(m) == "Ask again");
    CHECK(m.view().confirm == "Choose");
    CHECK(m.view().help.find("relay") != std::string::npos);
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kRefreshFound);
    CHECK(Names(m) == std::vector<std::string>{"List", "Search", "Show", "Sort", "Refresh", "Asking the relay..."});
    CHECK(Selected(m) == "Refresh");
    m.SetFound({{"kokiri (Saria)", "1/4", "kokiri", 0, true, "Saria", 1, 4, true}}, "");
    CHECK(Names(m).back() == "kokiri (Saria)");
    // This network's list: its own words
    SelectRow(m, "List");
    m.Press(Input::kRight);
    SelectRow(m, "Refresh");
    CHECK(m.view().help.find("listens again") != std::string::npos);
    m.Press(Input::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kRefreshFound);
    CHECK(Names(m).back() == "Looking for games...");
}

TEST_CASE("pause tab: Settings: Original game, A flips it and asks for a save") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Original game");
    CHECK_FALSE(m.options().original_game);
    m.Press(Input::kDecide);
    CHECK(m.options().original_game);
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kSave);
}
