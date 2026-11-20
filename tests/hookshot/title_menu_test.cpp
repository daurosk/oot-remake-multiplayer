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
    // First: the invite code is the way to host (it is on the clipboard); the other rows come after.
    CHECK(s[0] == "Invite code copied to your clipboard. Share it in DMs only: it contains your internet address.");
    CHECK(s[1] == "ABCDE-FGHJK");
    CHECK(s[2] == "Waiting for your friends to join...");
    h.copied = false;
    CHECK(PlainStatus(h)[0].find("Share it in DMs only") != std::string::npos);
    CHECK(PlainStatus(h)[2].find("Copy the code again") != std::string::npos);
    h.copied = true;
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
                // (one exception: the host is told the invite code holds their internet address, privacy-name)
                CHECK((line.find("address") == std::string::npos || line.find("Share it in DMs only: it contains your internet address.") != std::string::npos));
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

namespace {
SaveWait::Input Look(bool ready, bool title, bool coming = false, bool in_game = false) {
    SaveWait::Input in;
    in.host_save_ready = ready;
    in.save_coming = coming;
    in.title_up = title;
    in.in_game = in_game;
    return in;
}
// Steps every 100 ms over [from, to) with the same look.
void Run(SaveWait& a, std::uint64_t from, std::uint64_t to, const SaveWait::Input& in) {
    for (std::uint64_t ms = from; ms < to; ms += 100) a.Step(ms, in);
}
}  // namespace

TEST_CASE("title menu: after Join the title says what the host's save does, and presses nothing") {
    // Tick has nothing that presses: only rearm (the host's save offered again).
    static_assert(sizeof(SaveWait::Tick) == sizeof(bool));
    SaveWait a;
    CHECK_FALSE(a.watching());
    CHECK(a.Line().empty());
    a.Start(1000, "Saria");
    CHECK(a.active());
    CHECK(a.Line() == "Getting Saria's save: don't load a save yet...");
    CHECK_FALSE(a.Step(2000, Look(false, true)).rearm);
    // the save is in and played from: the player loads a save themselves
    CHECK_FALSE(a.Step(3000, Look(true, true)).rearm);
    CHECK(a.watching());
    CHECK_FALSE(a.active());   // (the title's co-op rows work again)
    CHECK(a.Line() == "Saria's save is in: load any save.");
    Run(a, 3100, 60000, Look(true, true));   // (nothing changes while they look at the title)
    CHECK(a.Line() == "Saria's save is in: load any save.");
    // they load it: Link in the game, done
    a.Step(60100, Look(true, false, false, true));
    CHECK_FALSE(a.watching());
    CHECK(a.Line().empty());
}

TEST_CASE("title menu: only title time with nothing coming counts toward late, a late save is still announced") {
    SaveWait a;
    a.Start(0, "");
    // a menu over the title, a file list, a slow PC (no title steps): no wait counted
    Run(a, 100, 3 * SaveWait::kWaitSaveMs, Look(false, false));
    CHECK(a.active());
    // a transfer under way on the title: no wait counted either
    Run(a, 3 * SaveWait::kWaitSaveMs, 5 * SaveWait::kWaitSaveMs, Look(false, true, true));
    CHECK(a.active());
    // a hitch (a 10 s gap between two looks) counts as 1 s
    a.Step(10 * SaveWait::kWaitSaveMs, Look(false, true));
    CHECK(a.active());
    // a minute of title time with nothing coming: late
    Run(a, 10 * SaveWait::kWaitSaveMs, 11 * SaveWait::kWaitSaveMs + 200, Look(false, true));
    CHECK(a.waiting());
    CHECK_FALSE(a.active());
    CHECK(a.Line() == "your friend's save is not here yet (the chat says Success when it is): Continue plays your "
                      "own save, New Game starts a new one.");
    // parts start coming: back to "Getting"
    a.Step(11 * SaveWait::kWaitSaveMs + 300, Look(false, true, true));
    CHECK(a.active());
    // ... and in
    a.Step(11 * SaveWait::kWaitSaveMs + 400, Look(true, true));
    CHECK(a.Line() == "your friend's save is in: load any save.");
}

TEST_CASE("title menu: a host with no save: the wait stops at once (hostsave-fix)") {
    SaveWait a;
    a.Start(0, "Saria");
    a.Step(500, Look(false, true));
    a.NoSave();
    CHECK_FALSE(a.watching());
    CHECK(a.Line() == "Saria has no save yet: New Game starts one, Continue plays your own.");
    // late: "no save" still ends it
    SaveWait b;
    b.Start(0, "Saria");
    Run(b, 100, SaveWait::kWaitSaveMs + 2000, Look(false, true));
    CHECK(b.waiting());
    b.NoSave();
    CHECK_FALSE(b.watching());
    // once the save is in, "no save" changes nothing
    SaveWait c;
    c.Start(0, "Saria");
    c.Step(100, Look(true, true));
    c.NoSave();
    CHECK(c.watching());
}

