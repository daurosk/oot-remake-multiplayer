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
    s.lines = {"Send this code to your friends:", s.code};
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
    CHECK(Names(m) == std::vector<std::string>{"Copy the code again", "Paste friend's answer", "Same network or VPN",
                                               "Stop hosting"});
    View v = m.view();
    CHECK(v.hint == "ABCDE-FGHJK");
    CHECK(v.gold == "Hosting: waiting for friends");
    CHECK(v.help.find("Send this code") != std::string::npos);   // what the host does, in place of the row's help
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
    // Stop takes a second press
    m.Press(Input::kDown);
    REQUIRE(Selected(m) == "Stop hosting");
    m.Press(Input::kDecide);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.view().rows[3].value == "Press again to confirm");
    m.Press(Input::kUp);   // anything else: not confirmed
    m.Press(Input::kDown);
    CHECK(m.view().rows[3].value == "Stop");
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
    m.Press(Input::kDecide);   // a fixed row says why, and does nothing
    CHECK(m.page() == Page::kMain);
    CHECK(m.view().help.find("stop yours first") != std::string::npos);
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

TEST_CASE("pause tab: Join pastes the code in one press, then joins with it") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    SelectRow(m, "Join a game");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kJoin);
    CHECK(Names(m) == std::vector<std::string>{"Friend's code", "Join"});
    CHECK(m.view().rows[0].value == "Paste");
    CHECK(m.view().rows[1].fixed);   // nothing to join yet
    m.Press(Input::kDecide);
    CHECK(m.typing());
    CHECK(m.view().typing);
    CHECK(m.TakeRequest()->kind == Request::Kind::kPaste);   // the module answers with the clipboard
    CHECK(m.Paste("  ABCDE-FGHJK\r\n"));
    CHECK_FALSE(m.typing());
    CHECK(m.join_text() == "ABCDE-FGHJK");
    CHECK(Selected(m) == "Join");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "ABCDE-FGHJK");
    // nothing on the clipboard: said, and the field can still be typed into
    Menu e;
    e.Show(CoopTitleMenu::Options{});
    e.Open(Page::kJoin);
    e.Press(Input::kDecide);
    e.TakeRequest();
    CHECK(e.Paste(""));
    CHECK(e.view().help.find("Nothing to paste") != std::string::npos);
    CHECK(e.Char('1'));
    CHECK(e.Char('2'));
    e.Press(Input::kErase);
    CHECK(e.join_text() == "1");
    CHECK(e.view().rows[0].value == "1_");
    e.Press(Input::kDecide);   // done: on to Join
    CHECK(Selected(e) == "Join");
}

TEST_CASE("pause tab: Join offers the last game again first, one press") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kJoin);
    m.SetRejoin("Saria", "ABCDE-FGHJK");
    CHECK(Names(m) == std::vector<std::string>{"Join Saria again", "Or a new code", "Join"});
    CHECK(Selected(m) == "Join Saria again");
    m.Press(Input::kDecide);
    auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "ABCDE-FGHJK");
    m.SetRejoin("", "ABCDE-FGHJK");
    CHECK(m.view().rows[0].name == "Join your friend again");
    m.SetRejoin("Saria", "");
    CHECK(m.view().rows[0].name == "Friend's code");
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
    m.SetSession(s);
    CHECK(m.view().rows[0].fixed);
    CHECK(m.view().rows[1].fixed);
    CHECK(m.view().gold == "Co-op is on");
    s.players = 3;
    m.SetSession(s);
    CHECK(m.view().gold == "Co-op is on: 3 players");
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kMain);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.view().help.find("Host.bat") != std::string::npos);
    SelectRow(m, "Settings");   // the settings still open
    m.Press(Input::kDecide);
    CHECK(m.page() == Page::kSettings);
}

TEST_CASE("pause tab: Local co-op says split screen comes in the next test release") {
    Menu m;
    m.Show(CoopTitleMenu::Options{});
    m.Open(Page::kLocal);
    CHECK(m.page() == Page::kLocal);
    const View v = m.view();
    REQUIRE(v.rows.size() == 2);
    CHECK(v.rows[0].kind == RowKind::kNote);
    CHECK(v.rows[1].name.find("next test release") != std::string::npos);
    CHECK(v.selected == -1);   // nothing to choose
    CHECK(v.help.find("two PCs") != std::string::npos);   // the help box says what to do meanwhile
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
                                               "Proximity voice chat", "Chat key"});   // (no patch 0015: no input rows)
    View v = m.view();
    CHECK(v.rows[0].value == "Saria");
    CHECK(v.rows[1].value == "By player number");
    CHECK(v.rows[3].kind == RowKind::kToggle);
    CHECK_FALSE(v.rows[3].on);   // the default
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
    m.Select(3);
    CHECK(Selected(m) == "Proximity voice chat");
    m.Click(3, 1);   // the On half
    CHECK(m.options().voice);
    m.Click(3, -1);
    CHECK_FALSE(m.options().voice);
    m.Click(3, 0);   // the name: selected only
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
                                               "Proximity voice chat", "Chat key", "Controller",
                                               "Keyboard and mouse"});
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
    CHECK(Names(m).size() == 5);
    CHECK_FALSE(Selected(m).empty());
}

TEST_CASE("pause tab: Customize Link opens the customizer; its changes are asked of the module; Back comes back") {
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
