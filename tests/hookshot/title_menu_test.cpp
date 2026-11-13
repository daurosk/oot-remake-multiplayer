// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer menu on Hookshot's title screen (hookshot/src/coop_title_menu.h).
#include <doctest/doctest.h>

#include <string>
#include <variant>

#include "coop_title_menu.h"
#include "hos_coop_host.h"

using namespace CoopTitleMenu;

TEST_CASE("title menu: ini lines are read past comments, the last one wins") {
    const std::string t = "# color = red\r\nname = Saria\r\ncolor = blue  # mine\r\n; pvp = on\r\ncolor=green\r\n";
    CHECK(IniGet(t, "name") == "Saria");
    CHECK(IniGet(t, "color") == "green");
    CHECK_FALSE(IniGet(t, "pvp").has_value());
    CHECK_FALSE(IniGet(t, "nam").has_value());
    CHECK_FALSE(IniGet("names = x\n", "name").has_value());
    CHECK(IniGet("server = 1.2.3.4:27510\n", "server") == "1.2.3.4:27510");
}

TEST_CASE("title menu: setting a key replaces its line, else uncomments it, else appends") {
    CHECK(IniSet("name = A\r\nport = 1\r\n", "name", "Saria", true) == "name = Saria\r\nport = 1\r\n");
    CHECK(IniSet("# Pick one:\n# color = blue\n", "color", "red", true) == "# Pick one:\ncolor = red\n");
    CHECK(IniSet("pvp = off\n", "LICHII_COOP_HUD", "0", false) == "pvp = off\nLICHII_COOP_HUD=0\n");
    CHECK(IniSet("", "name", "Saria", true) == "name = Saria\r\n");
    // a second active line is commented out, so the file says one thing
    CHECK(IniSet("a = 1\na = 2\n", "a", "3", true) == "a = 3\n# a = 2\n");
    // a longer key with the same start is another key
    CHECK(IniSet("LICHII_COOP_HUD_DIR=x\n", "LICHII_COOP_HUD", "0", false) == "LICHII_COOP_HUD_DIR=x\nLICHII_COOP_HUD=0\n");
}

TEST_CASE("title menu: unsetting comments the line out and leaves the rest") {
    CHECK(IniUnset("upnp = no\r\nx = 1\r\n", "upnp") == "# upnp = no\r\nx = 1\r\n");
    CHECK(IniUnset("x = 1\n", "upnp") == "x = 1\n");
}

TEST_CASE("title menu: names and join passwords as the server takes them") {
    CHECK(NameOk("Saria"));
    CHECK(NameOk("  Saria  "));
    CHECK(NameOk("abcdefghijklmnop"));
    CHECK_FALSE(NameOk("abcdefghijklmnopq"));
    CHECK_FALSE(NameOk(""));
    CHECK_FALSE(NameOk("   "));
    CHECK_FALSE(NameOk("a\tb"));
    CHECK(NameOk("\xC3\xA9\xC3\xA9"));   // two characters, four bytes
    CHECK(PasswordOk("ABCDEFGHJK"));
    CHECK_FALSE(PasswordOk("has space"));
    CHECK_FALSE(PasswordOk(""));
    CHECK_FALSE(PasswordOk(std::string(129, 'a')));
}