TEST_CASE("title menu: the player picking a file while the host's save comes: no line, their choice") {
    // Only the player's own A on a title button that is not a co-op row counts.
    CHECK(SaveWait::PlayerPicksFile(true, 0, false));
    CHECK(SaveWait::PlayerPicksFile(true, 2, false));
    CHECK_FALSE(SaveWait::PlayerPicksFile(true, 4, true));    // Host / Join / Settings
    CHECK_FALSE(SaveWait::PlayerPicksFile(true, -1, false));  // no button focused
    CHECK_FALSE(SaveWait::PlayerPicksFile(false, 0, false));  // not the title
    SaveWait a;
    a.Start(0, "Saria");
    CHECK(a.waiting());
    a.PlayerChose();
    CHECK(a.chose());
    CHECK_FALSE(a.active());   // (the title's rows work)
    CHECK(a.Line().empty());   // (their choice: no line)
    // their file loads: Link in the game, done
    a.Step(3000, Look(true, false, false, true));
    CHECK_FALSE(a.watching());
    // once the save is in, a pick changes nothing
    SaveWait b;
    b.Start(0, "Saria");
    b.Step(100, Look(true, true));
    b.PlayerChose();
    CHECK_FALSE(b.chose());
}

TEST_CASE("title menu: backing out of their own file to the title offers the host's save again (Revirie 0.10.1)") {
    SaveWait a;
    a.Start(0, "Saria");
    a.Step(100, Look(false, true));
    a.PlayerChose();
    // the title still up a moment after the A (TitleUp's 300 ms): nothing yet
    for (std::uint64_t ms = 200; ms < 3000; ms += 100) CHECK_FALSE(a.Step(ms, Look(false, true)).rearm);
    // their file list (the title gone), then back on the title
    Run(a, 3000, 5000, Look(false, false));
    bool rearm = false;
    std::uint64_t ms = 5000;
    for (; ms < 5000 + SaveWait::kBackMs + 500 && !rearm; ms += 100) rearm = a.Step(ms, Look(false, true)).rearm;
    CHECK(rearm);
    CHECK(ms >= 5000 + SaveWait::kBackMs);
    CHECK(a.active());
    CHECK(a.Line() == "Getting Saria's save: don't load a save yet...");
    // A New Game the player went on with: in the game, done, nothing re-armed
    SaveWait b;
    b.Start(0, "Saria");
    b.PlayerChose();
    Run(b, 100, 2000, Look(false, false));
    b.Step(2000, Look(false, false, false, true));
    CHECK_FALSE(b.watching());
    CHECK_FALSE(b.Step(9000, Look(false, true)).rearm);
}

TEST_CASE("title menu: the host drops while the save is awaited: it stops waiting and says so") {
    SaveWait::Input off = Look(false, true);
    off.session_live = false;
    // a join still connecting (never on yet) is not a drop
    SaveWait a;
    a.Start(0, "Saria");
    Run(a, 100, 3000, off);
    CHECK(a.active());
    // on, then off: host gone
    Run(a, 3000, 4000, Look(false, true));
    a.Step(4000, off);
    CHECK_FALSE(a.watching());
    CHECK(a.Line() == "Saria's game ended before the save came: Continue plays your own save, New Game starts a "
                      "new one.");
    // late: the same
    SaveWait late;
    late.Start(0, "Saria");
    Run(late, 100, SaveWait::kWaitSaveMs + 2000, Look(false, true));
    late.Step(SaveWait::kWaitSaveMs + 3000, off);
    CHECK_FALSE(late.watching());
    // once the save is in, a drop does not take the line away (the folder is already the host's)
    SaveWait in;
    in.Start(0, "Saria");
    in.Step(100, Look(true, true));
    SaveWait::Input gone = Look(true, true);
    gone.session_live = false;
    in.Step(200, gone);
    CHECK(in.Line() == "Saria's save is in: load any save.");
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
    REQUIRE(lines.size() >= 4);   // (and the firewall line on Windows)
    CHECK(lines[0].find("Radmin VPN") != std::string::npos);
    CHECK(lines[1] == line);
    CHECK(lines[2].find("copied") != std::string::npos);
    CHECK(lines[3].find("192.168.1.20 (home network)") != std::string::npos);
    // No usable address: say what to do.
    const auto none = LanLines({}, 27510, "x", false);
    REQUIRE(none.size() == 2);
    CHECK(none[1].find("VPN program") != std::string::npos);
}

