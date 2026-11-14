// SPDX-License-Identifier: GPL-3.0-or-later
// Runs OoTCoop.js's Game.Play for three players (two controllers, one keyboard) against a stand-in of Nucleus's
// Context and the .NET calls it makes, on a made-up kit in %TEMP%, and checks the files it writes.
// Windows only, no install:   cscript //nologo //E:jscript tools\nucleus\test_handler.js
// Exit code 0 when every check passes.

var fso = new ActiveXObject("Scripting.FileSystemObject");
var shell = new ActiveXObject("WScript.Shell");
var here = fso.GetParentFolderName(WScript.ScriptFullName);
var failures = 0, checks = 0;

function check(ok, what) {
    checks++;
    if (!ok) {
        failures++;
        WScript.Echo("FAIL: " + what);
    }
}
function readFile(p) {
    if (!fso.FileExists(p)) return "";
    var f = fso.OpenTextFile(p, 1);
    var t = f.AtEndOfStream ? "" : f.ReadAll();
    f.Close();
    return t;
}
function writeFile(p, t) {
    var f = fso.CreateTextFile(p, true);
    f.Write(t);
    f.Close();
}
function mkdirs(p) {
    if (!p || fso.FolderExists(p)) return;
    mkdirs(fso.GetParentFolderName(p));
    fso.CreateFolder(p);
}

// The .NET calls the handler uses, on the real file system.
var startedServers = [];
var attrMode = "number";   // how the DirectoryInfo stand-in reports FileAttributes (below)
var System = {
    IO: {
        Path: {
            Combine: function (a, b) { return fso.BuildPath(a, b); },
            GetDirectoryName: function (p) { return fso.GetParentFolderName(p); }
        },
        File: {
            Exists: function (p) { return fso.FileExists(p); },
            ReadAllText: function (p) { return readFile(p); },
            WriteAllText: function (p, t) { writeFile(p, t); }
        },
        Directory: {
            Exists: function (p) { return fso.FolderExists(p); },
            CreateDirectory: function (p) { mkdirs(p); },
            Move: function (a, b) { fso.MoveFolder(a, b); }
        },
        // FileAttributes as Jint may hand it over: a number (Directory 16, ReparsePoint 1024), or an object with
        // ToString (and valueOf: the number) or the enum's text, as attrMode says.
        DirectoryInfo: function (p) {
            var link = (fso.GetFolder(p).Attributes & 1024) !== 0, n = link ? 1040 : 16;
            var text = link ? "Directory, ReparsePoint" : "Directory";
            if (attrMode === "number") this.Attributes = n;
            else if (attrMode === "text") this.Attributes = text;
            else this.Attributes = { ToString: function () { return text; } };
        }
    },
    Environment: { GetEnvironmentVariable: function (n) { return shell.ExpandEnvironmentStrings("%" + n + "%"); } },
    Guid: {
        n: 0,
        NewGuid: function () {
            var k = ++System.Guid.n;
            return { ToString: function () { return "0123456789abcdef0123456789abcd" + (k < 10 ? "0" : "") + k; } };
        }
    },
    Threading: { Thread: { Sleep: function () { } } }
};

// A made-up kit: <root>\kit\server\coop_server.exe, the game in <root>\kit\hookshot\game, three instance folders.
var root = fso.BuildPath(shell.ExpandEnvironmentStrings("%TEMP%"), "oot-nucleus-test-" + new Date().getTime());
var kit = fso.BuildPath(root, "kit"), game = fso.BuildPath(kit, "hookshot\\game");
mkdirs(fso.BuildPath(kit, "server"));
writeFile(fso.BuildPath(kit, "server\\coop_server.exe"), "");
writeFile(fso.BuildPath(kit, "coop-options.ini"),
          "name = Saria\r\ncolor = red\r\nLICHII_COOP_PUPPET=1\r\nLICHII_COOP_PAD=2\r\n# LICHII_COOP_KEYBOARD=off\r\n");