TEST_CASE("title menu: settings read from and written to the two files") {
    const std::string coop = "server = 1.2.3.4\r\nname = Saria\r\n";
    const std::string options = "# color = blue\r\npvp = off\r\nLICHII_COOP_PUPPET=1\r\n# LICHII_COOP_TUNICS=0\r\n";
    Options o = Options::Read(coop, options);
    CHECK(o.name == "Saria");
    CHECK(o.colour.empty());
    CHECK(o.tunics);
    CHECK(o.hud);
    CHECK(o.chat_key == "T");
    CHECK_FALSE(o.pvp);
    CHECK(o.upnp);
    CHECK(o.password.empty());

    o.name = "Malon";
    o.colour = "red";
    o.tunics = false;
    o.chat_key = "F2";
    o.pvp = true;
    o.upnp = false;
    o.password = "OURSECRET";
    o.cheats = true;
    std::string c2 = coop, o2 = options;
    o.Write(c2, o2);
    CHECK(c2 == "server = 1.2.3.4\r\nname = Malon\r\n");
    CHECK(IniGet(o2, "color") == "red");
    CHECK(IniGet(o2, "LICHII_COOP_TUNICS") == "0");
    CHECK(IniGet(o2, "LICHII_COOP_CHAT_KEY") == "F2");
    CHECK(IniGet(o2, "pvp") == "on");
    CHECK(IniGet(o2, "upnp") == "no");
    CHECK(IniGet(o2, "server_password") == "OURSECRET");
    CHECK(IniGet(o2, "LICHII_COOP_CHEATS") == "1");
    CHECK(IniGet(o2, "LICHII_COOP_PUPPET") == "1");   // what the menu does not edit stays
    CHECK(o2.find("# color = blue") == std::string::npos);   // the commented line became the setting

    const Options back = Options::Read(c2, o2);
    CHECK(back.name == "Malon");
    CHECK(back.colour == "red");
    CHECK_FALSE(back.tunics);
    CHECK(back.chat_key == "F2");
    CHECK(back.pvp);
    CHECK_FALSE(back.upnp);
    CHECK(back.password == "OURSECRET");
    CHECK(back.cheats);

    // back to the defaults: the lines are commented out again
    Options d;   // (as read from these files, then every setting set back)
    d.name = "Malon";
    d.colour_in_file = back.colour_in_file;
    d.Write(c2, o2);
    CHECK_FALSE(IniGet(o2, "color").has_value());
    CHECK_FALSE(IniGet(o2, "LICHII_COOP_TUNICS").has_value());
    CHECK_FALSE(IniGet(o2, "server_password").has_value());
    CHECK(IniGet(o2, "pvp") == "off");
}

TEST_CASE("title menu: unknown values in the files read as the defaults") {
    const Options o = Options::Read("name = \r\n", "color = mauve\r\nLICHII_COOP_CHAT_KEY=k9\r\nserver_password = a b\r\n");
    CHECK(o.name == "Player");
    CHECK(o.colour == "mauve");   // (kept as written: the menu never deletes a colour line it did not change)
    CHECK(o.chat_key == "T");
    CHECK(o.password.empty());
    CHECK(Options::Read("", "color = Grey\r\n").colour == "Grey");   // (not one of the ten tunics: shown as written)
    CHECK(Options::Read("", "color = #ff8020\r\n").colour == "#ff8020");
    CHECK(Options::Read("", "color = Orange\r\n").colour == "orange");   // a tunic name: the row's value
    CHECK(Colours().size() == 11);   // none + the ten tunics
    CHECK(Options::Read("", "LICHII_COOP_CHAT_KEY=off\r\n").chat_key == "off");
}

TEST_CASE("title menu: the settings' variables for this run") {
    Options o;
    o.chat_key = "Y";
    o.hud = false;
    const auto env = o.Environment();
    bool hud = false, key = false;
    for (const auto& [k, v] : env) {
        hud |= k == "LICHII_COOP_HUD" && v == "0";
        key |= k == "LICHII_COOP_CHAT_KEY" && v == "Y";
    }
    CHECK(hud);
    CHECK(key);
}

TEST_CASE("title menu: coop.ini for joining") {
    const std::string t = CoopIniFor("server = old\r\nport = 27510\r\nname = A\r\n", "127.0.0.1:27512", "Saria", "PW");
    CHECK(IniGet(t, "server") == "127.0.0.1:27512");
    CHECK_FALSE(IniGet(t, "port").has_value());
    CHECK(IniGet(t, "name") == "Saria");
    CHECK(IniGet(t, "password") == "PW");
    CHECK_FALSE(IniGet(CoopIniFor(t, "x", "Saria", ""), "password").has_value());
}