TEST_CASE("Same network and VPN: two rows, the VPN found by its card's name") {
    using namespace CoopTitleMenu;
    CHECK(VpnLabel("Radmin VPN | Famatech Radmin VPN Ethernet Adapter") == "Radmin VPN");
    CHECK(VpnLabel("Hamachi | LogMeIn Hamachi Virtual Ethernet Adapter") == "Hamachi");
    CHECK(VpnLabel("ham0") == "Hamachi");
    CHECK(VpnLabel("ZeroTier One [8056c2e21c000001] | ZeroTier Virtual Port") == "ZeroTier");
    CHECK(VpnLabel("ztc3qb6hok") == "ZeroTier");
    CHECK(VpnLabel("Tailscale | Tailscale Tunnel") == "Tailscale");
    CHECK(VpnLabel("tailscale0") == "Tailscale");
    CHECK(VpnLabel("wg0") == "WireGuard");
    CHECK(VpnLabel("Office | WireGuard Tunnel") == "WireGuard");
    CHECK(VpnLabel("Wi-Fi | Intel(R) Wi-Fi 6 AX201 160MHz").empty());
    CHECK(VpnLabel("Ethernet | Realtek PCIe GbE Family Controller").empty());
    CHECK(VpnLabel("eth0").empty());
    CHECK(VpnLabel("wlan0").empty());
    CHECK(VpnLabel("").empty());

    // A ZeroTier card on a private range is a VPN, not the home network; the card with the router comes first.
    const LanSplit split = SplitLan({
        {"127.0.0.1", "Loopback Pseudo-Interface 1", false},
        {"192.168.56.1", "Ethernet 2 | VirtualBox Host-Only Ethernet Adapter", false},
        {"192.168.1.20", "Wi-Fi | Intel(R) Wi-Fi 6", true},
        {"10.147.17.5", "ZeroTier One [8056c2e21c000001] | ZeroTier Virtual Port", false},
        {"26.12.34.56", "Radmin VPN | Famatech Radmin VPN Ethernet Adapter", false},
        {"169.254.7.7", "Ethernet 3", false},
        {"192.168.1.20", "Wi-Fi | Intel(R) Wi-Fi 6", true},
    });
    REQUIRE(split.home.size() == 2);
    CHECK(split.home[0].address == "192.168.1.20");
    CHECK(split.home[1].address == "192.168.56.1");
    REQUIRE(split.vpn.size() == 2);
    CHECK(split.vpn[0].address == "10.147.17.5");
    CHECK(split.vpn[0].label == "ZeroTier");
    CHECK(split.vpn[1].label == "Radmin VPN");
    // No card names (the system did not say): the address range, as before.
    const LanSplit bare = SplitLan({{"25.1.2.3", "", false}, {"192.168.0.9", "", false}, {"8.8.8.8", "", false}});
    REQUIRE(bare.vpn.size() == 1);
    CHECK(bare.vpn[0].label == "Hamachi");
    REQUIRE(bare.home.size() == 1);
    CHECK(bare.home[0].address == "192.168.0.9");
    // A card name it does not know (renamed, another language): the VPN range still decides. Linux's container and
    // VM bridges are left out.
    const LanSplit named = SplitLan({{"26.1.2.3", "Mein Netzwerk | Unbekannter Adapter", false},
                                     {"172.17.0.1", "docker0", false},
                                     {"192.168.122.1", "virbr0", false},
                                     {"192.168.1.30", "enp3s0", false}});
    REQUIRE(named.vpn.size() == 1);
    CHECK(named.vpn[0].label == "Radmin VPN");
    REQUIRE(named.home.size() == 1);
    CHECK(named.home[0].address == "192.168.1.30");

    const auto home = HomeLines(split.home, 27510, "pw", true);
    CHECK(home[0].find("home network") != std::string::npos);
    CHECK(home[1] == "192.168.1.20:27510 pw");
    CHECK(home[2].find("copied") != std::string::npos);
    CHECK(home[3] == "If that does not work, try another of this PC's addresses in its place: 192.168.56.1");
    const auto vpn = VpnLines(split.vpn, 27510, "pw", false);
    CHECK(vpn[0].find("ZeroTier") != std::string::npos);
    CHECK(vpn[1] == "10.147.17.5:27510 pw");
    CHECK(vpn[2].find("exactly as it is") != std::string::npos);
    CHECK(vpn[3] ==
          "If that does not work, try another of this PC's addresses in its place: 26.12.34.56 (Radmin VPN)");
    const auto no_vpn = VpnLines({}, 27510, "pw", false);
    CHECK(no_vpn[0] == "No VPN found.");
    CHECK(HomeLines({}, 27510, "pw", false)[0].find("No home-network address") != std::string::npos);
    CHECK(HostHeadline(true) ==
          "Invite code copied to your clipboard. Share it in DMs only: it contains your internet address.");
}

TEST_CASE("title menu: players on the map are on unless turned off: off writes 0, on takes the line away") {
    Options o;
    CHECK(o.map_players);
    CHECK(o.map_names);
    std::string c, opt;
    o.map_players = false;
    o.map_names = false;
    o.Write(c, opt);
    CHECK(IniGet(opt, "LICHII_COOP_MAP") == "0");
    CHECK(IniGet(opt, "LICHII_COOP_MAP_NAMES") == "0");
    const Options back = Options::Read("", opt);
    CHECK_FALSE(back.map_players);
    CHECK_FALSE(back.map_names);
    int seen = 0;
    for (const auto& [k, v] : o.Environment()) {
        if (k == "LICHII_COOP_MAP" || k == "LICHII_COOP_MAP_NAMES") {
            CHECK(v == "0");
            ++seen;
        }
    }
    CHECK(seen == 2);
    o.map_players = o.map_names = true;
    o.Write(c, opt);
    CHECK_FALSE(IniGet(opt, "LICHII_COOP_MAP").has_value());
    CHECK_FALSE(IniGet(opt, "LICHII_COOP_MAP_NAMES").has_value());
    CHECK(Options::Read("", "LICHII_COOP_MAP=1\r\n").map_players);
}

