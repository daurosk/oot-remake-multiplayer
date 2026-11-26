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
var deleted = [];   // the folders Directory.Delete was given
var attrMode = "number";   // how the DirectoryInfo stand-in reports FileAttributes (below)
var System = {
    IO: {
        Path: {
            Combine: function (a, b) { return fso.BuildPath(a, b); },
            GetDirectoryName: function (p) { return fso.GetParentFolderName(p); },
            GetFileName: function (p) { return fso.GetFileName(p); }
        },
        File: {
            Exists: function (p) { return fso.FileExists(p); },
            ReadAllText: function (p) { return readFile(p); },
            WriteAllText: function (p, t) { writeFile(p, t); },
            Delete: function (p) { if (fso.FileExists(p)) fso.DeleteFile(p); },
            Copy: function (a, b, over) { fso.CopyFile(a, b, over); }
        },
        Directory: {
            Exists: function (p) { return fso.FolderExists(p); },
            CreateDirectory: function (p) { mkdirs(p); },
            Move: function (a, b) { fso.MoveFolder(a, b); },
            GetDirectories: function (p) { var r = [], e = new Enumerator(fso.GetFolder(p).SubFolders); for (; !e.atEnd(); e.moveNext()) r.push(e.item().Path); return r; },
            GetFiles: function (p) { var r = [], e = new Enumerator(fso.GetFolder(p).Files); for (; !e.atEnd(); e.moveNext()) r.push(e.item().Path); return r; },
            // (rmdir without /s: a link goes, not what it names; a folder with files in it would stay)
            Delete: function (p, recursive) {
                deleted.push(p);
                shell.Run("cmd /c rmdir \"" + p + "\"", 0, true);
            }
        },
        // FileAttributes as Jint may hand it over: a number (Directory 16, ReparsePoint 1024), or an object with
        // ToString (and valueOf: the number) or the enum's text, as attrMode says.
        FileInfo: function (p) {
            var f = fso.GetFile(p);
            this.Length = f.Size;
            this.LastWriteTimeUtc = String(f.DateLastModified);
            this.Attributes = f.Attributes;
        },
        DirectoryInfo: function (p) {
            var link = (fso.GetFolder(p).Attributes & 1024) !== 0, n = link ? 1040 : 16;
            var text = link ? "Directory, ReparsePoint" : "Directory";
            if (attrMode === "number") this.Attributes = n;
            else if (attrMode === "text") this.Attributes = text;
            else this.Attributes = { ToString: function () { return text; } };
        }
    },
    Environment: {
        vars: {},
        GetEnvironmentVariable: function (n) {
            if (System.Environment.vars.hasOwnProperty(n)) return System.Environment.vars[n];
            return shell.ExpandEnvironmentStrings("%" + n + "%");
        },
        SetEnvironmentVariable: function (n, v) {
            if (v === null) delete System.Environment.vars[n]; else System.Environment.vars[n] = v;
        }
    },
    Guid: {
        n: 0,
        NewGuid: function () {
            var k = ++System.Guid.n;
            return { ToString: function () { return "0123456789abcdef0123456789abcd" + (k < 10 ? "0" : "") + k; } };
        }
    },
    Threading: { Thread: { Sleep: function () { } } },
    // coop_server.exe processes running now (running), as a CLR array comes over (Length).
    Diagnostics: {
        Process: {
            running: 0,
            GetProcessesByName: function (n) { return { Length: n === "coop_server" ? System.Diagnostics.Process.running : 0 }; }
        }
    }
};

// A made-up kit: <root>\kit\server\coop_server.exe and start-server.cmd, the game in <root>\kit\hookshot\game, three
// instance folders.
var root = fso.BuildPath(shell.ExpandEnvironmentStrings("%TEMP%"), "oot-nucleus-test-" + new Date().getTime());
var kit = fso.BuildPath(root, "kit"), game = fso.BuildPath(kit, "hookshot\\game");
mkdirs(fso.BuildPath(kit, "server"));
writeFile(fso.BuildPath(kit, "server\\coop_server.exe"), "");
writeFile(fso.BuildPath(kit, "start-server.cmd"), "");
writeFile(fso.BuildPath(kit, "coop-options.ini"),
          "name = Saria\r\ncolor = red\r\nLICHII_COOP_PUPPET=1\r\nLICHII_COOP_PAD=2\r\n# LICHII_COOP_KEYBOARD=off\r\n");