TEST_CASE("title menu: the Join field is an invite code, an address or an error") {
    CodeResult none;
    CHECK(std::holds_alternative<std::string>(ReadJoin("   ", none)));
    const auto addr = ReadJoin(" 25.1.2.3:27510 ", none);
    REQUIRE(std::holds_alternative<JoinAddress>(addr));
    CHECK(std::get<JoinAddress>(addr).server == "25.1.2.3:27510");
    CHECK(std::holds_alternative<JoinAddress>(ReadJoin("myhost.example", none)));
    CHECK(std::holds_alternative<std::string>(ReadJoin("1.2.3.4:99999", none)));
    CHECK(std::holds_alternative<std::string>(ReadJoin("hello, there", none)));
    const auto with = ReadJoin("25.1.2.3 OURSECRET", none);
    REQUIRE(std::holds_alternative<JoinAddress>(with));
    CHECK(std::get<JoinAddress>(with).server == "25.1.2.3");
    CHECK(std::get<JoinAddress>(with).password == "OURSECRET");
    CHECK(std::get<JoinAddress>(addr).password.empty());
    CHECK(std::holds_alternative<std::string>(ReadJoin("25.1.2.3 two words", none)));

    CodeResult bad;
    bad.looks_like_code = true;
    bad.error = "a letter is wrong";
    CHECK(std::get<std::string>(ReadJoin("ABCDE-FGHJK", bad)) == "a letter is wrong");

    CodeResult good;
    good.looks_like_code = true;
    good.invite = JoinInvite{"203.0.113.5:27510", "PW"};
    const auto inv = ReadJoin("ABCDE", good);
    REQUIRE(std::holds_alternative<JoinInvite>(inv));
    CHECK(std::get<JoinInvite>(inv).host == "203.0.113.5:27510");
}

TEST_CASE("title menu: the main page moves round, Back closes it") {
    Menu m;
    CHECK(m.Press(Input::kDecide) == Sound::kNone);   // closed: nothing
    m.Open(Options{});
    CHECK(m.open());
    CHECK(m.page() == Page::kMain);
    CHECK(m.view().title == "Multiplayer");
    CHECK(m.view().rows.size() == 4);
    CHECK(m.view().rows[0].selected);
    CHECK(m.Press(Input::kUp) == Sound::kMove);
    CHECK(m.selected() == 3);   // round to Back
    CHECK(m.Press(Input::kDown) == Sound::kMove);
    CHECK(m.selected() == 0);
    CHECK(m.Press(Input::kBack) == Sound::kBack);
    CHECK_FALSE(m.open());
}

TEST_CASE("title menu: it opens on another page for the title's own rows, and Back closes it") {
    Menu m;
    m.Open(Options{}, Page::kSettings);
    CHECK(m.page() == Page::kSettings);
    CHECK(m.view().title == "Co-op Settings");
    CHECK(m.Press(Input::kBack) == Sound::kBack);
    CHECK_FALSE(m.open());   // (opened by the title's row: back to the title)
    m.Open(Options{}, Page::kJoin);
    CHECK(m.view().title == "Join");
    m.Open(Options{}, Page::kClosed);
    CHECK(m.page() == Page::kMain);
}

TEST_CASE("title menu: Host asks the module to host, OK closes the dialog") {
    Menu m;
    m.Open(Options{});
    CHECK(m.Press(Input::kDecide) == Sound::kStart);
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kHost);
    CHECK_FALSE(m.TakeRequest().has_value());
    m.ShowPage(Page::kHosting);
    m.SetStatus({"Send this code to your friends:", "ABCDE-FGHJK"});
    CHECK(m.view().title == "Host");
    CHECK(m.view().rows[0].label == "OK");
    CHECK(m.view().rows[2].label == "Paste friend's answer");
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kCopyInvite);
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kPasteAnswer);
    REQUIRE(m.view().rows.size() == 4);
    CHECK(m.view().rows[3].label == "Same network or VPN");
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kCopyLan);
    CHECK(m.page() == Page::kHosting);
    CHECK(m.page() == Page::kHosting);   // (the dialog stays: the result shows in it)
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kBack);
    CHECK_FALSE(m.open());
}

TEST_CASE("title menu: Join: A on the code pastes, OK joins, Cancel closes") {
    Menu m;
    m.Open(Options{}, Page::kJoin);
    CHECK(m.view().rows[0].value == "(press A to paste it)");
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);   // A on the code
    CHECK(m.TakeRequest()->kind == Request::Kind::kPaste);
    CHECK(m.Paste("  ABCDE-FGHJK\r\n"));
    CHECK(m.join_text() == "ABCDE-FGHJK");
    CHECK(m.view().rows[m.selected()].label == "OK");   // the paste put the cursor on OK
    CHECK(m.Press(Input::kDecide) == Sound::kStart);
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "ABCDE-FGHJK");
    m.Open(Options{}, Page::kJoin);
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kRefuse);   // OK with nothing pasted
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.Char('a'));
    CHECK(m.Press(Input::kErase) == Sound::kMove);
    CHECK(m.join_text().empty());
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kBack);   // Cancel
    CHECK_FALSE(m.open());
}