TEST_CASE("title menu: voice chat is off unless turned on: on writes open, ptt stays ptt, off takes the line away") {
    Options o;
    CHECK_FALSE(o.voice);
    CHECK_FALSE(o.voice_ptt);
    std::string c, opt = "LICHII_COOP_VOICE=ptt\r\n";
    o.Write(c, opt);
    CHECK_FALSE(IniGet(opt, "LICHII_COOP_VOICE").has_value());
    CHECK_FALSE(Options::Read("", opt).voice);
    // turned on: open mic
    o.voice = true;
    o.Write(c, opt);
    CHECK(IniGet(opt, "LICHII_COOP_VOICE") == "open");
    CHECK(Options::Read("", opt).voice);
    CHECK_FALSE(Options::Read("", opt).voice_ptt);
    // the Voice mode row's push to talk
    o.voice_ptt = true;
    o.Write(c, opt);
    CHECK(IniGet(opt, "LICHII_COOP_VOICE") == "ptt");
    // an older file's ptt (and on, 1) read back
    CHECK(Options::Read("", "LICHII_COOP_VOICE=ptt\r\n").voice_ptt);
    CHECK(Options::Read("", "LICHII_COOP_VOICE=PTT\r\n").VoiceMode() == "ptt");
    CHECK(Options::Read("", "LICHII_COOP_VOICE=on\r\n").VoiceMode() == "ptt");   // an older file's on: as it meant
    CHECK(Options::Read("", "LICHII_COOP_VOICE=1\r\n").voice_ptt);
    CHECK(Options::Read("", "LICHII_COOP_VOICE=open-mic\r\n").VoiceMode() == "open");
    CHECK(Options::Read("", "LICHII_COOP_VOICE=Open\r\n").VoiceMode() == "open");
    CHECK_FALSE(Options::Read("", "LICHII_COOP_VOICE=off\r\n").voice);
    // this run's variable: the mode, or unset for off
    auto env_of = [](const Options& x) {
        for (const auto& [k, v] : x.Environment()) {
            if (k == "LICHII_COOP_VOICE") {
                return v;
            }
        }
        return std::string("(none)");
    };
    CHECK(env_of(o) == "ptt");
    o.voice_ptt = false;
    CHECK(env_of(o) == "open");
    o.voice = false;
    CHECK(env_of(o).empty());
    // the push-to-talk key the Voice mode row names
    CHECK(Options{}.voice_key == "V");
    CHECK(Options::Read("", "LICHII_COOP_VOICE_KEY=b\r\n").voice_key == "B");
    CHECK(Options::Read("", "LICHII_COOP_VOICE_KEY=0x14\r\n").voice_key == "your key");
}