mkdirs(fso.BuildPath(game, "user\\save"));
writeFile(fso.BuildPath(game, "user\\save\\host.sav"), "x");
mkdirs(fso.BuildPath(game, "ngx"));                       // DLSS's runtime, built in after the instances were made
writeFile(fso.BuildPath(game, "ngx\\nvngx_dlss.dll"), "dlss");
mkdirs(fso.BuildPath(game, "logs"));
writeFile(fso.BuildPath(game, "newfile.dll"), "new");
writeFile(fso.BuildPath(game, "exefs_patches.txt"), "built with");
mkdirs(fso.BuildPath(game, "texts"));                     // a folder a later build added
writeFile(fso.BuildPath(game, "texts\en.txt"), "words");
mkdirs(fso.BuildPath(game, "cache"));                     // the solo game's shader cache: each instance keeps its own
writeFile(fso.BuildPath(game, "language.txt"), "solo");
writeFile(fso.BuildPath(game, "coop.ini"), "own");
writeFile(fso.BuildPath(game, "coop-saves.ini"), "solo");
writeFile(fso.BuildPath(game, "coop-name-checked.txt"), "solo");
// (written by the solo game after the instances were made: must stay each instance's own)
var instances = [];
for (var i = 0; i < 3; i++) {
    instances[i] = fso.BuildPath(root, "content\\OoTCoopLongshot\\Instance" + i);
    mkdirs(instances[i]);
    writeFile(fso.BuildPath(instances[i], "longshot_options.ini"),
              "fullscreen=1\r\nvsync=0\r\ndiscord_status=1\r\nmouse_camera=1\r\n");
}
// The game's logs, which an older handler's instance folders linked (instance 0): the link goes, the logs stay.
// Instance 2 has its own logs folder already: kept.
mkdirs(fso.BuildPath(game, "logs"));
writeFile(fso.BuildPath(game, "logs\\longshot_log.txt"), "the game's own log");
shell.Run("cmd /c mklink /J \"" + fso.BuildPath(instances[0], "logs") + "\" \"" + fso.BuildPath(game, "logs") + "\"", 0, true);
mkdirs(fso.BuildPath(instances[2], "logs"));
writeFile(fso.BuildPath(instances[2], "logs\\longshot_log.txt"), "player 3's log");
// Instance 1 holds an old copy of a build file, and a file an older build had that the game folder no longer has.
writeFile(fso.BuildPath(instances[1], "exefs_patches.txt"), "old build");
writeFile(fso.BuildPath(instances[1], "gone.dll"), "old");
writeFile(fso.BuildPath(instances[1], "coop-linked.txt"), "gone.dll\r\n");
writeFile(fso.BuildPath(instances[1], "language.txt"), "player 2's");
// What each game writes beside its exe: its log, the caches it writes in place (Game.FileSymlinkCopyInstead) and
// the big one it writes through a .tmp over Nucleus's link. The game folder has its own, with other bytes: kept.
writeFile(fso.BuildPath(game, "longshot_log.txt"), "the solo game's log");
writeFile(fso.BuildPath(game, "longshot_pipeline_states.bin"), "solo states");
writeFile(fso.BuildPath(game, "longshot_pipelines.bin"), "solo pipelines");
writeFile(fso.BuildPath(game, "longshot_pipeline_states_shipped.bin"), "shipped, new build");
writeFile(fso.BuildPath(instances[1], "longshot_log.txt"), "player 2's log");
writeFile(fso.BuildPath(instances[1], "longshot_pipeline_states.bin"), "player 2's states");
writeFile(fso.BuildPath(instances[1], "longshot_pipelines.bin"), "player 2's pipelines");
writeFile(fso.BuildPath(instances[1], "longshot_pipeline_states_shipped.bin"), "shipped, an older build");
// Instance 1 has a real user folder from an earlier start (Nucleus copied it): kept beside, renamed.
mkdirs(fso.BuildPath(instances[1], "user"));

var Game = {
    Hook: {},
    Options: [],
    AddOption: function (name, desc, key, values, def) {
        Game.Options.push({ name: name, desc: desc, key: key, values: values, def: def });
    }
};
var startedWindows = [];   // cmd /c start ...: what ran, and OOT_COOP_SESSION_PASSWORD at that moment
var failStart = false;     // the next cmd /c start throws (Process.Start failing in Nucleus)
var logged = [];
var Context = {
    Log: function (s) { logged.push(s); },
    StartProcess: function (path, args) {
        if (/cmd\.exe$/i.test(path) && /^\/c start /.test(args)) {
            startedWindows.push({ args: args, password: System.Environment.vars.OOT_COOP_SESSION_PASSWORD });
            if (failStart) { failStart = false; throw new Error("Process.Start failed"); }
        } else if (/cmd\.exe$/i.test(path)) {
            shell.Run("\"" + path + "\" " + args, 0, true);
        } else {
            startedServers.push(path + " " + args);
        }
        return true;
    }
};

eval(readFile(fso.BuildPath(here, "OoTCoop.js")));

// Nucleus lists controllers in its setup screen only for a handler with XInput (or DirectInput) on
// (DevicesFunctions.GamepadTimer_Tick); its input DLLs come only with the other switches, which stay off.
check(Game.Hook.XInputEnabled === true && Game.Hook.XInputReroute === false && Game.Hook.DInputEnabled === false,
      "Nucleus lists the XInput pads, no DirectInput or reroute");
check(Game.Hook.CustomDllEnabled === false && !Game.UseX360ce && !Game.XInputPlusDll && !Game.ProtoInput,
      "no Nucleus input DLLs");

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