TEST_CASE("title menu: Co-op Settings shows name, colour, players and voice, Save writes, Back forgets") {
    Options start;
    start.name = "Saria";
    Menu m;
    m.Open(start, Page::kSettings);
    const auto v = m.view();
    REQUIRE(v.rows.size() == 6);
    CHECK(v.rows[0].label == "Your name");
    CHECK(v.rows[0].value == "Saria");
    CHECK(v.rows[1].label == "Your colour");
    CHECK(v.rows[2].label == "Players when you host");
    CHECK(v.rows[3].label == "Voice chat");
    CHECK(v.rows[4].label == "Save");
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kRight) == Sound::kMove);
    CHECK(m.options().colour == "red");
    m.Press(Input::kDown);
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kMove);   // voice off
    CHECK_FALSE(m.options().voice);
    CHECK(m.Press(Input::kBack) == Sound::kBack);   // not saved
    CHECK_FALSE(m.open());

    m.Open(start, Page::kSettings);
    CHECK(m.options().voice);
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);   // edit the name
    CHECK(m.editing());
    m.Press(Input::kErase);
    CHECK(m.Char('!'));
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);
    CHECK(m.options().name == "Sari!");
    for (int i = 0; i < 4; ++i) {
        m.Press(Input::kDown);
    }
    REQUIRE(m.view().rows[m.selected()].label == "Save");
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(m.options().name == "Sari!");
    CHECK_FALSE(m.open());
}

TEST_CASE("title menu: Save refuses an empty name") {
    Options start;
    start.name = "";
    Menu m;
    m.Open(start, Page::kSettings);
    for (int i = 0; i < 4; ++i) {
        m.Press(Input::kDown);
    }
    CHECK(m.Press(Input::kDecide) == Sound::kRefuse);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.page() == Page::kSettings);
}

TEST_CASE("title menu: the name stops at 16 characters") {
    Options start;
    start.name = "abcdefghijklmno";
    Menu m;
    m.Open(start, Page::kSettings);
    m.Press(Input::kDecide);
    m.Char('p');
    m.Char('q');
    CHECK(m.options().name == "abcdefghijklmnop");
    CHECK(m.Paste("xyz"));
    CHECK(m.options().name == "abcdefghijklmnop");
}

TEST_CASE("title menu: Up or Down while editing ends the edit, and keys only type into a field") {
    Menu m;
    m.Open(Options{});
    CHECK_FALSE(m.Char('a'));   // the main page has no field
    CHECK_FALSE(m.Paste("x"));
    m.Open(Options{}, Page::kSettings);
    m.Press(Input::kDecide);
    CHECK(m.editing());
    CHECK(m.Press(Input::kRight) == Sound::kNone);
    m.Press(Input::kDown);
    CHECK_FALSE(m.editing());
    CHECK_FALSE(m.Char('a'));
}

TEST_CASE("title menu: voice off is LICHII_COOP_VOICE=off, on is the default") {
    Options o;
    o.voice = false;
    std::string c, opt;
    o.Write(c, opt);
    CHECK(IniGet(opt, "LICHII_COOP_VOICE") == "off");
    CHECK_FALSE(Options::Read("", opt).voice);
    o.voice = true;
    o.Write(c, opt);
    CHECK_FALSE(IniGet(opt, "LICHII_COOP_VOICE").has_value());
    CHECK(Options::Read("", opt).voice);
}