TEST_CASE("title menu: a fresh kit has voice off, and turning it on gives open mic") {
    for (const char* file : {"", "# LICHII_COOP_VOICE=open\r\n", "LICHII_COOP_VOICE=off\r\n"}) {
        CAPTURE(file);
        Options o = Options::Read("", file);
        CHECK_FALSE(o.voice);       // no line (or off): voice off
        CHECK_FALSE(o.voice_ptt);   // and no push to talk waiting
        o.voice = true;             // the menu's row turned on
        CHECK(o.VoiceMode() == "open");
        std::string c, opt = file;
        o.Write(c, opt);
        CHECK(IniGet(opt, "LICHII_COOP_VOICE") == "open");
        CHECK_FALSE(Options::Read("", opt).voice_ptt);
    }
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

// lobby-state-b (player report 22): a join is "Connected!" only once the session took this game in.
TEST_CASE("title menu: a join told to connect says joining until the session's welcome") {
    CHECK(ShownState(HOS_COOP_CONNECTED, false, false) == HOS_COOP_JOINING);
    CHECK(ShownState(HOS_COOP_CONNECTED, false, true) == HOS_COOP_CONNECTED);
    CHECK(ShownState(HOS_COOP_HOSTING, true, false) == HOS_COOP_HOSTING);
    CHECK(ShownState(HOS_COOP_JOINING, false, false) == HOS_COOP_JOINING);
    HostView h;
    h.state = HOS_COOP_CONNECTED;
    h.in_session = false;
    CHECK(PlainStatus(h) == std::vector<std::string>{"Joining your friend's game..."});
    h.refused = "the session is full";
    CHECK(PlainStatus(h)[0] == "Your friend's game turned this one away: the session is full.");
    CHECK(PlainStatus(h)[1] == "Press Leave, then join again.");
    // One row: only the part before ':' (the full reason is in the chat and the log); a ban offers no join again.
    h.refused = "the host banned you from this game: you cannot join it again until the host stops hosting";
    h.banned = true;
    CHECK(PlainStatus(h) ==
          std::vector<std::string>{"Your friend's game turned this one away: the host banned you from this game.",
                                   "Press Leave."});
    h.banned = false;
    h.refused.clear();
    h.in_session = true;
    CHECK(PlainStatus(h) == std::vector<std::string>{"Connected! Choose your file and play."});
}

TEST_CASE("title menu: a refusal's row keeps the part before the first ':'") {
    CHECK(ShortReason("the session is full") == "the session is full");
    CHECK(ShortReason("a: b: c") == "a");
    CHECK(RefusedLine("x: y") == "Your friend's game turned this one away: x.");
}

TEST_CASE("title menu: the voice's microphone in coop-options.ini") {
    CHECK(Options::Read("", "").voice_mic.empty());
    Options o = Options::Read("", "LICHII_COOP_VOICE=open\r\nLICHII_COOP_VOICE_MIC=Headset Microphone (USB)\r\n");
    CHECK(o.voice_mic == "Headset Microphone (USB)");
    // a name with " #" or " ;" in it is all of the name, not a comment (as the launcher reads the line)
    CHECK(Options::Read("", "LICHII_COOP_VOICE_MIC=Mic #2 ; left\r\n").voice_mic == "Mic #2 ; left");
    std::string c, opt;
    o.voice_mic = "Webcam";
    o.Write(c, opt);
    CHECK(IniGet(opt, "LICHII_COOP_VOICE_MIC") == "Webcam");
    o.voice = false;   // voice off: the microphone is kept for next time
    o.Write(c, opt);
    CHECK(IniGet(opt, "LICHII_COOP_VOICE_MIC") == "Webcam");
    o.voice_mic.clear();   // Default: no line
    o.Write(c, opt);
    CHECK_FALSE(IniGet(opt, "LICHII_COOP_VOICE_MIC").has_value());
    o.voice_mic = "Webcam";
    bool seen = false;
    for (const auto& [k, v] : o.Environment()) {
        seen |= k == "LICHII_COOP_VOICE_MIC" && v == "Webcam";
    }
    CHECK(seen);
}

TEST_CASE("title menu: a room or join code through a relay is an address, and the relay lines are read (issue 261)") {
    CHECK(AddressOk("forest-gang@relay.example.net"));
    CHECK(AddressOk("Forest_Gang@relay.example.net:27530"));
    CHECK(AddressOk("ABCD-EFGH@1.2.3.4"));
    CHECK_FALSE(AddressOk("ab@relay.example.net"));            // too short for a room
    CHECK_FALSE(AddressOk("forest gang@relay.example.net"));   // no spaces
    CHECK_FALSE(AddressOk("forest-gang@"));
    CHECK_FALSE(AddressOk("forest-gang@relay.example.net:0"));
    const auto t = ReadJoin("forest-gang@relay.example.net pw-room", CodeResult{});
    REQUIRE(std::holds_alternative<JoinAddress>(t));
    CHECK(std::get<JoinAddress>(t).server == "forest-gang@relay.example.net");
    CHECK(std::get<JoinAddress>(t).password == "pw-room");

    const Options o = Options::Read("", "relay = relay.example.net:27530\r\n# relay_room = old\r\nrelay_room = forest-gang \r\n");
    CHECK(o.relay == "relay.example.net:27530");
    CHECK(o.relay_key.empty());
    CHECK(o.relay_room == "forest-gang");
    // Read only: writing the options leaves the relay lines as they are.
    std::string coop, options = "relay = relay.example.net:27530\r\nrelay_room = forest-gang\r\n";
    o.Write(coop, options);
    CHECK(options.find("relay_room = forest-gang") != std::string::npos);
}

TEST_CASE("title menu: ParsePort and host port, 1 to 65535, read and written") {
    CHECK(ParsePort("27510") == std::uint16_t{27510});
    CHECK(ParsePort(" 1 ") == std::uint16_t{1});
    CHECK(ParsePort("65535") == std::uint16_t{65535});
    CHECK_FALSE(ParsePort("").has_value());
    CHECK_FALSE(ParsePort("0").has_value());
    CHECK_FALSE(ParsePort("65536").has_value());
    CHECK_FALSE(ParsePort("27510a").has_value());
    CHECK(AddressOk("192.168.1.50:28000"));
    CHECK_FALSE(AddressOk("192.168.1.50: 28000"));

    CHECK(Options::Read("", "").host_port == kDefaultPort);
    CHECK(Options::Read("host_port = 28000\r\n", "").host_port == 28000);
    CHECK(Options::Read("host_port = 28000\r\n", "server_port = 29000\r\n").host_port == 29000);
    CHECK(Options::Read("", "server_port = 99999\r\n").host_port == kDefaultPort);

    Options o;
    o.host_port = 28123;
    std::string coop, opt = "# server_port = 27510\r\n";
    o.Write(coop, opt);
    CHECK(IniGet(opt, "server_port") == "28123");
    CHECK(Options::Read("", opt).host_port == 28123);
    o.host_port = kDefaultPort;
    o.Write(coop, opt);
    CHECK_FALSE(IniGet(opt, "server_port").has_value());
}

TEST_CASE("title menu: the save list is re-read once after a switch, when the game's own test says idle") {
    using R = SaveListRefresh;
    R r;
    R::Input in;
    in.title = true;
    in.manager = true;
    in.busy = false;
    in.state = R::kRead;                     // the game's list after its boot read (state 4): idle
    in.now_ms = 1000;
    CHECK(r.Step(in) == R::Action::none);    // no switch: nothing asked
    CHECK(r.waiting() == R::Wait::none);
    in.stale = true;
    in.busy = true;                          // the game is reading already: wait for it
    in.state = 2;
    CHECK(r.Step(in) == R::Action::none);
    CHECK(r.pending());
    CHECK(r.waiting() == R::Wait::busy);
    in.stale = false;
    in.busy = false;
    in.state = R::kRead;
    CHECK(r.Step(in) == R::Action::ask);     // idle at state 4 (the case the first run missed): asked once
    CHECK(r.asked());
    in.state = 0;                            // reset, read not begun yet
    CHECK(r.Step(in) == R::Action::none);
    in.busy = true;
    in.state = 1;
    CHECK(r.Step(in) == R::Action::none);
    in.busy = false;
    in.state = R::kRead;
    CHECK(r.Step(in) == R::Action::done);    // read, idle again
    CHECK(r.Step(in) == R::Action::none);
}

TEST_CASE("title menu: the save list waits for the title and the manager, retries a refused reset, gives up") {
    using R = SaveListRefresh;
    R r;
    R::Input in;
    in.stale = true;
    in.busy = false;
    in.state = R::kRead;
    CHECK(r.Step(in) == R::Action::none);    // no title: kept
    CHECK(r.waiting() == R::Wait::title);
    in.stale = false;
    in.title = true;
    CHECK(r.Step(in) == R::Action::none);    // no manager yet
    CHECK(r.waiting() == R::Wait::manager);
    in.manager = true;
    CHECK(r.Step(in) == R::Action::ask);
    r.Refused();                             // the game's reset said not now (a save screen holds the list)
    CHECK(r.pending());
    CHECK(r.Step(in) == R::Action::ask);     // asked again
    in.state = 0;                            // never read
    in.now_ms = R::kGiveUpMs - 1;
    CHECK(r.Step(in) == R::Action::none);
    in.stale = true;                         // another switch meanwhile
    in.now_ms = R::kGiveUpMs;
    CHECK(r.Step(in) == R::Action::gave_up);
    in.stale = false;
    in.state = R::kRead;
    CHECK(r.Step(in) == R::Action::ask);     // the later switch: asked again
}

TEST_CASE("title menu: after a re-read the open title gets OpenTitle's buttons for the list (report 124)") {
    // OpenTitle.module (read from the game): with a save BtnNum stops at frame 3, Continue (0001), New Game (0000),
    // Options (0002) on Pa_Button_00..02, sound arrangement 1; without one frame 2, New Game and Options, type 0.
    const TitleArrangement with = ArrangementFor(true);
    CHECK(with.btn_num_frame == 3.0f);
    CHECK(with.sound_type == 1);
    REQUIRE(with.count == 3);
    const char* panes[] = {"Pa_Button_00/T_Btn_00", "Pa_Button_01/T_Btn_00", "Pa_Button_02/T_Btn_00"};
    const char* labels_with[] = {"0001", "0000", "0002"};
    for (std::size_t i = 0; i < with.count; ++i) {
        INFO("button " << i);
        CHECK(std::string(with.texts[i].pane) == panes[i]);
        CHECK(std::string(with.texts[i].label) == labels_with[i]);
    }
    const TitleArrangement without = ArrangementFor(false);
    CHECK(without.btn_num_frame == 2.0f);
    CHECK(without.sound_type == 0);
    REQUIRE(without.count == 2);
    const char* labels_without[] = {"0000", "0002"};
    for (std::size_t i = 0; i < without.count; ++i) {
        INFO("button " << i);
        CHECK(std::string(without.texts[i].pane) == panes[i]);
        CHECK(std::string(without.texts[i].label) == labels_without[i]);
    }
    CHECK(without.texts[2].pane == nullptr);   // (the third button is hidden by the frame: no text set)
    // The frame is the number of buttons shown, as the game's BtnNum animation counts them.
    CHECK(static_cast<std::size_t>(with.btn_num_frame) == with.count);
    CHECK(static_cast<std::size_t>(without.btn_num_frame) == without.count);
}

TEST_CASE("title menu: Skip title screen is free solo and held while co-op does anything (patch 0028, report 133)") {
    SkipTitleInput in;   // solo, co-op idle: Longshot's Skip title screen may choose
    CHECK(SkipTitleHold(in) == nullptr);
    for (const int state : {HOS_COOP_STARTING, HOS_COOP_HOSTING, HOS_COOP_JOINING, HOS_COOP_WAIT_HOST, HOS_COOP_CONNECTED,
                            HOS_COOP_FAILED}) {
        SkipTitleInput s;
        s.host_state = state;
        INFO("state " << state);
        CHECK(SkipTitleHold(s) != nullptr);
    }
    SkipTitleInput live;
    live.session_live = true;
    CHECK(std::string(SkipTitleHold(live)) == "a co-op session is live");
    SkipTitleInput coming;
    coming.save_coming = true;
    CHECK(std::string(SkipTitleHold(coming)) == "the host's save is on its way");
    SkipTitleInput wait;
    wait.save_wait = true;
    CHECK(std::string(SkipTitleHold(wait)) == "the host's save is on its way");
    SkipTitleInput host;
    host.host_state = HOS_COOP_HOSTING;
    CHECK(std::string(SkipTitleHold(host)) == "co-op is hosting");
    SkipTitleInput join;
    join.host_state = HOS_COOP_CONNECTED;
    CHECK(std::string(SkipTitleHold(join)) == "co-op is joining");
}

TEST_CASE("title menu: approve_joins (the host's) is read from coop-options.ini, off without a line, never rewritten") {
    CHECK_FALSE(Options::Read("", "").approve_joins);
    CHECK_FALSE(Options::Read("", "# approve_joins = on\r\n").approve_joins);
    CHECK_FALSE(Options::Read("", "approve_joins = off\r\n").approve_joins);
    CHECK(Options::Read("", "approve_joins = on\r\n").approve_joins);
    // No row edits it: the line stays as the player wrote it.
    const Options o = Options::Read("", "approve_joins = yes\r\n");
    CHECK(o.approve_joins);
    std::string coop, options = "approve_joins = yes\r\n";
    o.Write(coop, options);
    CHECK(IniGet(options, "approve_joins") == "yes");
}

TEST_CASE("title menu: auto_rejoin is off unless coop-options.ini turns it on, and the page's row writes it") {
    CHECK_FALSE(Options::Read("", "").auto_rejoin);
    CHECK(Options::Read("", "auto_rejoin = on\r\n").auto_rejoin);
    Options o = Options::Read("", "");
    o.auto_rejoin = true;
    std::string coop, options;
    o.Write(coop, options);
    CHECK(options.find("auto_rejoin = on") != std::string::npos);
    CHECK(Options::Read(coop, options).auto_rejoin);
    o.auto_rejoin = false;
    o.Write(coop, options);
    CHECK_FALSE(Options::Read(coop, options).auto_rejoin);
}

TEST_CASE("title menu: Start, Multiplayer, Options on the game's three, Multiplayer shows Host, Join, Co-op Settings, Back") {
    using CoopTitleMenu::MenuFor;
    using CoopTitleMenu::RowOf;
    using CoopTitleMenu::TitleDo;
    // With a save screen mod (one Start) and a save the game's three are Continue, New Game, Options: Start is
    // Continue's own press, Options its own.
    auto l = MenuFor(true, false, true);
    CHECK(l.btn_num_frame == 3);
    CHECK(l.count == 3);
    CHECK(std::string(l.rows[0].label) == "0103");
    CHECK(l.rows[0].action == TitleDo::kGame);
    CHECK(l.rows[1].action == TitleDo::kMultiplayer);
    CHECK(l.rows[2].action == TitleDo::kGame);
    CHECK(l.focus == -1);
    // Without one the game shows New Game and Options: Start is New Game's press, and the third row decides the
    // game's second button (Options) in its place.
    l = MenuFor(false, false, true);
    CHECK(l.btn_num_frame == 3);
    CHECK(std::string(l.rows[0].label) == "0103");
    CHECK(l.rows[0].action == TitleDo::kGame);
    CHECK(l.rows[2].action == TitleDo::kDecide);
    CHECK(l.rows[2].decide == 1);
    // The submenu: the game's fourth slot shown, all four the module's, the cursor on Host.
    for (const bool any : {true, false}) {
        for (const bool one : {true, false}) {
            l = MenuFor(any, true, one);
            CHECK(l.btn_num_frame == 4);
            CHECK(l.count == 4);
            CHECK(l.focus == 0);
            CHECK(l.rows[0].action == TitleDo::kHost);
            CHECK(l.rows[1].action == TitleDo::kJoin);
            CHECK(l.rows[2].action == TitleDo::kSettings);
            CHECK(l.rows[3].action == TitleDo::kBack);
        }
    }
    // Every label a row uses is one of the pack's, or the game's own Continue (0001) and New Game (0000).
    for (const bool any : {true, false}) {
        for (const bool sub : {true, false}) {
            for (const bool one : {true, false}) {
                const auto look = MenuFor(any, sub, one);
                for (std::size_t i = 0; i < look.count; ++i) {
                    bool known = std::string(look.rows[i].label) == "0000" || std::string(look.rows[i].label) == "0001";
                    for (const auto& [label, text] : CoopTitleMenu::kMenuLabels) known = known || std::string(label) == look.rows[i].label;
                    CHECK(known);
                }
                CHECK((sub ? RowOf(look, TitleDo::kBack) : RowOf(look, TitleDo::kMultiplayer)) >= 0);
            }
        }
    }
}

TEST_CASE("title menu: without a save screen mod, New Game stays reachable (playing alone)") {
    using CoopTitleMenu::MenuFor;
    using CoopTitleMenu::RowOf;
    using CoopTitleMenu::TitleDo;
    // A save: Continue and New Game each the game's own press (its buttons 0 and 1), Multiplayer on its third
    // (Options: the module's row, the A kept), Options in the fourth slot deciding the game's third button.
    auto l = MenuFor(true, false, false);
    CHECK(l.btn_num_frame == 4);
    REQUIRE(l.count == 4);
    CHECK(std::string(l.rows[0].label) == "0001");
    CHECK(l.rows[0].action == TitleDo::kGame);
    CHECK(std::string(l.rows[1].label) == "0000");
    CHECK(l.rows[1].action == TitleDo::kGame);
    CHECK(l.rows[2].action == TitleDo::kMultiplayer);
    CHECK(std::string(l.rows[3].label) == "0105");
    CHECK(l.rows[3].action == TitleDo::kDecide);
    CHECK(l.rows[3].decide == 2);
    CHECK(RowOf(l, TitleDo::kMultiplayer) == 2);   // (Back and B put the cursor there)
    // No save: New Game (the game's first), Multiplayer, Options deciding the game's second.
    l = MenuFor(false, false, false);
    CHECK(l.btn_num_frame == 3);
    REQUIRE(l.count == 3);
    CHECK(std::string(l.rows[0].label) == "0000");
    CHECK(l.rows[0].action == TitleDo::kGame);
    CHECK(l.rows[1].action == TitleDo::kMultiplayer);
    CHECK(l.rows[2].action == TitleDo::kDecide);
    CHECK(l.rows[2].decide == 1);
    CHECK(RowOf(l, TitleDo::kMultiplayer) == 1);
    CHECK(RowOf(l, TitleDo::kHost) == -1);
}

TEST_CASE("title menu: an A on the game's own rows and Options is the player's, the module's rows are not") {
    using CoopTitleMenu::MenuFor;
    using CoopTitleMenu::PlayersOwn;
    using CoopTitleMenu::TitleDo;
    CHECK_FALSE(PlayersOwn(nullptr));
    for (const bool any : {true, false}) {
        for (const bool one : {true, false}) {
            const auto top = MenuFor(any, false, one);
            for (std::size_t i = 0; i < top.count; ++i) {
                CHECK(PlayersOwn(&top.rows[i]) == (top.rows[i].action != TitleDo::kMultiplayer));
            }
            const auto sub = MenuFor(any, true, one);
            for (std::size_t i = 0; i < sub.count; ++i) {
                CHECK_FALSE(PlayersOwn(&sub.rows[i]));   // (Host in the first place too: not the game's Continue)
            }
        }
    }
}

TEST_CASE("title menu: list_room is off unless coop-options.ini turns it on, and it is written back") {
    using CoopTitleMenu::Options;
    CHECK_FALSE(Options::Read("", "").list_room);
    Options o = Options::Read("", "list_room = on\r\n");
    CHECK(o.list_room);
    std::string coop, options = "relay = relay.example:27600\r\n";
    o.list_room = true;
    o.Write(coop, options);
    CHECK(Options::Read(coop, options).list_room);
    o.list_room = false;
    o.Write(coop, options);
    CHECK_FALSE(Options::Read(coop, options).list_room);
    CHECK(options.find("relay = relay.example:27600") != std::string::npos);   // (the other lines kept)
}

namespace {
PlayerChoice::Pad TitlePad(std::uint64_t buttons, int focused, bool coop_row = false) {
    PlayerChoice::Pad p;
    p.buttons = buttons;
    p.title_up = true;
    p.focused = focused;
    p.on_coop_row = coop_row;
    return p;
}
constexpr std::uint64_t kPadA = PlayerChoice::kA;
}  // namespace

TEST_CASE("title menu: the player's own A drops the pending switch, on the A's edge only") {
    // (seen before a code mod's hold: Save Profiles' file select takes this A, so it is the only place it shows)
    PlayerChoice c;
    CHECK(c.Step(1000, TitlePad(kPadA, 0)).picks_file);
    CHECK_FALSE(c.Step(1016, TitlePad(kPadA, 0)).picks_file);   // held: no new edge
    CHECK_FALSE(c.Step(1032, TitlePad(0, 0)).picks_file);
    CHECK(c.Step(1048, TitlePad(kPadA, 2)).picks_file);         // Options or New Game: the player's own too
    c.Step(1064, TitlePad(0, 0));
    CHECK_FALSE(c.Step(1080, TitlePad(kPadA, 4, true)).picks_file);   // Host / Join / Settings: our menu
    c.Step(1096, TitlePad(0, 0));
    CHECK_FALSE(c.Step(1112, TitlePad(kPadA, -1)).picks_file);  // no button focused
    c.Step(1128, TitlePad(0, 0));
    PlayerChoice::Pad off = TitlePad(kPadA, 0);
    off.title_up = false;
    CHECK_FALSE(c.Step(1144, off).picks_file);                  // not the title
    c.Step(1160, TitlePad(0, 0));
    CHECK_FALSE(c.Step(1176, TitlePad(1ull << 1, 0)).picks_file);   // B is not a choice
}

TEST_CASE("title menu: the player's choice window: only the first button, kWindowMs, renewed by a later A") {
    PlayerChoice c;
    CHECK_FALSE(c.Lately(500));
    c.Step(1000, TitlePad(kPadA, 2));   // Options: drops the switch but is no choice of a file to load
    CHECK_FALSE(c.Lately(1001));
    c.Step(1016, TitlePad(0, 2));
    c.Step(1032, TitlePad(kPadA, 4, true));   // a co-op row in the first place (the title's own menu: Host)
    CHECK_FALSE(c.Lately(1033));
    c.Step(1048, TitlePad(0, 0));
    const auto seen = c.Step(2000, TitlePad(kPadA, 0));
    CHECK(seen.window_opened);
    CHECK(c.Lately(2000));
    CHECK(c.Lately(2000 + PlayerChoice::kWindowMs - 1));
    CHECK_FALSE(c.Lately(2000 + PlayerChoice::kWindowMs));
    c.Step(3000, TitlePad(0, 0));
    CHECK_FALSE(c.Step(4000, TitlePad(kPadA, 0)).window_opened);   // (inside the window: renewed, not new)
    CHECK(c.Lately(4000 + PlayerChoice::kWindowMs - 1));
    PlayerChoice z;
    z.Step(0, TitlePad(kPadA, 0));   // a clock at 0 still counts
    CHECK(z.Lately(0));
}

TEST_CASE("title menu: the player's choice window ends in the game, on a join and on Clear") {
    PlayerChoice c;
    c.Step(1000, TitlePad(kPadA, 0));
    PlayerChoice::Pad away = TitlePad(kPadA, 0);   // (Save Profiles' file select over the title, A still held)
    away.title_up = false;
    c.Step(1100, away);
    CHECK(c.Lately(1100));   // the title out of sight alone does not end it
    PlayerChoice::Pad in_game = TitlePad(0, -1);
    in_game.title_up = false;
    in_game.in_game = true;
    c.Step(5000, in_game);   // the load came: a return to the title later is a new choice
    CHECK_FALSE(c.Lately(5001));
    c.Step(9000, TitlePad(kPadA, 0));
    CHECK(c.Lately(9001));
    c.Clear();               // a join begun after the A, or the host's save offered again (backed out of the pick)
    CHECK_FALSE(c.Lately(9002));
}

TEST_CASE("title menu: an A on Host in the Multiplayer submenu (the game's Continue place) opens no choice window") {
    // (CoopTitle::OnCoopRow: in the title's own menu, a row that is not the player's own is a co-op row)
    using CoopTitleMenu::MenuFor;
    using CoopTitleMenu::PlayersOwn;
    for (const bool one : {true, false}) {
        PlayerChoice c;
        const auto sub = MenuFor(true, true, one);
        const auto seen = c.Step(1000, TitlePad(kPadA, 0, !PlayersOwn(&sub.rows[0])));
        CHECK_FALSE(seen.picks_file);
        CHECK_FALSE(c.Lately(1001));
        c.Step(1016, TitlePad(0, 0));
        const auto top = MenuFor(true, false, one);
        CHECK(c.Step(1032, TitlePad(kPadA, 0, !PlayersOwn(&top.rows[0]))).window_opened);   // Continue / Start
        c.Clear();
        c.Step(1048, TitlePad(0, 1));
        const int multiplayer = CoopTitleMenu::RowOf(top, CoopTitleMenu::TitleDo::kMultiplayer);
        CHECK_FALSE(c.Step(1064, TitlePad(kPadA, multiplayer, true)).picks_file);
    }
}