// The server: the kit's start-server.cmd in its own window, once (player 1), never coop_server.exe directly. No kit
// coop.ini: Host.bat's port 27510.
var comspec = shell.ExpandEnvironmentStrings("%ComSpec%");
check(startedServers.length === 0, "coop_server not started directly: " + startedServers.join(" | "));
check(startedWindows.length === 1 && startedWindows[0].args === "/c start \"Squadala server\" /d \"" + kit + "\" \"" +
      comspec + "\" /c \"\"" + fso.BuildPath(kit, "start-server.cmd") + "\" 27510\"",
      "one server window, start-server.cmd on port 27510: " + (startedWindows.length ? startedWindows[0].args : ""));
var pw = "0123456789abcdef0123456789abcd01";
check(startedWindows.length === 1 && startedWindows[0].password === pw, "the session password handed over");
for (var q = 0; q < 3; q++) {
    var ini = readFile(fso.BuildPath(instances[q], "coop.ini"));
    check(ini.indexOf("server = 127.0.0.1\r\nport = 27510\r\n") >= 0, "coop.ini " + q + " joins the server: " + ini);
    check(ini.indexOf("password = " + pw + "\r\n") >= 0, "coop.ini " + q + " password: " + ini);
    var link = fso.BuildPath(instances[q], "user");
    check(fso.FolderExists(link) && (fso.GetFolder(link).Attributes & 1024) !== 0, "user " + q + " is a link");
    var ls = readFile(fso.BuildPath(instances[q], "longshot_options.ini"));
    check(ls.indexOf("fullscreen=0") >= 0 && ls.indexOf("fullscreen=1") < 0 && ls.indexOf("discord_status=0") >= 0 &&
          ls.indexOf("vsync=0") >= 0, "longshot_options.ini " + q + ": " + ls);
    check(readFile(fso.BuildPath(instances[q], "player-key.txt")).length >= 32, "player-key.txt " + q);
}
check(readFile(fso.BuildPath(instances[0], "ngx\\nvngx_dlss.dll")) === "dlss" && (fso.GetFolder(fso.BuildPath(instances[0], "ngx")).Attributes & 1024) !== 0, "ngx folder linked into an instance made before it");
check(readFile(fso.BuildPath(instances[1], "newfile.dll")) === "new", "a new game DLL copied in");
check(readFile(fso.BuildPath(instances[1], "exefs_patches.txt")) === "built with", "the build's mod code list copied in");
check(readFile(fso.BuildPath(instances[0], "texts\en.txt")) === "words", "a folder a later build added is linked in");
check(!fso.FolderExists(fso.BuildPath(instances[0], "cache")), "the game's cache is not taken");
check(!fso.FileExists(fso.BuildPath(instances[1], "gone.dll")), "a copied file the game no longer has is removed");
check(readFile(fso.BuildPath(instances[1], "language.txt")) === "player 2's", "a player's own setting is kept");
check(readFile(fso.BuildPath(instances[1], "longshot_log.txt")) === "player 2's log", "a player's own log is kept");
check(readFile(fso.BuildPath(instances[1], "longshot_pipeline_states.bin")) === "player 2's states" &&
      readFile(fso.BuildPath(instances[1], "longshot_pipelines.bin")) === "player 2's pipelines", "a player's own shader caches are kept");
check(readFile(fso.BuildPath(instances[1], "longshot_pipeline_states_shipped.bin")) === "shipped, new build", "the build's shipped pipeline states are brought up to date");
check(!fso.FileExists(fso.BuildPath(instances[0], "longshot_log.txt")) && !fso.FileExists(fso.BuildPath(instances[0], "longshot_pipelines.bin")), "a game's log and caches are not copied into an instance without them");
check(!fso.FileExists(fso.BuildPath(instances[1], "coop-saves.ini")) && !fso.FileExists(fso.BuildPath(instances[1], "coop-name-checked.txt")), "per-player files the solo game wrote are not taken");
check(!fso.FolderExists(fso.BuildPath(instances[0], "logs")), "logs stay the instance's own");
var noted = readFile(fso.BuildPath(game, "nucleus-players\\instances.txt"));
check(noted === "1=" + instances[0] + "\r\n2=" + instances[1] + "\r\n3=" + instances[2] + "\r\n", "instances.txt: " + noted);
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

// Each game its own logs: Nucleus links no logs folder, and an old link is removed (the logs it named kept).
var excluded = false;
for (var x = 0; x < Game.DirExclusions.length; x++) excluded = excluded || Game.DirExclusions[x] === "logs";
check(excluded, "Nucleus does not link the logs folder");
check(!fso.FolderExists(fso.BuildPath(instances[0], "logs")), "player 1's old logs link removed");
check(readFile(fso.BuildPath(game, "logs\\longshot_log.txt")) === "the game's own log", "the game's logs kept");
check(readFile(fso.BuildPath(instances[2], "logs\\longshot_log.txt")) === "player 3's log", "a real logs folder kept");
check(deleted.length === 1, "only the link deleted: " + deleted.join(" | "));