TEST_CASE("title menu: Host and Join in plain words, nothing about ports or passwords") {
    HostView h;
    h.host = true;
    h.state = HOS_COOP_HOSTING;
    h.invite = "ABCDE-FGHJK";
    h.copied = true;
    h.players = 1;
    auto s = PlainStatus(h);
    REQUIRE(s.size() >= 3);
    CHECK(s[0] == "Send this code to your friends:");
    CHECK(s[1] == "ABCDE-FGHJK");
    CHECK(s[2].find("copied") != std::string::npos);
    h.players = 3;
    CHECK(PlainStatus(h).back() == "2 friend(s) joined.");
    h.invite.clear();
    CHECK(PlainStatus(h)[0] == "Making your code...");
    h.problem = HOS_COOP_PROBLEM_PORT_TAKEN;
    CHECK(PlainStatus(h)[0] == "Another co-op window is already open on this PC.");
    h.problem = HOS_COOP_PROBLEM_BAD_CODE;   // the host pasted an answer code that is not right
    h.invite = "ABCDE-FGHJK";
    s = PlainStatus(h);
    CHECK(s[0] == "That answer code didn't work: ask your friend to copy it again.");
    CHECK(s[2] == "ABCDE-FGHJK");   // (the invite code stays in sight)

    HostView j;
    j.state = HOS_COOP_CONNECTED;
    CHECK(PlainStatus(j)[0] == "Connected! Choose your file and play.");
    j.state = HOS_COOP_FAILED;
    j.problem = HOS_COOP_PROBLEM_BAD_CODE;
    CHECK(PlainStatus(j)[0] == "That code is not right.");
    j.problem = HOS_COOP_PROBLEM_NONE;
    CHECK(PlainStatus(j)[0] == "Your friend could not be reached.");
    j.state = HOS_COOP_WAIT_HOST;
    j.answer = "QWERT-YUPAS";
    s = PlainStatus(j);
    CHECK(s[0] == "Your friend's router blocks the way in.");
    CHECK(s[2] == "QWERT-YUPAS");
    for (const auto& st : {HOS_COOP_STARTING, HOS_COOP_HOSTING, HOS_COOP_JOINING, HOS_COOP_WAIT_HOST, HOS_COOP_CONNECTED,
                           HOS_COOP_FAILED}) {
        for (const bool host : {true, false}) {
            HostView v;
            v.state = st;
            v.host = host;
            v.invite = "ABCDE";
            for (const auto& line : PlainStatus(v)) {
                CHECK(line.find("port") == std::string::npos);
                CHECK(line.find("password") == std::string::npos);
                CHECK(line.find("address") == std::string::npos);
            }
        }
    }
}

TEST_CASE("title menu: the kit's options as the launcher's variables") {
    const std::string t = "# LICHII_COOP_CHEATS=1\r\nauto_update = ask\r\npvp = off\r\ncolor = blue  # mine\r\n"
                          "LICHII_COOP_PUPPET=1\r\nLICHII_COOP_FLAGS = shared_flags.txt\r\nEDEN_NX2_X=2\r\n"
                          "LICHII_COOP_bad=1\r\nserver_password = PW\r\n";
    const auto v = KitVariables(t);
    REQUIRE(v.size() == 4);
    CHECK(v[0] == std::pair<std::string, std::string>{"LICHII_COOP_COLOR", "blue"});
    CHECK(v[1] == std::pair<std::string, std::string>{"LICHII_COOP_PUPPET", "1"});
    CHECK(v[2] == std::pair<std::string, std::string>{"LICHII_COOP_FLAGS", "shared_flags.txt"});
    CHECK(v[3] == std::pair<std::string, std::string>{"EDEN_NX2_X", "2"});
    CHECK(KitVariables("color =\r\n").empty());
}

TEST_CASE("title menu: the host's player count, 2 to 16, round the ends, read and written") {
    Menu m;
    m.Open(Options{}, Page::kSettings);
    for (int i = 0; i < 2; ++i) {
        m.Press(Input::kDown);
    }
    REQUIRE(m.view().rows[m.selected()].label == "Players when you host");
    CHECK(m.view().rows[m.selected()].value == "4");
    m.Press(Input::kLeft);
    m.Press(Input::kLeft);
    CHECK(m.options().max_players == 2);
    m.Press(Input::kLeft);
    CHECK(m.options().max_players == 16);
    m.Press(Input::kRight);
    CHECK(m.options().max_players == 2);
    Options o;
    o.max_players = 12;
    std::string c, opt;
    o.Write(c, opt);
    CHECK(IniGet(opt, "max_players") == "12");
    CHECK(Options::Read("", opt).max_players == 12);
    CHECK(Options::Read("", "max_players = 40\r\n").max_players == 16);
    CHECK(Options::Read("", "max_players = x\r\n").max_players == 4);
}

