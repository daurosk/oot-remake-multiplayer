// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer menu on Hookshot's title screen (hookshot/src/coop_title_menu.h).
#include <doctest/doctest.h>

#include <string>
#include <variant>

#include "coop_title_menu.h"

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
    Options d;
    d.name = "Malon";
    d.Write(c2, o2);
    CHECK_FALSE(IniGet(o2, "color").has_value());
    CHECK_FALSE(IniGet(o2, "LICHII_COOP_TUNICS").has_value());
    CHECK_FALSE(IniGet(o2, "server_password").has_value());
    CHECK(IniGet(o2, "pvp") == "off");
}

TEST_CASE("title menu: unknown values in the files read as the defaults") {
    const Options o = Options::Read("name = \r\n", "color = mauve\r\nLICHII_COOP_CHAT_KEY=k9\r\nserver_password = a b\r\n");
    CHECK(o.name == "Player");
    CHECK(o.colour.empty());
    CHECK(o.chat_key == "T");
    CHECK(o.password.empty());
    CHECK(Options::Read("", "color = Grey\r\n").colour == "gray");
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

TEST_CASE("title menu: it opens on another page for the title's own rows") {
    Menu m;
    m.Open(Options{}, Page::kSettings);
    CHECK(m.page() == Page::kSettings);
    CHECK(m.view().title == "Multiplayer settings");
    CHECK(m.Press(Input::kBack) == Sound::kBack);
    CHECK(m.page() == Page::kMain);
    m.Open(Options{}, Page::kJoin);
    CHECK(m.view().title == "Join a game");
    m.Open(Options{}, Page::kClosed);
    CHECK(m.page() == Page::kMain);
}

TEST_CASE("title menu: Host asks the module to host") {
    Menu m;
    m.Open(Options{});
    CHECK(m.Press(Input::kDecide) == Sound::kStart);
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kHost);
    CHECK_FALSE(m.TakeRequest().has_value());
    m.ShowPage(Page::kHosting);
    m.SetStatus({"Invite code: ABCDE-..."});
    CHECK(m.view().title == "Hosting");
    CHECK(m.view().status.size() == 1);
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kCopyInvite);
    CHECK(m.Press(Input::kBack) == Sound::kBack);
    CHECK_FALSE(m.open());
}

TEST_CASE("title menu: Join takes typed or pasted text and asks to join it") {
    Menu m;
    m.Open(Options{});
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kOpen);
    CHECK(m.page() == Page::kJoin);
    m.Press(Input::kDown);
    m.Press(Input::kDown);   // on Join, the field empty
    CHECK(m.Press(Input::kDecide) == Sound::kRefuse);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.Char('a'));
    CHECK(m.Char('b'));
    CHECK(m.Press(Input::kErase) == Sound::kMove);
    CHECK(m.join_text() == "a");
    CHECK(m.Press(Input::kPaste) == Sound::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kPaste);
    CHECK(m.Paste("  ABCDE-FGHJK\r\n"));
    CHECK(m.join_text() == "ABCDE-FGHJK");
    CHECK(m.view().rows[0].value == "ABCDE-FGHJK");
    CHECK(m.Press(Input::kDecide) == Sound::kStart);   // the paste put the cursor on Join
    const auto r = m.TakeRequest();
    REQUIRE(r.has_value());
    CHECK(r->kind == Request::Kind::kJoin);
    CHECK(r->text == "ABCDE-FGHJK");
    CHECK(m.Press(Input::kBack) == Sound::kBack);
    CHECK(m.page() == Page::kMain);
    CHECK(m.selected() == 1);
}