// The mouse camera: a controller player's is "As the stick" (0), never Native; a keyboard player keeps its own.
var m1 = readFile(fso.BuildPath(instances[0], "longshot_options.ini"));
var m2 = readFile(fso.BuildPath(instances[1], "longshot_options.ini"));
var m3 = readFile(fso.BuildPath(instances[2], "longshot_options.ini"));
check(m1.indexOf("mouse_camera=0") >= 0 && m1.indexOf("mouse_camera=1") < 0, "player 1 (pad): mouse camera off: " + m1);
check(m2.indexOf("mouse_camera=0") >= 0 && m2.indexOf("mouse_camera=1") < 0, "player 2 (pad): mouse camera off: " + m2);
check(m3.indexOf("mouse_camera=1") >= 0 && m3.indexOf("mouse_camera=0") < 0, "player 3 (keyboard) keeps it: " + m3);

// The settings: the kit's coop-options.ini for every game (coop-kit.txt names the kit), each game's own lines in its
// coop-player.ini (controller, keyboard, look), no full copy of its own (it went stale: known issue up to 0.11.12).
var o1 = readFile(fso.BuildPath(instances[0], "coop-player.ini"));
var o2 = readFile(fso.BuildPath(instances[1], "coop-player.ini"));
var o3 = readFile(fso.BuildPath(instances[2], "coop-player.ini"));
for (var oi = 0; oi < 3; oi++) {
    check(!fso.FileExists(fso.BuildPath(instances[oi], "coop-options.ini")), "instance " + oi + ": no copy of the options");
    check(readFile(fso.BuildPath(instances[oi], "coop-kit.txt")) === kit + "\r\n", "instance " + oi + ": coop-kit.txt names the kit");
    check(readFile(fso.BuildPath(instances[oi], "longshot_options.ini")).indexOf("frame_rate_cap=-1") >= 0,
          "instance " + oi + ": the steady 30 FPS cap");
}
check(o1.indexOf("LICHII_COOP_PAD=2\r\n") >= 0 && o1.indexOf("LICHII_COOP_KEYBOARD=off") >= 0, "player 1 pad 2: " + o1);
check(o1.indexOf("color = red") >= 0, "player 1 takes the kit's colour: " + o1);
check(o1.indexOf("LICHII_COOP_PUPPET") < 0 && o1.indexOf("name = Saria") < 0, "the kit's other lines stay in the kit's file: " + o1);
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
writeFile(fso.BuildPath(game, "newfile.dll"), "rebuilt");   // a rebuild replaced the DLL
Game.Play();
check(readFile(fso.BuildPath(instances[1], "coop.ini")).indexOf("password = " + pw + "\r\n") >= 0, "password from session.txt");
check(readFile(fso.BuildPath(instances[1], "newfile.dll")) === "rebuilt", "a DLL the handler copied is copied again after a rebuild");
check(readFile(fso.BuildPath(instances[1], "coop-player.ini")).indexOf("LICHII_COOP_PAD=3\r\n") >= 0, "Nucleus's pad 3");

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

// One choice on Nucleus's setup screen: This PC, Host on this PC (the default) or Join a game on this network.
var opt = Game.Options.length === 1 ? Game.Options[0] : {};
check(Game.Options.length === 1 && opt.name === "This PC" && opt.key === "ThisPC", "the option This PC");
check(opt.values && opt.values.length === 3 && opt.values[0] === "Host on this PC" &&
      opt.values[1] === "Join a game on this network" && opt.values[2] === "Join a game on another PC" &&
      opt.def === "Host on this PC",
      "its three values, Host on this PC the default: " + (opt.values ? opt.values.join(" | ") : ""));

// A run as player p (0-based); returns its coop.ini.
function playAs(p) {
    Context.PlayerID = p;
    Context.ExePath = fso.BuildPath(instances[p], "longshot.exe");
    Context.Nickname = players[p].nick;
    Context.GamepadId = players[p].pad;
    Context.IsKeyboardPlayer = players[p].keyboard;
    Game.Play();
    return readFile(fso.BuildPath(instances[p], "coop.ini"));
}
function newSession() {
    coopSession = null;
    startedServers = [];
    startedWindows = [];
}
var kitOptions = fso.BuildPath(kit, "coop-options.ini"), kitOptionsText = readFile(kitOptions);
var sessionTxt = fso.BuildPath(game, "nucleus-players\\session.txt");
writeFile(fso.BuildPath(kit, "coop.ini"), "server = 192.168.1.20\r\nport = 27515\r\nname = Saria\r\n");

// Player 1 starts the kit's start-server.cmd (Host.bat's server) in its own window, on the kit's port, with a new
// session password handed over in OOT_COOP_SESSION_PASSWORD only while it starts. A choice left from an older
// handler (Context.Options) changes nothing.
writeFile(kitOptions, kitOptionsText + "# server_password = not-this-one\r\n");
newSession();
Context.Options = { Online: "This PC only" };
var on1 = playAs(0), on2 = playAs(1);
Context.Options = undefined;
check(startedServers.length === 0, "coop_server not started directly: " + startedServers.join(" | "));
check(startedWindows.length === 1, "one server window, an old choice ignored");
var win = startedWindows.length ? startedWindows[0] : { args: "" };
check(win.args === "/c start \"Squadala server\" /d \"" + kit + "\" \"" + comspec + "\" /c \"\"" +
      fso.BuildPath(kit, "start-server.cmd") + "\" 27515\"",
      "start-server.cmd through cmd /c (the window closes with the server), the kit's port: " + win.args);