TEST_CASE("title menu: Join offers the last host again first, one press") {
    Menu m;
    m.Open(Options{}, Page::kJoin);
    m.SetRejoin("Saria", "ABCDE-FGHJK");
    auto v = m.view();
    REQUIRE(v.rows.size() == 4);
    CHECK(v.rows[0].label == "Join Saria again");
    CHECK(v.rows[0].selected);
    CHECK(v.rows[1].label == "Or a new code");
    CHECK(m.Press(Input::kDecide) == Sound::kStart);
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "ABCDE-FGHJK");
    // a new code still works under it
    m.Open(Options{}, Page::kJoin);
    m.SetRejoin("", "ABCDE-FGHJK");
    CHECK(m.view().rows[0].label == "Join your friend again");
    CHECK(m.Paste("QWERT-YUPAS"));
    CHECK(m.view().rows[m.selected()].label == "OK");
    CHECK(m.Press(Input::kDecide) == Sound::kStart);
    CHECK(m.TakeRequest()->text == "QWERT-YUPAS");
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kBack);   // Cancel
    CHECK_FALSE(m.open());
    // no last code: no such row
    m.Open(Options{}, Page::kJoin);
    m.SetRejoin("Saria", "");
    CHECK(m.view().rows[0].label == "Your friend's code");
}

TEST_CASE("title menu: a colour the row does not list survives a Save that only changes the name") {
    for (const std::string colour : {"gray", "#F82", "0xFF8020", "#FF8020FF", "rgb(255, 128, 32)", "Orange"}) {
        const std::string file = "# Your colour\r\ncolor = " + colour + "  # mine\r\n";
        Options o = Options::Read("name = Saria\r\n", file);
        Menu m;
        m.Open(o, Page::kSettings);
        m.Press(Input::kDecide);   // edit the name
        m.Char('!');
        m.Press(Input::kDecide);
        for (int i = 0; i < 4; ++i) {
            m.Press(Input::kDown);
        }
        REQUIRE(m.view().rows[m.selected()].label == "Save");
        m.Press(Input::kDecide);
        REQUIRE(m.TakeRequest()->kind == Request::Kind::kSave);
        std::string coop = "name = Saria\r\n", opt = file;
        m.options().Write(coop, opt);
        CHECK(IniGet(coop, "name") == "Saria!");
        CHECK(opt.find("color = " + colour + "  # mine") != std::string::npos);   // the line exactly as it was
    }
    // the colour row changed: then it is written
    Options o = Options::Read("", "color = gray\r\n");
    Menu m;
    m.Open(o, Page::kSettings);
    m.Press(Input::kDown);
    m.Press(Input::kRight);
    std::string coop, opt = "color = gray\r\n";
    m.options().Write(coop, opt);
    CHECK(IniGet(opt, "color") == "red");
}

TEST_CASE("title menu: after Join the host's save loads in one step") {
    AutoLoad a;
    CHECK_FALSE(a.active());
    CHECK(a.Line().empty());
    a.Start(1000, "Saria");
    CHECK(a.active());
    CHECK(a.Line() == "Getting Saria's save...");
    // waiting for the save: no presses
    auto t = a.Step(2000, false, true, false);
    CHECK_FALSE(t.press_a);
    CHECK_FALSE(t.focus_continue);
    // the save is here, the title is up: Continue shown (a first-time joiner's title opened without it), the
    // cursor to it, then A pressed now and then
    t = a.Step(3000, true, true, false);
    CHECK(a.Line() == "Loading Saria's save...");
    t = a.Step(3050, true, true, false);
    CHECK(t.reveal_continue);
    CHECK_FALSE(t.focus_continue);
    CHECK_FALSE(t.press_a);
    CHECK_FALSE(a.Step(3100, true, true, false).reveal_continue);   // once
    t = a.Step(3300, true, true, false);
    CHECK(t.focus_continue);
    CHECK_FALSE(t.press_a);   // (the cursor first)
    t = a.Step(3600, true, true, false);
    CHECK(t.press_a);
    CHECK_FALSE(t.focus_continue);   // once only
    t = a.Step(3450, true, false, false);   // held 120 ms, then let go (the title may be gone: the file list)
    CHECK_FALSE(t.press_a);
    t = a.Step(3300 + 120 + AutoLoad::kPressEveryMs + 150, true, false, false);
    t = a.Step(3300 + 2 * AutoLoad::kPressEveryMs, true, false, false);
    int presses = 0;
    for (std::uint64_t ms = 4000; ms < 10000; ms += 20) {
        presses += a.Step(ms, true, false, false).press_a ? 1 : 0;
    }
    CHECK(presses > 10);   // (A held in a few short presses, about every 1.2 s)
    CHECK(presses < 60);
    // Link is in the game: done, nothing more pressed, no line
    a.Step(10000, true, false, true);
    CHECK_FALSE(a.active());
    CHECK(a.Line().empty());
    CHECK_FALSE(a.Step(10500, true, false, false).press_a);
}