mkdirs(fso.BuildPath(game, "user\\save"));
writeFile(fso.BuildPath(game, "user\\save\\host.sav"), "x");
var instances = [];
for (var i = 0; i < 3; i++) {
    instances[i] = fso.BuildPath(root, "content\\OoTCoopLongshot\\Instance" + i);
    mkdirs(instances[i]);
    writeFile(fso.BuildPath(instances[i], "longshot_options.ini"), "fullscreen=1\r\nvsync=0\r\ndiscord_status=1\r\n");
}
// Instance 1 has a real user folder from an earlier start (Nucleus copied it): kept beside, renamed.
mkdirs(fso.BuildPath(instances[1], "user"));

var Game = { Hook: {} };
var Context = {
    Log: function (s) { WScript.Echo("log: " + s); },
    StartProcess: function (path, args) {
        if (/cmd\.exe$/i.test(path)) {
            shell.Run("\"" + path + "\" " + args, 0, true);
        } else {
            startedServers.push(path + " " + args);
        }
        return true;
    }
};

eval(readFile(fso.BuildPath(here, "OoTCoop.js")));

var players = [
    { nick: "Saria", pad: 2, keyboard: false },
    { nick: "A name that is far too long", pad: 0, keyboard: false },   // Nucleus gave no pad number
    { nick: "", pad: 0, keyboard: true }
];
for (var p = 0; p < players.length; p++) {
    Context.PlayerID = p;
    Context.RootInstallFolder = game;
    Context.ExePath = fso.BuildPath(instances[p], "longshot.exe");
    Context.Nickname = players[p].nick;
    Context.GamepadId = players[p].pad;
    Context.IsKeyboardPlayer = players[p].keyboard;
    Game.Play();
}

check(startedServers.length === 1, "one server started: " + startedServers.join(" | "));
check(/coop_server\.exe --port 27590 --bind 127\.0\.0\.1 --max-players 4 --password 0123456789abcdef0123456789abcd01$/
          .test(startedServers[0]), "server command line: " + startedServers[0]);
var pw = "0123456789abcdef0123456789abcd01";
for (var q = 0; q < 3; q++) {
    var ini = readFile(fso.BuildPath(instances[q], "coop.ini"));
    check(ini.indexOf("server = 127.0.0.1\r\n") >= 0 && ini.indexOf("port = 27590\r\n") >= 0, "coop.ini " + q + " server");
    check(ini.indexOf("password = " + pw + "\r\n") >= 0, "coop.ini " + q + " password: " + ini);
    var link = fso.BuildPath(instances[q], "user");
    check(fso.FolderExists(link) && (fso.GetFolder(link).Attributes & 1024) !== 0, "user " + q + " is a link");
    var ls = readFile(fso.BuildPath(instances[q], "longshot_options.ini"));
    check(ls.indexOf("fullscreen=0") >= 0 && ls.indexOf("fullscreen=1") < 0 && ls.indexOf("discord_status=0") >= 0 &&
          ls.indexOf("vsync=0") >= 0, "longshot_options.ini " + q + ": " + ls);
    check(readFile(fso.BuildPath(instances[q], "player-key.txt")).length >= 32, "player-key.txt " + q);
}
check(readFile(fso.BuildPath(instances[0], "coop.ini")).indexOf("name = Saria\r\n") >= 0, "player 1 name");
check(readFile(fso.BuildPath(instances[1], "coop.ini")).indexOf("name = A name that is f\r\n") >= 0, "name cut to 16");
check(readFile(fso.BuildPath(instances[2], "coop.ini")).indexOf("name = Player 3\r\n") >= 0, "empty name: Player 3");
check(fso.FileExists(fso.BuildPath(instances[0], "user\\save\\host.sav")), "player 1 plays from the game's own saves");
check(fso.FolderExists(fso.BuildPath(game, "nucleus-players\\player2\\user")), "player 2's saves beside the game");
check(!fso.FileExists(fso.BuildPath(instances[1], "user\\save\\host.sav")), "player 2 has its own saves");
var kept = fso.GetFolder(instances[1]).SubFolders, keptOld = false;
for (var e = new Enumerator(kept); !e.atEnd(); e.moveNext()) keptOld = keptOld || /^user\.nucleus-\d+$/.test(e.item().Name);
check(keptOld, "the real user folder of an earlier start kept, renamed");
var k1 = readFile(fso.BuildPath(instances[0], "player-key.txt")), k2 = readFile(fso.BuildPath(instances[1], "player-key.txt"));
check(k1 !== k2, "each player has its own key");

