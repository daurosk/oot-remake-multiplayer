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
                                               "Same network", "VPN", "Port", "Game", "Stop hosting"});
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
    REQUIRE(Selected(m) == "Game");
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

TEST_CASE("pause tab: Host page: Private game is the default, Public game offers a ready room name, is remembered, "
          "and while hosting public no row holds an address") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kHost);
    REQUIRE(m.TakeRequest()->kind == Request::Kind::kHost);
    m.SetSession(Session{});   // (stopped: the Start row and the choices)
    SelectRow(m, "Game");
    CHECK(m.view().help.find("Private game") != std::string::npos);
    m.Press(Input::kDecide);
    CHECK(m.options().public_game);
    CHECK_FALSE(m.options().relay_room.empty());   // a ready name, as forest-4821
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(Names(m) == std::vector<std::string>{"Start hosting", "Game", "Room name", "Players", "Port"});
    CHECK(m.view().help.find("no password") != std::string::npos);
    Session s = Hosting(1, "");
    s.public_game = true;
    m.SetSession(s);
    CHECK(Names(m) == std::vector<std::string>{"Port", "Game", "Stop hosting"});
    // Switched to private while hosting public: it says plainly it applies at the next Host.
    SelectRow(m, "Game");
    m.Press(Input::kDecide);
    CHECK_FALSE(m.options().public_game);
    CHECK(m.view().help.find("next Host") != std::string::npos);
}

TEST_CASE("pause tab: Join: a room name or an address, its port and the password, then Join (no friend's code)") {
    Menu m;
    CoopTitleMenu::Options o;
    o.relay = "relay.example.net:27530";
    m.Show(o);
    SelectRow(m, "Join a game");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kJoin);
    CHECK(Names(m) == std::vector<std::string>{"Room or address", "Port", "Password", "Join"});
    CHECK(m.view().rows[0].value == "Type");
    CHECK(m.view().rows[1].value == "27510");
    CHECK(m.view().rows[3].fixed);   // nothing to join yet
    const auto type = [&](const std::string& row, const std::string& text) {
        SelectRow(m, row);
        m.Press(Input::kDecide);
        REQUIRE(m.typing());
        for (const char c : text) m.Char(static_cast<unsigned char>(c));
        m.Press(Input::kDecide);
    };
    // A bare name (a room, or a PC on the home network): as typed; Join tries it as a PC first, then as a room
    // (CoopPause, CoopTitleMenu::BareJoinName)
    type("Room or address", "forest-gang");
    type("Password", "kokiri");
    CHECK(m.join_text() == "forest-gang kokiri");
    CHECK(m.view().rows[2].value == "******");
    SelectRow(m, "Join");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "forest-gang kokiri");
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

TEST_CASE("pause tab: Join: A on Room or address pastes an invite code or an address line (a pad has no Ctrl+V)") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);
    SelectRow(m, "Room or address");
    m.Press(Input::kDecide);
    CHECK(m.typing());
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kPaste);   // (the module reads the clipboard and hands it to Paste)
    m.Paste("ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567\n");
    CHECK_FALSE(m.typing());
    CHECK(m.join_text() == "ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567");
    CHECK(Selected(m) == "Join");
    CHECK(m.view().rows[0].value == "ABCDE-FG...Z2-34567 (35)");   // (the code shown on its row)
    CHECK_FALSE(m.view().rows[3].fixed);
    // An address line: its address, port and password
    SelectRow(m, "Room or address");
    m.Press(Input::kDecide);
    m.Paste("25.1.2.3:27512 ABCDEF");
    CHECK_FALSE(m.typing());
    CHECK(m.join_text() == "25.1.2.3:27512 ABCDEF");
    // Any other text on the clipboard is left alone: the field stays open to be typed, nothing changes
    Menu t;
    t.Show(CoopTitleMenu::Options{});
    t.Open(Page::kJoin);
    SelectRow(t, "Room or address");
    t.Press(Input::kDecide);
    t.Paste("see you at eight, bring snacks");
    CHECK(t.typing());
    CHECK(t.join_ip().empty());
    t.Paste("");   // (an empty clipboard: the same)
    CHECK(t.typing());
    for (const char c : std::string("mypc")) t.Char(static_cast<unsigned char>(c));
    t.Press(Input::kDecide);
    CHECK(t.join_text() == "mypc");
    // A key typed before the clipboard came: the late paste is not used
    Menu k;
    k.Show(CoopTitleMenu::Options{});
    k.Open(Page::kJoin);
    SelectRow(k, "Room or address");
    k.Press(Input::kDecide);
    k.Char('1');
    k.Paste("ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567");   // (the clipboard asked on open, come late)
    CHECK(k.typing());
    k.Press(Input::kDecide);
    CHECK(k.join_text() == "1");
    SelectRow(k, "Room or address");   // (a Ctrl+V while typing: the player's own paste, used)
    k.Press(Input::kDecide);
    k.Char('2');
    k.Press(Input::kPaste);
    REQUIRE(k.TakeRequest()->kind == Request::Kind::kPaste);
    k.Paste("25.1.2.3");
    CHECK(k.join_ip() == "25.1.2.3");
    // B before the clipboard came (Linux's late one): it is dropped, not a page paste; the next Ctrl+V is used.
    Menu b;
    b.Show(CoopTitleMenu::Options{});
    b.Open(Page::kJoin);
    SelectRow(b, "Room or address");
    b.Press(Input::kDecide);
    b.Press(Input::kBack);
    CHECK_FALSE(b.typing());
    b.Paste("ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567");
    CHECK(b.join_text().empty());
    b.Press(Input::kPaste);
    b.Paste("ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567");
    CHECK(b.join_text() == "ABCDE-FGHJK-LMNPQ-RSTUV-WXYZ2-34567");
}