TEST_CASE("title menu: no host's save, or no way in: a plain line and no endless presses") {
    AutoLoad a;
    a.Start(0, "");
    a.Step(AutoLoad::kWaitSaveMs + 1, false, true, false);
    CHECK_FALSE(a.active());
    CHECK(a.Line() == "your friend's save did not arrive: Continue plays your own save, New Game starts a new one.");
    AutoLoad b;
    b.Start(0, "Saria");
    b.Step(100, true, true, false);
    b.Step(100 + AutoLoad::kLoadMs + 1, true, true, false);
    CHECK_FALSE(b.active());
    CHECK(b.Line() == "Press Continue to play Saria's save.");
    CHECK_FALSE(b.Step(100 + AutoLoad::kLoadMs + 500, true, true, false).press_a);
}

TEST_CASE("title menu: a host with no save: AutoLoad stops waiting at once and presses nothing (hostsave-fix)") {
    AutoLoad a;
    a.Start(0, "Saria");
    CHECK(a.Line() == "Getting Saria's save...");
    a.Step(500, false, true, false);
    a.NoSave();
    CHECK_FALSE(a.active());
    CHECK(a.Line() == "Saria has no save yet: New Game starts one, Continue plays your own.");
    for (std::uint64_t t = 600; t < AutoLoad::kWaitSaveMs * 2; t += 100) {
        CHECK_FALSE(a.Step(t, false, true, false).press_a);
    }
    // While waiting, nothing is pressed: the game stays on the player's own folder until the save is whole.
    AutoLoad b;
    b.Start(0, "Saria");
    for (std::uint64_t t = 100; t < AutoLoad::kWaitSaveMs; t += 100) {
        const auto tick = b.Step(t, false, true, false);
        CHECK_FALSE(tick.press_a);
        CHECK_FALSE(tick.reveal_continue);
        CHECK_FALSE(tick.focus_continue);
    }
    b.NoSave();   // (only while waiting)
    AutoLoad c;
    c.Start(0, "Saria");
    c.Step(100, true, true, false);
    c.NoSave();
    CHECK(c.active());
}

TEST_CASE("title menu: the player picking a file while the host's save comes stops AutoLoad (hostsave-fix)") {
    // Only the player's own A on a title button that is not a co-op row counts.
    CHECK(AutoLoad::PlayerPicksFile(true, false, AutoLoad::kContinue, false));
    CHECK(AutoLoad::PlayerPicksFile(true, false, 0, false));
    CHECK_FALSE(AutoLoad::PlayerPicksFile(true, true, AutoLoad::kContinue, false));   // AutoLoad's own press
    CHECK_FALSE(AutoLoad::PlayerPicksFile(true, false, 4, true));                    // Host / Join / Settings
    CHECK_FALSE(AutoLoad::PlayerPicksFile(true, false, -1, false));                  // no button focused
    CHECK_FALSE(AutoLoad::PlayerPicksFile(false, false, AutoLoad::kContinue, false)); // not the title
    AutoLoad a;
    a.Start(0, "Saria");
    CHECK(a.waiting());
    a.PlayerChose();
    CHECK_FALSE(a.active());
    CHECK(a.Line().empty());   // (their choice: no "did not arrive" line)
    CHECK_FALSE(a.Step(AutoLoad::kWaitSaveMs + 1, true, true, false).press_a);
    // Once loading the host's save, the player's A changes nothing.
    AutoLoad b;
    b.Start(0, "Saria");
    b.Step(100, true, true, false);
    CHECK_FALSE(b.waiting());
    b.PlayerChose();
    CHECK(b.active());
}