var o1 = readFile(fso.BuildPath(instances[0], "coop-options.ini"));
var o2 = readFile(fso.BuildPath(instances[1], "coop-options.ini"));
var o3 = readFile(fso.BuildPath(instances[2], "coop-options.ini"));
check(o1.indexOf("LICHII_COOP_PAD=2\r\n") >= 0 && o1.indexOf("LICHII_COOP_KEYBOARD=off") >= 0, "player 1 pad 2: " + o1);
check(o1.indexOf("color = red") >= 0 && o1.indexOf("LICHII_COOP_PUPPET=1") >= 0, "player 1 keeps the kit's switches");
check(o2.indexOf("LICHII_COOP_PAD=2\r\n") >= 0 && o2.indexOf("color") < 0, "player 2: 2nd controller player, no colour: " + o2);
check(o2.split("LICHII_COOP_PAD=").length === 2, "one pad line");
check(o3.indexOf("LICHII_COOP_PAD=none") >= 0 && o3.indexOf("LICHII_COOP_KEYBOARD=focus") >= 0, "keyboard player: " + o3);

// A script run anew for a later player (no session in memory): the password from nucleus-players\session.txt.
coopSession = null;
Context.PlayerID = 1;
Context.ExePath = fso.BuildPath(instances[1], "longshot.exe");
Context.Nickname = "Mido";
Context.GamepadId = 3;
Context.IsKeyboardPlayer = false;
Game.Play();
check(readFile(fso.BuildPath(instances[1], "coop.ini")).indexOf("password = " + pw + "\r\n") >= 0, "password from session.txt");
check(readFile(fso.BuildPath(instances[1], "coop-options.ini")).indexOf("LICHII_COOP_PAD=3\r\n") >= 0, "Nucleus's pad 3");

// Runs again (the next session): links stay, a new password.
Context.PlayerID = 0;
Context.ExePath = fso.BuildPath(instances[0], "longshot.exe");
Context.GamepadId = 1;
Context.IsKeyboardPlayer = false;
Game.Play();
check(readFile(fso.BuildPath(instances[0], "coop.ini")).indexOf("password = 0123456789abcdef0123456789abcd") >= 0 &&
      readFile(fso.BuildPath(instances[0], "coop.ini")).indexOf(pw) < 0, "a new password for a new session");
check(fso.FileExists(fso.BuildPath(instances[0], "user\\save\\host.sav")), "player 1's link kept");
var renamed = false;
for (var e0 = new Enumerator(fso.GetFolder(instances[0]).SubFolders); !e0.atEnd(); e0.moveNext()) {
    renamed = renamed || /^user\.nucleus-/.test(e0.item().Name);
}
check(!renamed, "a rerun keeps the working link (none renamed aside)");

// coopIsLink with each form the attributes can come in: a number (the default above), the enum's text, a wrapper.
var junction = fso.BuildPath(instances[0], "user"), plain = fso.BuildPath(game, "user");
var modes = ["number", "text", "wrapper"];
for (var m = 0; m < modes.length; m++) {
    attrMode = modes[m];
    check(coopIsLink(junction) === true && coopIsLink(plain) === false, "coopIsLink with attributes as " + attrMode);
}
attrMode = "number";

// Clean up: the links first (rmdir removes a link, not what it names), then the rest.
for (var r = 0; r < 3; r++) shell.Run("cmd /c rmdir \"" + fso.BuildPath(instances[r], "user") + "\"", 0, true);
check(fso.FileExists(fso.BuildPath(game, "user\\save\\host.sav")), "removing the link left the saves");
fso.DeleteFolder(root, true);

WScript.Echo(failures === 0 ? "ok: " + checks + " checks" : failures + " of " + checks + " checks failed");
WScript.Quit(failures === 0 ? 0 : 1);