var onPw = coopSession.password;
check(/^[0-9a-f]{32}$/.test(onPw) && win.password === onPw, "a new session password, handed over: " + onPw);
check(System.Environment.vars.OOT_COOP_SESSION_PASSWORD === undefined, "the password not left in the env");
check(on1.indexOf("server = 127.0.0.1\r\nport = 27515\r\n") >= 0 && on1.indexOf("password = " + onPw + "\r\n") >= 0,
      "player 1 joins its server on this PC: " + on1);
check(on2.indexOf("server = 127.0.0.1\r\nport = 27515\r\n") >= 0 && on2.indexOf("password = " + onPw + "\r\n") >= 0,
      "player 2 joins at 127.0.0.1: " + on2);
check(readFile(sessionTxt) === onPw + "\r\n127.0.0.1\r\n27515\r\n", "session.txt: " + readFile(sessionTxt));
coopSession = null;
var on2b = playAs(1);
check(on2b.indexOf("server = 127.0.0.1\r\nport = 27515\r\n") >= 0 && on2b.indexOf("password = " + onPw) >= 0 &&
      startedWindows.length === 1, "a later player from session.txt starts nothing: " + on2b);

// Host on this PC, chosen or not: the same. An old "Online" key, another value, no value: host too.
var hostChoices = [{ ThisPC: "Host on this PC" }, { Online: "Allow online players" }, { ThisPC: "Online" }, {}];
for (var hc = 0; hc < hostChoices.length; hc++) {
    newSession();
    Context.Options = hostChoices[hc];
    var h1 = playAs(0), h2 = playAs(1);
    Context.Options = undefined;
    var hPw = coopSession.password, hWhat = "host (choice " + hc + "): ";
    check(startedServers.length === 0 && startedWindows.length === 1 &&
          /\\start-server\.cmd" 27515"$/.test(startedWindows[0].args), hWhat + "one server window");
    check(h1.indexOf("server = 127.0.0.1\r\nport = 27515\r\n") >= 0 && h1.indexOf("password = " + hPw + "\r\n") >= 0 &&
          h2.indexOf("server = 127.0.0.1\r\nport = 27515\r\n") >= 0 && h2.indexOf("password = " + hPw + "\r\n") >= 0 &&
          h1.indexOf("join_lan") < 0 && h2.indexOf("join_lan") < 0, hWhat + "coop.ini: " + h2);
    check(readFile(sessionTxt) === hPw + "\r\n127.0.0.1\r\n27515\r\n", hWhat + "session.txt: " + readFile(sessionTxt));
}

// The kit's server_password, server_bind and server_allow (coop-options.ini), as Host.bat passes them: this PC's
// players go to the bound address, and 127.0.0.1 is added to the allowed ones.
writeFile(kitOptions, kitOptionsText + "server_password = hunter2x\r\nserver_bind = 25.1.2.3\r\n" +
          "server_allow = 25.0.0.0/8, 26.1.2.3\r\n");
newSession();
var b1 = playAs(0), b2 = playAs(2);
win = startedWindows.length ? startedWindows[0] : { args: "" };
check(/\.cmd" 27515 -Bind 25\.1\.2\.3 -Allow 25\.0\.0\.0\/8,26\.1\.2\.3,127\.0\.0\.1,25\.1\.2\.3"$/.test(win.args),
      "-Bind and -Allow (+127.0.0.1 and the bound address the local players come from): " + win.args);
check(win.password === undefined, "the kit's own password is not handed over (start-server reads it)");
check(b1.indexOf("server = 25.1.2.3\r\n") >= 0 && b1.indexOf("password = hunter2x\r\n") >= 0 &&
      b2.indexOf("server = 25.1.2.3\r\n") >= 0 && b2.indexOf("password = hunter2x\r\n") >= 0,
      "the bound address and the kit's password: " + b2);
coopSession = null;
check(playAs(1).indexOf("server = 25.1.2.3\r\nport = 27515\r\n") >= 0, "the bound address from session.txt");
writeFile(kitOptions, kitOptionsText);

// A bad kit port: 27510.
writeFile(fso.BuildPath(kit, "coop.ini"), "port = lots\r\n");
newSession();
var bad = playAs(0);
check(startedWindows.length === 1 && /\.cmd" 27510"$/.test(startedWindows[0].args) &&
      bad.indexOf("server = 127.0.0.1\r\nport = 27510\r\n") >= 0, "no kit port: 27510: " + bad);

// The start fails (Nucleus's Process.Start throws): the password is still taken out of the environment.
newSession();
failStart = true;
var threw = false;
try { playAs(0); } catch (e) { threw = true; }
check(threw && startedWindows.length === 1 && /^[0-9a-f]{32}$/.test(startedWindows[0].password) &&
      System.Environment.vars.OOT_COOP_SESSION_PASSWORD === undefined, "a failed start clears the env too");