TEST_CASE("pause tab: Join offers the last game again first, one press") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);
    m.SetRejoin("Saria", "ABCDE-FGHJK");
    CHECK(Names(m) == std::vector<std::string>{"Join Saria again", "Room or address", "Port", "Password", "Join"});
    CHECK(Selected(m) == "Join Saria again");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "ABCDE-FGHJK");
    m.SetRejoin("", "ABCDE-FGHJK");
    CHECK(m.view().rows[0].name == "Join your friend again");
    m.SetRejoin("Saria", "");
    CHECK(m.view().rows[0].name == "Room or address");
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
    CHECK(Names(m) == std::vector<std::string>{"Leave the game"});
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
                                               "Port when you host", "Relay", "Room name", "Password on this network",
                                               "Proximity voice chat", "Chat key", "Show players on map", "Names on map", "New Game+",
                                               "Saves"});   // (no patch 0015: no input rows; Navi's two last)
    View v = m.view();
    CHECK(v.rows[0].value == "Saria");
    CHECK(v.rows[1].value == "By player number");
    CHECK(v.rows[3].value == "27510");
    CHECK(v.rows[4].value == "None");   // (the relay: the kit's default, none yet)
    CHECK(v.rows[5].value == "None");   // (no room name)
    CHECK(v.rows[6].value == "Needed");   // (the password on this network: needed unless chosen)
    CHECK(v.rows[7].kind == RowKind::kToggle);
    CHECK_FALSE(v.rows[7].on);   // the default
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
    REQUIRE(v.rows.size() == 13);   // (the port, relay, same-network and chat key rows before them, New Game+ and Saves after)
    CHECK(v.rows[9].kind == RowKind::kToggle);
    CHECK(v.rows[9].on);
    CHECK(v.rows[10].on);
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

TEST_CASE("pause tab: Settings: the password on this network, needed unless chosen, saved") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kSettings);
    SelectRow(m, "Password on this network");
    CHECK(m.view().rows[static_cast<std::size_t>(m.view().selected)].value == "Needed");   // (the default)
    m.Press(Input::kDecide);
    CHECK(m.options().same_network_open);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(m.view().rows[static_cast<std::size_t>(m.view().selected)].value == "Not needed");
    m.Press(Input::kLeft);   // (two choices: either way flips it)
    CHECK_FALSE(m.options().same_network_open);
    // coop-options.ini: same_network_open = on while it is not needed, no line otherwise; a file without the line
    // (every older one) and an older same_network_password line keep it needed.
    CHECK_FALSE(CoopTitleMenu::Options::Read("", "").same_network_open);
    CHECK_FALSE(CoopTitleMenu::Options::Read("", "same_network_password = on\n").same_network_open);
    CoopTitleMenu::Options o;
    o.same_network_open = true;
    std::string coop_ini, options_ini;
    o.Write(coop_ini, options_ini);
    CHECK(options_ini.find("same_network_open = on") != std::string::npos);
    CHECK(CoopTitleMenu::Options::Read(coop_ini, options_ini).same_network_open);
    o.same_network_open = false;
    o.Write(coop_ini, options_ini);
    CHECK(options_ini.find("# same_network_open = on") != std::string::npos);   // (commented out, as IniUnset does)
    CHECK_FALSE(CoopTitleMenu::Options::Read(coop_ini, options_ini).same_network_open);
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
    m.Select(7);
    CHECK(Selected(m) == "Proximity voice chat");
    m.Click(7, 1);   // the On half
    CHECK(m.options().voice);
    m.Click(7, -1);
    CHECK_FALSE(m.options().voice);
    m.Click(7, 0);   // the name: selected only
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
                                               "Port when you host", "Relay", "Room name", "Password on this network",
                                               "Proximity voice chat", "Chat key", "Controller",
                                               "Keyboard and mouse", "Show players on map", "Names on map",
                                               "New Game+", "Saves"});
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
    CHECK(Names(m).size() == 13);   // (with the port, the relay rows, the password on this network, two map rows, New Game+ and Saves)
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
    CHECK(v.rows[0].kind == RowKind::kHeading);
    CHECK(v.rows[0].name == "Tunic");
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
    m.Click(2, 1);   // Goron Tunic
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
    CHECK(Names(m) == std::vector<std::string>{"Start hosting", "Game", "Players", "Port"});
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
    CHECK(m.join_text() == "queue");
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
    m.Press(Input::kDecide);   // (a bad port: not kept, and a note says why)
    for (int i = 0; i < 5; ++i) m.Press(Input::kErase);
    for (const char c : std::string("99999")) m.Char(static_cast<unsigned char>(c));
    m.Press(Input::kDecide);
    CHECK(m.options().relay == "relay.examplenet:27530");
    CHECK_FALSE(m.TakeRequest().has_value());
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