TEST_CASE("title menu: Settings change with Left and Right, Save asks to write them, Back forgets") {
    Options start;
    start.name = "Saria";
    Menu m;
    m.Open(start);
    m.Press(Input::kDown);
    m.Press(Input::kDown);
    CHECK(m.Press(Input::kDecide) == Sound::kOpen);
    REQUIRE(m.page() == Page::kSettings);
    CHECK(m.view().rows[0].value == "Saria");
    m.Press(Input::kDown);   // colour
    CHECK(m.Press(Input::kRight) == Sound::kMove);
    CHECK(m.options().colour == "red");
    CHECK(m.Press(Input::kLeft) == Sound::kMove);
    CHECK(m.Press(Input::kLeft) == Sound::kMove);
    CHECK(m.options().colour == "gray");   // round the list
    m.Press(Input::kDown);   // tunics
    CHECK(m.Press(Input::kDecide) == Sound::kMove);
    CHECK_FALSE(m.options().tunics);
    CHECK(m.Press(Input::kBack) == Sound::kBack);   // not saved
    CHECK(m.page() == Page::kMain);
    CHECK(m.options().tunics);
    CHECK(m.options().colour.empty());

    m.Press(Input::kDecide);   // Settings again (the cursor came back to it)
    REQUIRE(m.page() == Page::kSettings);
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);   // edit the name
    CHECK(m.editing());
    CHECK(m.view().rows[0].editing);
    m.Press(Input::kErase);
    CHECK(m.Char('!'));
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);   // done
    CHECK_FALSE(m.editing());
    CHECK(m.options().name == "Sari!");
    for (int i = 0; i < 12; ++i) {
        m.Press(Input::kDown);
    }
    CHECK(m.view().rows[m.selected()].label == "Save");
    CHECK(m.Press(Input::kDecide) == Sound::kDecide);
    CHECK(m.TakeRequest()->kind == Request::Kind::kSave);
    CHECK(m.page() == Page::kMain);
    CHECK(m.options().name == "Sari!");
    CHECK(m.view().status[0] == "Settings saved.");
}

TEST_CASE("title menu: Save refuses a bad name or password") {
    Options start;
    start.name = "";
    Menu m;
    m.Open(start);
    m.Press(Input::kDown);
    m.Press(Input::kDown);
    m.Press(Input::kDecide);
    for (int i = 0; i < 12; ++i) {
        m.Press(Input::kDown);
    }
    CHECK(m.Press(Input::kDecide) == Sound::kRefuse);
    CHECK_FALSE(m.TakeRequest().has_value());
    CHECK(m.page() == Page::kSettings);
}

TEST_CASE("title menu: the name stops at 16 characters, the password takes no spaces") {
    Options start;
    start.name = "abcdefghijklmno";
    Menu m;
    m.Open(start);
    m.Press(Input::kDown);
    m.Press(Input::kDown);
    m.Press(Input::kDecide);
    m.Press(Input::kDecide);   // edit the name
    m.Char('p');
    m.Char('q');
    CHECK(m.options().name == "abcdefghijklmnop");
    m.Press(Input::kDecide);
    for (int i = 0; i < 9; ++i) {
        m.Press(Input::kDown);
    }
    CHECK(m.view().rows[m.selected()].value == "New each session");
    m.Press(Input::kDecide);
    CHECK(m.Paste("my pass"));
    CHECK(m.options().password == "my");
    m.Char(' ');
    m.Char('x');
    CHECK(m.options().password == "myx");
}

TEST_CASE("title menu: Up or Down while editing ends the edit, and keys only type into a field") {
    Menu m;
    m.Open(Options{});
    CHECK_FALSE(m.Char('a'));   // the main page has no field
    CHECK_FALSE(m.Paste("x"));
    m.Press(Input::kDown);
    m.Press(Input::kDown);
    m.Press(Input::kDecide);
    m.Press(Input::kDecide);
    CHECK(m.editing());
    CHECK(m.Press(Input::kRight) == Sound::kNone);
    m.Press(Input::kDown);
    CHECK_FALSE(m.editing());
    CHECK_FALSE(m.Char('a'));
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
    for (int i = 0; i < 7; ++i) {
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