// A coop_server already running (Host.bat, start-server.cmd, a session not ended): nothing started, the player told.
newSession();
logged = [];
System.Diagnostics.Process.running = 1;
var blocked = 0;
try { playAs(0); } catch (e) { blocked++; }
try { playAs(1); } catch (e) { blocked++; }
System.Diagnostics.Process.running = 0;
check(blocked === 2, "a running server: no game of the session starts (it would be a guest there): " + blocked);
check(startedWindows.length === 0 && startedServers.length === 0, "a running server: none started");
check(logged.join("\n").indexOf("already running") >= 0, "a running server: the player is told: " + logged.join(" | "));

// A kit without start-server.cmd, and no kit at all (no server\coop_server.exe above the game): no server, said in
// Nucleus's log; the games look for one at 127.0.0.1, port 27510.
var noKit = [[fso.BuildPath(kit, "start-server.cmd"), "no start-server.cmd in"],
             [fso.BuildPath(kit, "server\\coop_server.exe"), "no kit above"]];
for (var nk = 0; nk < noKit.length; nk++) {
    fso.DeleteFile(noKit[nk][0]);
    newSession();
    logged = [];
    var n1 = playAs(0);
    writeFile(noKit[nk][0], "");
    check(startedWindows.length === 0 && startedServers.length === 0, noKit[nk][1] + ": no server started");
    check(logged.join("\n").indexOf(noKit[nk][1]) >= 0 && logged.join("\n").indexOf("no server started") >= 0,
          noKit[nk][1] + ": logged: " + logged.join(" | "));
    check(n1.indexOf("server = 127.0.0.1\r\nport = 27510\r\n") >= 0, noKit[nk][1] + ": coop.ini: " + n1);
}

// A session.txt of an older handler (the password only): 127.0.0.1, port 27510.
writeFile(sessionTxt, "oldpassword\r\n");
coopSession = null;
var old = playAs(1);
check(old.indexOf("server = 127.0.0.1\r\nport = 27510\r\n") >= 0 && old.indexOf("password = oldpassword\r\n") >= 0,
      "an old session.txt: " + old);

// Join a game on this network: no server window, no coop_server, no session.txt; each coop.ini has join_lan and its
// name, no server, port or password. A stale session.txt (a host session before) is not used.
var JOIN = { ThisPC: "Join a game on this network" };
function joinOnly(ini, lan, what) {
    check(ini.indexOf("join_lan = " + lan + "\r\n") >= 0 && !/server|port|password/.test(ini), what + ": " + ini);
}
writeFile(sessionTxt, "stalepassword\r\n192.168.1.99\r\n27599\r\n");
newSession();
logged = [];
Context.Options = JOIN;
var j = [playAs(0), playAs(1), playAs(2)];
check(startedWindows.length === 0 && startedServers.length === 0, "join: no server window and no coop_server");
check(!fso.FileExists(sessionTxt), "join: session.txt removed");
check(logged.join("\n").indexOf("join_lan = first") >= 0, "join: logged: " + logged.join(" | "));
joinOnly(j[0], "first", "join: player 1's coop.ini");
joinOnly(j[1], "first", "join: player 2's coop.ini");
joinOnly(j[2], "first", "join: player 3's coop.ini");
check(j[0].indexOf("name = Saria\r\n") >= 0 && j[2].indexOf("name = Player 3\r\n") >= 0, "join: the names kept");
check(readFile(fso.BuildPath(instances[1], "coop-player.ini")).indexOf("LICHII_COOP_PAD=") >= 0 &&
      readFile(fso.BuildPath(instances[2], "coop-player.ini")).indexOf("LICHII_COOP_KEYBOARD=focus") >= 0,
      "join: the pads and the keyboard player as before");
check(readFile(fso.BuildPath(instances[0], "longshot_options.ini")).indexOf("fullscreen=0") >= 0, "join: longshot options");

// A later player run anew with a stale session.txt, or with a host session still in memory: join_lan all the same.
writeFile(sessionTxt, "stalepassword\r\n192.168.1.99\r\n27599\r\n");
coopSession = null;
joinOnly(playAs(1), "first", "join: a later player ignores a stale session.txt");
coopSession = { password: "stalepassword", server: "192.168.1.99", port: 27599, pads: 0 };
joinOnly(playAs(2), "first", "join: a later player ignores a host session in memory");
fso.DeleteFile(sessionTxt);

// A coop_server already running on this PC (the host may be here): join mode starts none and says nothing of it.
newSession();
logged = [];
System.Diagnostics.Process.running = 1;
joinOnly(playAs(0), "first", "join with a server running");
System.Diagnostics.Process.running = 0;
check(startedWindows.length === 0 && logged.join("\n").indexOf("already running") < 0, "join: a running server left alone");

// join_lan_host in the kit's coop-options.ini: that host's name.
writeFile(kitOptions, kitOptionsText + "join_lan_host =  Malon \r\n");
newSession();
var jh = [playAs(0), playAs(1)];
joinOnly(jh[0], "Malon", "join_lan_host: player 1");
joinOnly(jh[1], "Malon", "join_lan_host: player 2");
writeFile(kitOptions, kitOptionsText);