TEST_CASE("title menu: with no Continue under the cursor AutoLoad presses nothing on the title") {
    CHECK_FALSE(AutoLoad::PressAllowed(true, 0));    // New Game focused (Continue hidden: the cursor could not go there)
    CHECK_FALSE(AutoLoad::PressAllowed(true, -1));   // nothing focused
    CHECK_FALSE(AutoLoad::PressAllowed(true, 4));    // one of our rows
    CHECK(AutoLoad::PressAllowed(true, AutoLoad::kContinue));
    CHECK(AutoLoad::PressAllowed(false, 0));         // the title gone: the file list takes the A
    // and AutoLoad then gives up with the plain line, no endless presses
    AutoLoad a;
    a.Start(0, "Saria");
    a.Step(10, true, true, false);
    a.Step(AutoLoad::kLoadMs + 20, true, true, false);
    CHECK(a.Line() == "Press Continue to play Saria's save.");
}

TEST_CASE("Same network or VPN: the addresses a friend can use, and the line Join takes") {
    using namespace CoopTitleMenu;
    CHECK(LanLabel("192.168.1.20") == "home network");
    CHECK(LanLabel("10.0.0.5") == "home network");
    CHECK(LanLabel("172.16.4.4") == "home network");
    CHECK(LanLabel("172.32.4.4").empty());
    CHECK(LanLabel("26.12.34.56") == "Radmin VPN");
    CHECK(LanLabel("25.1.2.3") == "Hamachi");
    CHECK(LanLabel("100.100.1.2") == "Tailscale");
    CHECK(LanLabel("100.128.1.2").empty());
    CHECK(LanLabel("127.0.0.1").empty());
    CHECK(LanLabel("169.254.3.3").empty());
    CHECK(LanLabel("8.8.8.8").empty());
    CHECK(LanLabel("192.168.1").empty());
    CHECK(LanLabel("192.168.1.999").empty());
    CHECK(LanLabel("fe80::1").empty());
    // VPN addresses first, nothing twice, the unusable ones dropped.
    const auto a = LanAddresses({"127.0.0.1", "192.168.1.20", "26.12.34.56", "192.168.1.20", "8.8.8.8"});
    REQUIRE(a.size() == 2);
    CHECK(a[0].address == "26.12.34.56");
    CHECK(a[1].address == "192.168.1.20");
    // The copied line is what Join's field reads as an address with its password.
    const std::string line = LanJoinText(a[0].address, 27510, "abcdefghijklmnopqrst");
    CHECK(line == "26.12.34.56:27510 abcdefghijklmnopqrst");
    const JoinTarget t = ReadJoin(line, CodeResult{});
    REQUIRE(std::holds_alternative<JoinAddress>(t));
    CHECK(std::get<JoinAddress>(t).server == "26.12.34.56:27510");
    CHECK(std::get<JoinAddress>(t).password == "abcdefghijklmnopqrst");
    const auto lines = LanLines(a, 27510, "abcdefghijklmnopqrst", true);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].find("Radmin VPN") != std::string::npos);
    CHECK(lines[1] == line);
    CHECK(lines[2].find("copied") != std::string::npos);
    CHECK(lines[3].find("192.168.1.20 (home network)") != std::string::npos);
    // No usable address: say what to do.
    const auto none = LanLines({}, 27510, "x", false);
    REQUIRE(none.size() == 2);
    CHECK(none[1].find("VPN program") != std::string::npos);
}

TEST_CASE("Join says what to paste on a home network or VPN until something else is said") {
    using namespace CoopTitleMenu;
    Menu m;
    m.Open(Options{}, Page::kJoin);
    REQUIRE(m.view().status.size() == 2);
    CHECK(m.view().status[0].find("home network or VPN") != std::string::npos);
    m.SetStatus({"Joining..."});
    CHECK(m.view().status == std::vector<std::string>{"Joining..."});
}
