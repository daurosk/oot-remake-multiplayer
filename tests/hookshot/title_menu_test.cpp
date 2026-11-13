// SPDX-License-Identifier: GPL-3.0-or-later
// What the Multiplayer tab works with (hookshot/src/coop_title_menu.h): the settings files, the Join field,
// Host and Join in plain words, the addresses for a home network or VPN, and the host's save loaded after a join.
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
    CHECK(a.Line() == "your friend's save did not arrive: choose a file to play.");
    AutoLoad b;
    b.Start(0, "Saria");
    b.Step(100, true, true, false);
    b.Step(100 + AutoLoad::kLoadMs + 1, true, true, false);
    CHECK_FALSE(b.active());
    CHECK(b.Line() == "Press Continue to play Saria's save.");
    CHECK_FALSE(b.Step(100 + AutoLoad::kLoadMs + 500, true, true, false).press_a);
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

TEST_CASE("title menu: voice chat is off unless turned on: on writes ptt, off takes the line away") {
    Options o;
    CHECK_FALSE(o.voice);
    std::string c, opt = "LICHII_COOP_VOICE=ptt\r\n";
    o.Write(c, opt);
    CHECK_FALSE(IniGet(opt, "LICHII_COOP_VOICE").has_value());
    CHECK_FALSE(Options::Read("", opt).voice);
    o.voice = true;
    o.Write(c, opt);
    CHECK(IniGet(opt, "LICHII_COOP_VOICE") == "ptt");
    CHECK(Options::Read("", opt).voice);
    // a mode written by hand stays as it is while voice stays on
    std::string open = "LICHII_COOP_VOICE=open\r\n";
    Options::Read("", open).Write(c, open);
    CHECK(IniGet(open, "LICHII_COOP_VOICE") == "open");
    CHECK_FALSE(Options::Read("", "LICHII_COOP_VOICE=off\r\n").voice);
    // this run's variable: ptt, or unset for off
    bool seen = false;
    for (const auto& [k, v] : o.Environment()) {
        if (k == "LICHII_COOP_VOICE") {
            seen = true;
            CHECK(v == "ptt");
        }
    }
    CHECK(seen);
    // "open" by hand counts for this run too (Host / Join set the variable), not ptt in its place
    const auto by_hand = Options::Read("", "LICHII_COOP_VOICE=Open\r\n");
    CHECK(by_hand.VoiceMode() == "open");
    bool open_seen = false;
    for (const auto& [k, v] : by_hand.Environment()) {
        if (k == "LICHII_COOP_VOICE") {
            open_seen = true;
            CHECK(v == "open");
        }
    }
    CHECK(open_seen);
    CHECK(Options::Read("", "LICHII_COOP_VOICE=on\r\n").VoiceMode() == "ptt");
}

TEST_CASE("title menu: the host's player count, 2 to 16, read and written") {
    Options o;
    o.max_players = 12;
    std::string c, opt;
    o.Write(c, opt);
    CHECK(IniGet(opt, "max_players") == "12");
    CHECK(Options::Read("", opt).max_players == 12);
    CHECK(Options::Read("", "max_players = 40\r\n").max_players == 16);
    CHECK(Options::Read("", "max_players = x\r\n").max_players == 4);
}

TEST_CASE("title menu: a colour the Look row does not list survives a Save that only changes the name") {
    for (const std::string colour : {"gray", "#F82", "0xFF8020", "#FF8020FF", "rgb(255, 128, 32)", "Orange"}) {
        const std::string file = "# Your colour\r\ncolor = " + colour + "  # mine\r\n";
        Options o = Options::Read("name = Saria\r\n", file);
        o.name = "Saria!";
        std::string coop = "name = Saria\r\n", opt = file;
        o.Write(coop, opt);
        CHECK(IniGet(coop, "name") == "Saria!");
        CHECK(opt.find("color = " + colour + "  # mine") != std::string::npos);   // the line exactly as it was
    }
    Options o = Options::Read("", "color = gray\r\n");
    o.colour = "red";   // the row changed: then it is written
    std::string coop, opt = "color = gray\r\n";
    o.Write(coop, opt);
    CHECK(IniGet(opt, "color") == "red");
}