// No kit: the game's own coop-options.ini.
var gameOptions = fso.BuildPath(game, "coop-options.ini");
writeFile(gameOptions, "join_lan_host = Mido\r\n");
fso.DeleteFile(fso.BuildPath(kit, "server\\coop_server.exe"));
newSession();
joinOnly(playAs(0), "Mido", "join_lan_host from the game's options (no kit)");
writeFile(fso.BuildPath(kit, "server\\coop_server.exe"), "");
fso.DeleteFile(gameOptions);
check(startedWindows.length === 0 && startedServers.length === 0, "join, no kit: no server");
Context.Options = undefined;

// Back to host after a join session: the server again, at 127.0.0.1.
newSession();
var back = playAs(0);
check(startedWindows.length === 1 && back.indexOf("server = 127.0.0.1\r\n") >= 0 && back.indexOf("join_lan") < 0 &&
      fso.FileExists(sessionTxt), "host after join: " + back);

// Join a game on another PC: no server started, every game's coop.ini points at the host, the guard skipped, the
// handler closes no coop_server, a bad code tells the player.
function noHostLine(p) {
    var t = readFile(fso.BuildPath(instances[p], "coop.ini"));
    return t !== "" && !/^\s*(server|port|password)\s*=/mi.test(t);   // written, and no host line (starts solo)
}
var REMOTE = { ThisPC: "Join a game on another PC" }, joinTxt = fso.BuildPath(game, "nucleus-players\\join.txt");
check(coopSha256([97, 98, 99]).slice(0, 4).join(",") === "186,120,22,191", "SHA-256 of abc: " + coopSha256([97, 98, 99]).slice(0, 4).join(","));
var invite = "CHFSA-6JFPP-5AATB-UCVDW-EXFYG-ZH2J3-KSGWK-S2GA";
writeFile(joinTxt, invite + "\r\n");
newSession();
Context.Options = REMOTE;
Game.KillProcessesOnClose = ["coop_server"];
System.Diagnostics.Process.running = 1;        // a Host.bat of this PC is running: not our business
logged = [];
var j1 = playAs(0), j2 = playAs(1), j3 = playAs(2);
System.Diagnostics.Process.running = 0;
check(startedWindows.length === 0 && startedServers.length === 0, "remote: no server started");
check(logged.join("\n").indexOf("already running") < 0, "remote: the running-server guard is skipped");
check(Game.KillProcessesOnClose.length === 0, "remote: nothing closed at the end (no coop_server killed)");
var joinOk = true, js = [j1, j2, j3];
for (var q = 0; q < 3; q++) {
    joinOk = joinOk && js[q].indexOf("server = 203.0.113.5\r\nport = 27510\r\n") >= 0 &&
             js[q].indexOf("password = ABCDEFGHJKLMNPQRSTUV\r\n") >= 0;
}
check(joinOk, "remote: every game's coop.ini points at the host: " + j3);
// A later start that runs the script anew per player: from session.txt.
coopSession = null;
check(playAs(1).indexOf("server = 203.0.113.5\r\nport = 27510\r\n") >= 0, "remote: a later player from session.txt");
// The Same network / VPN line, with and without a port.
writeFile(joinTxt, "26.1.2.3:27600 hunter2x\r\n");
newSession();
check(playAs(0).indexOf("server = 26.1.2.3\r\nport = 27600\r\n") >= 0, "remote: address:port password");
writeFile(joinTxt, "25.9.9.9 hunter2x\r\n");
newSession();
var lineNoPort = playAs(0);
check(lineNoPort.indexOf("server = 25.9.9.9\r\nport = 27510\r\n") >= 0 && lineNoPort.indexOf("password = hunter2x\r\n") >= 0,
      "remote: address password, the kit port");
// Bad input: the player is told, no server started, nothing joined.
var bads = [invite.substring(0, invite.length - 1), "CHFSA-6JFPP-5AATB-UCVDW-EXFYG-ZH2J3-KSGWK-S2GB", "hello", "999.1.1.1 pw", "",
            "EHDDG-3ACVT-ASCAS-DASCS-NB2JP-PE7EA-GJEUQ-WC2NA-MLPZ3-JS"];
for (var bi = 0; bi < bads.length; bi++) {
    writeFile(joinTxt, bads[bi] + "\r\n");
    newSession();
    logged = [];
    writeFile(sessionTxt, "stale\r\n10.0.0.1\r\n27510\r\n");
    for (var dp = 0; dp < 2; dp++) {
        writeFile(fso.BuildPath(instances[dp], "coop.ini"), "server = 203.0.113.5\r\nport = 27510\r\npassword = oldhost\r\n");   // the last session's host
    }
    var badThrew = false, bj = "", bj2 = "", badThrew2 = false;
    try { bj = playAs(0); } catch (e1) { badThrew = true; }
    try { bj2 = playAs(1); } catch (e2) { badThrew2 = true; }   // a later player: nothing either
    check(badThrew && badThrew2 && startedWindows.length === 0 && startedServers.length === 0 &&
          logged.join("\n").indexOf("Nothing is started") >= 0 && !fso.FileExists(sessionTxt) && noHostLine(0) && noHostLine(1),
          "remote: '" + bads[bi] + "' tells the player, starts nothing, no host line in coop.ini, no stale session: " + logged.join(" | "));
}
// A bad code keeps the player's own lines (the name typed in the game, the look): only the host's lines go, and the
// next start still has them.
writeFile(joinTxt, "hello\r\n");
newSession();
writeFile(fso.BuildPath(instances[0], "coop.ini"),
          "server = 203.0.113.5\r\nport = 27510\r\npassword = oldhost\r\nname = Kokiri Kid\r\nlook_boots = black\r\ncolor = 3\r\n");
try { playAs(0); } catch (e3) { }
var noHost = readFile(fso.BuildPath(instances[0], "coop.ini"));
check(noHostLine(0) && noHost.indexOf("name = Kokiri Kid\r\n") >= 0 && noHost.indexOf("look_boots = black\r\n") >= 0 &&
      noHost.indexOf("color = 3\r\n") >= 0, "remote: a bad code keeps the name and look in coop.ini: " + noHost);
fso.DeleteFile(joinTxt);
Context.Options = undefined;
newSession();
var afterBad = playAs(0);
check(afterBad.indexOf("name = Kokiri Kid\r\n") >= 0 && afterBad.indexOf("look_boots = black\r\n") >= 0,
      "the start after a bad code still has the name and look: " + afterBad);

// What the player set in the game stays over a new start: the name typed in the game and the look in coop.ini, and the
// player's own lines the game saved in coop-player.ini; the controller lines are the handler's again.
var w = "# Written by the Nucleus Co-op handler (tools/nucleus/OoTCoop.js) at each start.\r\n";
writeFile(fso.BuildPath(instances[0], "coop.ini"),
          w + "server = 127.0.0.1\r\nport = 27510\r\nname = Kokiri Kid\r\npassword = x\r\nlook_boots = black\r\n");
var ownFile = fso.BuildPath(instances[0], "coop-player.ini");
writeFile(ownFile, "# mine\r\nlook_hylian_frame = red\r\nLICHII_COOP_PAD=4\r\n");
newSession();
var kept = playAs(0);
check(kept.indexOf("name = Kokiri Kid\r\n") >= 0 && kept.indexOf("name = Saria") < 0, "the name typed in the game stays: " + kept);
check(kept.indexOf("look_boots = black\r\n") >= 0, "the look in coop.ini stays: " + kept);
check(kept.indexOf("server = 127.0.0.1\r\n") >= 0 && kept.indexOf("password = x") < 0, "the session's lines new: " + kept);
var keptOptions = readFile(ownFile);
check(keptOptions.indexOf("look_hylian_frame = red") >= 0, "the player's own look stays: " + keptOptions);
check(keptOptions.indexOf("LICHII_COOP_PAD=4") < 0 && keptOptions.split("LICHII_COOP_PAD=").length === 2,
      "the controller line is the handler's: " + keptOptions);

// An instance of an older handler (0.11.8 to 0.11.12): a full copy of the options beside the game. Its player lines are
// taken over once and the copy goes, so the kit's settings reach this game again.
fso.DeleteFile(ownFile);
writeFile(fso.BuildPath(instances[0], "coop-options.ini"),
          w + "LICHII_COOP_CHAT_KEY=Y\r\nlook_hylian_frame = blue\r\ncolor = pink\r\nLICHII_COOP_PAD=4\r\n");
newSession();
playAs(0);
var moved = readFile(ownFile);
check(!fso.FileExists(fso.BuildPath(instances[0], "coop-options.ini")), "the old copy goes");
check(moved.indexOf("look_hylian_frame = blue") >= 0 && moved.indexOf("color = pink") >= 0, "its look is taken over: " + moved);
check(moved.indexOf("LICHII_COOP_CHAT_KEY") < 0, "its other lines are the kit's again: " + moved);
// A coop-options.ini the player put there themselves (not the handler's) is left alone.
writeFile(fso.BuildPath(instances[0], "coop-options.ini"), "mine = 1\r\n");
newSession();
playAs(0);
check(readFile(fso.BuildPath(instances[0], "coop-options.ini")) === "mine = 1\r\n", "a player's own file stays");
fso.DeleteFile(fso.BuildPath(instances[0], "coop-options.ini"));

// local_fps = 60 in the kit's options: the console's 60 for every game.
writeFile(kitOptions, kitOptionsText + "local_fps = 60\r\n");
newSession();
playAs(1);
check(readFile(fso.BuildPath(instances[1], "longshot_options.ini")).indexOf("frame_rate_cap=0") >= 0, "local_fps = 60");
writeFile(kitOptions, kitOptionsText);

// Clean up: the links first (rmdir removes a link, not what it names), then the rest.
for (var r = 0; r < 3; r++) shell.Run("cmd /c rmdir \"" + fso.BuildPath(instances[r], "user") + "\"", 0, true);
check(fso.FileExists(fso.BuildPath(game, "user\\save\\host.sav")), "removing the link left the saves");
fso.DeleteFolder(root, true);

WScript.Echo(failures === 0 ? "ok: " + checks + " checks" : failures + " of " + checks + " checks failed");
WScript.Quit(failures === 0 ? 0 : 1);
