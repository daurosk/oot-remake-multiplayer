// SPDX-License-Identifier: GPL-3.0-or-later
// Nucleus Co-op handler for OoT Co-op on Longshot: 2 to 4 players on one PC, split-screen, one click.
// How to install it and what it does: tools/nucleus/README.md and docs/play-lan.md, "Split-screen with Nucleus Co-op".
//
// Each player's game runs from its own Nucleus instance folder (links to the game's big files, its own small ones).
// Player 1 starts the kit's co-op server and plays on it; players 2 to 4 join it on this PC. The server is the kit's
// own start-server.cmd, as Host.bat starts it: every address of this PC, the kit's port and join password, a join code
// (or the kit's room name) on the Squadala relay, built in, and an invite code in the server's window, one copied to
// the clipboard, that friends on other PCs type under Join. The option "This PC" can choose "Join a game on this
// network" instead: no server, and each game joins the first game hosted on this network it finds (coop.ini
// join_lan). Every game reads its own controller itself
// (LICHII_COOP_PAD, patch 0015), also behind the other windows, so no Nucleus input hooks are used (Nucleus only
// lists the pads).
// Written for Nucleus's script engine (Jint) in plain ES3 style, so tools/nucleus/test_handler.js runs it under
// Windows' own cscript too.

Game.GameName = "OoT Co-op (Longshot)";
Game.GUID = "OoTCoopLongshot";
Game.ExecutableName = "longshot.exe";
Game.MaxPlayers = 4;
Game.MaxPlayersOneMonitor = 4;
Game.Description = "Split-screen OoT Co-op on Longshot. Add the longshot.exe of a kit's co-op game (the kit's " +
    "hookshot\\game folder). Player 1 hosts on this PC, the others join. Each game needs about 10 GB of RAM. " +
    "Friends on other PCs can join too, with the room name or join code on the Squadala relay, or the invite " +
    "code, that the server window shows and copies. This PC: Join a game on this network when another PC hosts.";

// The option on Nucleus's setup screen. Anything but COOP_JOIN (no choice made yet, an older Nucleus, the "Online"
// key of an older handler) hosts on this PC.
var COOP_HOST = "Host on this PC";
var COOP_JOIN = "Join a game on this network";
Game.AddOption("This PC",
               "Host on this PC: player 1 starts the server, as Host.bat does, and friends on other PCs can join " +
               "with its invite code. Join a game on this network: no server here; each game joins the first game " +
               "hosted on this network (or the host named by join_lan_host in coop-options.ini), no password needed.",
               "ThisPC", [COOP_HOST, COOP_JOIN], COOP_HOST);

// The files: the game's big read-only files are linked, its folders too; what a game writes is its own.
Game.SymlinkGame = true;
Game.SymlinkExe = true;          // 450 MB: linked, the exe's folder is still the instance's
Game.SymlinkFolders = true;      // romfs (the game's files) and mods as links, not copies
Game.KeepSymLinkOnExit = true;   // instance folders stay: the next start makes no links (no admin prompt) and keeps
                                 // each game's settings and caches
// user (the saves): never linked or copied by Nucleus; Game.Play links it (player 1 to the game's own saves, the
// others to nucleus-players\playerN). nucleus-players: this handler's folder beside the game. logs: each game's own
// (the game makes it at its start), else every game writes the same longshot_log.txt and co-op logs.
Game.DirExclusions = ["user", "nucleus-players", "logs"];
// Written by Game.Play for each game.
Game.FileSymlinkExclusions = ["coop.ini", "coop-options.ini", "coop-kit.txt", "player-key.txt", "longshot_log.txt",
    "longshot_log.1.txt", "hookshot_log.txt", "longshot_load_start.txt", "longshot_damaged_files.txt"];
// Small files a game writes in place: a copy each. (The big caches stay links: the game saves one through a .tmp
// moved over it, which puts the game's own file in its instance folder and leaves the original as it is.)
Game.FileSymlinkCopyInstead = ["longshot_options.ini", "language.txt", "longshot_pipeline_states.bin",
    "longshot_compute_variants.bin", "longshot_computes.bin", "longshot_module_ids.bin", "mkw_vk_pipelines.stamp",
    "longshot_prepared.bin", "longshot_pipelines.bin.built"];

// Input: each game picks its own controller (LICHII_COOP_PAD) and reads it in the background; no hook DLLs.
// XInputEnabled only makes Nucleus list the Xbox-style pads in its setup screen (DevicesFunctions.GamepadTimer_Tick
// lists none without it) and give each player its pad (Context.GamepadId, XInput slot + 1). Nucleus injects or copies
// input DLLs only for CustomDllEnabled, XInputPlusDll, UseX360ce and ProtoInput, all off here.
Game.SupportsKeyboard = true;
Game.Hook.CustomDllEnabled = false;
Game.Hook.XInputEnabled = true;
Game.Hook.XInputReroute = false;
Game.Hook.DInputEnabled = false;   // DirectInput pads would get Nucleus's DirectInput order, not the game's
Game.Hook.ForceFocus = false;

// Windows: Nucleus places, sizes and unborders them.
Game.SetWindowHook = false;
Game.KeepAspectRatio = false;
Game.PauseBetweenStarts = 15;         // seconds (Nucleus sleeps TimeSpan.FromSeconds): player 1 first, it hosts
                                      // (the first player in hosts, docs/protocol.md)
Game.PauseBetweenProcessGrab = 5;     // seconds
Game.KillProcessesOnClose = ["coop_server"];   // the session's server (and any other coop_server.exe on this PC)

// The session's settings (also in nucleus-players\session.txt, for a start that runs the script anew per player).
var COOP_KIT_PORT = 27510;   // the kit's coop.ini port, else this one (Host.bat's)
var coopSession = null;

function coopPath(a, b) {
    return System.IO.Path.Combine(a, b);
}
function coopRead(p) {
    return System.IO.File.Exists(p) ? String(System.IO.File.ReadAllText(p)) : "";
}
function coopWrite(p, text) {
    System.IO.File.WriteAllText(p, text);
}
function coopTrim(s) {
    return String(s).replace(/^\s+|\s+$/g, "");
}

// The kit this game was built in: coop-kit.txt beside the game, else the nearest folder above it that has
// server\coop_server.exe (a kit's game is <kit>\hookshot\game). "" when none.
function coopFindKit(gameDir) {
    var named = coopTrim(coopRead(coopPath(gameDir, "coop-kit.txt")).split(/\r?\n/)[0]);
    if (named !== "" && System.IO.File.Exists(coopPath(coopPath(named, "server"), "coop_server.exe"))) {
        return named;
    }
    var d = gameDir;
    for (var i = 0; i < 6 && d; i++) {
        if (System.IO.File.Exists(coopPath(coopPath(d, "server"), "coop_server.exe"))) {
            return d;
        }
        var up = System.IO.Path.GetDirectoryName(d);
        if (!up || String(up) === String(d)) {
            break;
        }
        d = String(up);
    }
    return "";
}

// The value of `key` in an ini text ("key = value", any case; '#' and ';' lines skipped; the last one wins), ""
// when none.
function coopGetKey(text, key) {
    var lines = String(text).split(/\r?\n/), value = "";
    for (var i = 0; i < lines.length; i++) {
        var t = coopTrim(lines[i]), eq = t.indexOf("=");
        if (eq > 0 && t.charAt(0) !== "#" && t.charAt(0) !== ";" &&
            coopTrim(t.substring(0, eq)).toLowerCase() === key.toLowerCase()) {
            value = coopTrim(t.substring(eq + 1));
        }
    }
    return value;
}

// True when Nucleus's setup screen has "Join a game on this network" chosen.
function coopWantsJoin() {
    try {
        var o = Context.Options;
        return !!o && String(o["ThisPC"]) === COOP_JOIN;
    } catch (e) {
        return false;   // no such option (an older Nucleus): host
    }
}

// What coop.ini's join_lan says in join mode: join_lan_host from the options (a host's player name), else "first".
function coopJoinLan(options) {
    var host = coopCleanName(coopGetKey(options, "join_lan_host"));
    return host === "" ? "first" : host;
}

// Player 1's server: the kit's start-server.cmd in a window of its own, as Host.bat starts it (scripts\start-server.ps1:
// coop_server on every address of this PC, the router forward, the invite code shown and copied, a log in the kit's
// logs folder). Returns the session { password, server, port } the players on this PC use.
function coopStartOnline(kit) {
    var options = coopRead(coopPath(kit, "coop-options.ini"));
    var port = parseInt(coopGetKey(coopRead(coopPath(kit, "coop.ini")), "port"), 10);
    if (!(port >= 1 && port <= 65535)) {
        port = COOP_KIT_PORT;
    }
    // The kit's join password (server_password), else a new one for this session, handed over as Host.bat does
    // (OOT_COOP_SESSION_PASSWORD: never on a command line; the server puts it in the invite code).
    var password = coopGetKey(options, "server_password");
    var made = password === "";
    if (made) {
        password = coopNewKey();
    }
    // server_bind: the server listens on that address only, so the players on this PC go there too (and come from
    // it). server_allow: this PC's address the players come from (127.0.0.1, or the bound one) is added, else its own
    // players would be refused.
    var bind = coopGetKey(options, "server_bind");
    var allow = coopGetKey(options, "server_allow").replace(/\s/g, "");
    var args = String(port);
    if (/^\d{1,3}(\.\d{1,3}){3}$/.test(bind)) {
        args += " -Bind " + bind;
    } else {
        bind = "";
    }
    if (/^[0-9.\/,]+$/.test(allow)) {
        args += " -Allow " + allow + ",127.0.0.1" + (bind !== "" ? "," + bind : "");
    }
    // A window of its own that closes with the server: start runs cmd /c (a .cmd started by itself would be cmd /k,
    // whose window stays open after Nucleus closes coop_server). The inner cmd /c drops the outer pair of quotes.
    var cmd = String(System.Environment.GetEnvironmentVariable("ComSpec") || "C:\\Windows\\System32\\cmd.exe");
    try {
        if (made) {
            System.Environment.SetEnvironmentVariable("OOT_COOP_SESSION_PASSWORD", password);
        }
        Context.StartProcess(cmd, "/c start \"Squadala server\" /d \"" + kit + "\" \"" + cmd + "\" /c \"\"" +
                             coopPath(kit, "start-server.cmd") + "\" " + args + "\"", false);
    } finally {
        if (made) {
            System.Environment.SetEnvironmentVariable("OOT_COOP_SESSION_PASSWORD", null);
        }
    }
    return { password: password, server: bind !== "" ? bind : "127.0.0.1", port: port, pads: 0 };
}

// A player name the game takes: up to 16 characters, no control characters ("" when none is left).
function coopCleanName(nick) {
    var n = coopTrim(String(nick || "").replace(/[\x00-\x1f\x7f]/g, ""));
    if (n.length > 16) {
        n = coopTrim(n.substring(0, 16));
    }
    return n;
}
function coopName(nick, player) {
    var n = coopCleanName(nick);
    return n === "" ? "Player " + player : n;
}

// `text` without the lines whose key (before "=") is one of `keys`, any case, spaces around it skipped.
function coopDropKeys(text, keys) {
    var lines = String(text).split(/\r?\n/), out = [];
    for (var i = 0; i < lines.length; i++) {
        var t = coopTrim(lines[i]), eq = t.indexOf("="), drop = false;
        if (eq > 0 && t.charAt(0) !== "#" && t.charAt(0) !== ";") {
            var k = coopTrim(t.substring(0, eq)).toLowerCase();
            for (var j = 0; j < keys.length; j++) {
                drop = drop || k === keys[j].toLowerCase();
            }
        }
        if (!drop) {
            out.push(lines[i]);
        }
    }
    while (out.length && coopTrim(out[out.length - 1]) === "") {
        out.pop();
    }
    return out.join("\r\n");
}

// `text`'s lines whose key passes `keep` (the key in lower case), comments and other lines left out.
function coopKeepKeys(text, keep) {
    var lines = String(text).split(/\r?\n/), out = [];
    for (var i = 0; i < lines.length; i++) {
        var t = coopTrim(lines[i]), eq = t.indexOf("=");
        if (eq > 0 && t.charAt(0) !== "#" && t.charAt(0) !== ";" && keep(coopTrim(t.substring(0, eq)).toLowerCase())) {
            out.push(t);
        }
    }
    return out.join("\r\n");
}

// What this handler wrote before (its first line says so), without that first line; "" for any other text.
var COOP_WRITTEN = "# Written by the Nucleus Co-op handler (tools/nucleus/OoTCoop.js) at each start.";
function coopOwnWritten(text) {
    var t = String(text);
    if (t.indexOf(COOP_WRITTEN) !== 0) {
        return "";
    }
    return coopTrim(t.substring(COOP_WRITTEN.length).replace(/^\r?\n/, ""));
}

// `text` with key=value set (the line replaced, else added).
function coopSetKey(text, key, value) {
    var t = coopDropKeys(text, [key]);
    return (t === "" ? "" : t + "\r\n") + key + "=" + value;
}

// FileAttributes.ReparsePoint (1024). Jint hands a CLR enum over as a number or as a wrapper; both are taken.
function coopIsLink(p) {
    var a = new System.IO.DirectoryInfo(p).Attributes;
    if (typeof a === "number") {
        return (a & 1024) !== 0;
    }
    var n = Number(a);
    if (!isNaN(n)) {
        return (n & 1024) !== 0;
    }
    return String(a.ToString ? a.ToString() : a).indexOf("ReparsePoint") >= 0;
}

// <link> made a junction to <target> (no admin needed). A real folder there is kept beside it, renamed.
function coopLinkFolder(link, target) {
    if (System.IO.Directory.Exists(link)) {
        if (coopIsLink(link)) {
            return true;
        }
        System.IO.Directory.Move(link, link + ".nucleus-" + new Date().getTime());
    }
    var cmd = System.Environment.GetEnvironmentVariable("ComSpec") || "C:\\Windows\\System32\\cmd.exe";
    Context.StartProcess(String(cmd), "/c mklink /J \"" + link + "\" \"" + target + "\"", false);
    for (var i = 0; i < 100 && !System.IO.Directory.Exists(link); i++) {
        System.Threading.Thread.Sleep(100);
    }
    return System.IO.Directory.Exists(link);
}

// 32 hex digits (a player key, a join password).
function coopNewKey() {
    return String(System.Guid.NewGuid().ToString("N"));
}

// How many coop_server.exe run on this PC now (0 when it cannot be told). A CLR array may come over with Length or
// length.
function coopServersRunning() {
    try {
        var ps = System.Diagnostics.Process.GetProcessesByName("coop_server");
        var n = ps.Length !== undefined ? ps.Length : ps.length;
        return Number(n) || 0;
    } catch (e) {
        return 0;
    }
}

// A message for the player: a message box when the script may show one, and Nucleus's log either way.
function coopTell(text) {
    Context.Log("OoT Co-op: " + text);
    try {
        System.Windows.Forms.MessageBox.Show(text, "Squadala!");
    } catch (e) {
        // no Windows Forms in this script engine: the log only
    }
}

Game.Play = function () {
    var player = Context.PlayerID + 1;                  // 1 to 4
    var game = String(Context.RootInstallFolder);       // the game folder Nucleus was given
    var inst = String(System.IO.Path.GetDirectoryName(Context.ExePath));   // this player's instance folder
    var mine = coopPath(game, "nucleus-players");
    System.IO.Directory.CreateDirectory(mine);
    var sessionFile = coopPath(mine, "session.txt");
    var kit = coopFindKit(game);
    var kitOptions = coopRead(kit !== "" ? coopPath(kit, "coop-options.ini") : coopPath(game, "coop-options.ini"));
    var join = coopWantsJoin();

    // The session: player 1 makes it and starts the server; the others take it.
    // session.txt: the join password, the server's address, its port (a line each).
    // Join a game on this network: no server and no session.txt (so no later start takes a stale server); each game
    // finds the host itself (coop.ini join_lan).
    if (join) {
        if (player === 1 || coopSession === null || coopSession.lan === undefined) {
            coopSession = { lan: coopJoinLan(kitOptions), pads: 0 };
        }
        if (player === 1) {
            if (System.IO.File.Exists(sessionFile)) {
                System.IO.File.Delete(sessionFile);
            }
            Context.Log("OoT Co-op: join a game on this network (join_lan = " + coopSession.lan + "): no server started");
        }
    } else if (player === 1) {
        var running = kit !== "" ? coopServersRunning() : 0;
        if (running > 0) {
            // Another server (Host.bat, start-server.cmd, an earlier session not ended) holds the port or has
            // another join password: this session's games could not join. Nothing is started; it is not closed here
            // (a friend may be playing on it).
            coopSession = { password: coopNewKey(), server: "127.0.0.1", port: COOP_KIT_PORT, pads: 0 };
            coopTell("A co-op server (coop_server.exe) is already running on this PC, so this session starts none " +
                     "and its games cannot join. Close that server's window, end this " +
                     "session in Nucleus and press Play again.");
        } else if (kit !== "" && System.IO.File.Exists(coopPath(kit, "start-server.cmd"))) {
            coopSession = coopStartOnline(kit);
            Context.Log("OoT Co-op: the server window shows the relay's join code or room, and the invite code (one copied)");
        } else {
            // No kit (or one without start-server.cmd): no server; the games look for one at 127.0.0.1.
            coopSession = { password: coopNewKey(), server: "127.0.0.1", port: COOP_KIT_PORT, pads: 0 };
            Context.Log("OoT Co-op: " + (kit === "" ? "no kit above " + game + " (server\\coop_server.exe)"
                                                   : "no start-server.cmd in " + kit) + ": no server started");
        }
        coopWrite(sessionFile, coopSession.password + "\r\n" + coopSession.server + "\r\n" + coopSession.port + "\r\n");
    } else if (coopSession === null || coopSession.lan !== undefined) {
        var saved = coopRead(sessionFile).split(/\r?\n/);
        var savedServer = coopTrim(saved.length > 1 ? saved[1] : ""), savedPort = parseInt(saved[2], 10);
        coopSession = { password: coopTrim(saved[0]),
                        server: /^\d{1,3}(\.\d{1,3}){3}$/.test(savedServer) ? savedServer : "127.0.0.1",
                        port: savedPort >= 1 && savedPort <= 65535 ? savedPort : COOP_KIT_PORT, pads: 0 };
    }

    // The saves: player 1's own (the game's user folder), the others' their own folder beside the game. A guest
    // plays from its co-op save folder, filled with the host's save when it joins.
    var saves = player === 1 ? coopPath(game, "user") : coopPath(coopPath(mine, "player" + player), "user");
    System.IO.Directory.CreateDirectory(saves);
    if (!coopLinkFolder(coopPath(inst, "user"), saves)) {
        Context.Log("OoT Co-op: player " + player + ": could not link " + inst + "\\user to " + saves);
    }

    // The logs: this game's own. An instance folder made by an older handler kept a link to the game's logs folder:
    // the link goes (a non-recursive delete of a link removes the link only), the game makes its own at its start.
    var logs = coopPath(inst, "logs");
    if (System.IO.Directory.Exists(logs) && coopIsLink(logs)) {
        System.IO.Directory.Delete(logs, false);
    }

    // The player's key (the Lobby's mutes and bans follow it): player 1's own, the others' kept with their saves.
    var keyFile = player === 1 ? coopPath(game, "player-key.txt")
                               : coopPath(coopPath(mine, "player" + player), "player-key.txt");
    if (coopTrim(coopRead(keyFile)) === "") {
        coopWrite(keyFile, coopNewKey() + "\r\n");
    }
    coopWrite(coopPath(inst, "player-key.txt"), coopRead(keyFile));

    // What the player set in this game last time stays: the name typed in the game, and the look and settings the game
    // saved (coop.ini's name and look, coop-options.ini). Before, each start wrote both anew and the name went back to
    // Nucleus's nickname and the look to the kit's (a tester, 2026-11-20).
    var before = coopRead(coopPath(inst, "coop.ini"));
    var ownName = coopCleanName(coopGetKey(before, "name"));
    var ownLook = coopKeepKeys(before, function (k) { return k === "color" || k.indexOf("look_") === 0; });

    // coop.ini: the server on this PC, the session's join password, the player's name. Join mode: join_lan instead of
    // the server, port and password.
    var name = "name = " + (ownName !== "" ? ownName : coopName(Context.Nickname, player)) + "\r\n";
    coopWrite(coopPath(inst, "coop.ini"),
              COOP_WRITTEN + "\r\n" +
              (coopSession.lan !== undefined ? "join_lan = " + coopSession.lan + "\r\n" + name
                  : "server = " + coopSession.server + "\r\nport = " + coopSession.port + "\r\n" + name +
                    "password = " + coopSession.password + "\r\n") +
              (ownLook !== "" ? ownLook + "\r\n" : ""));

    // coop-options.ini: this game's own from last time (the settings and look the game saved there), else the kit's
    // (else the game's) switches; with this game's controller and keyboard. The game reads it beside longshot.exe, as no
    // kit is above the instance folder (CoopTitle::ApplyKitOptions). Removed, the next start takes the kit's again.
    var ownOptions = coopOwnWritten(coopRead(coopPath(inst, "coop-options.ini")));
    var options = ownOptions !== "" ? coopDropKeys(ownOptions, ["LICHII_COOP_PAD", "LICHII_COOP_KEYBOARD"])
        : coopDropKeys(kitOptions, ["LICHII_COOP_PAD", "LICHII_COOP_KEYBOARD"].concat(player === 1 ? [] : ["color"]));
    if (Context.IsKeyboardPlayer) {
        options = coopSetKey(options, "LICHII_COOP_PAD", "none");
        options = coopSetKey(options, "LICHII_COOP_KEYBOARD", "focus");
    } else {
        // Nucleus's number for the pad (XInput's slot + 1), else the controller players in order.
        coopSession.pads++;
        var pad = Context.GamepadId > 0 ? Context.GamepadId : coopSession.pads;
        options = coopSetKey(options, "LICHII_COOP_PAD", String(pad));
        options = coopSetKey(options, "LICHII_COOP_KEYBOARD", "off");
    }
    coopWrite(coopPath(inst, "coop-options.ini"), COOP_WRITTEN + "\r\n" + options + "\r\n");

    // Longshot's own options: in a window (Nucleus places it), no Discord status from every copy. A controller
    // player's camera is its pad's: Mouse camera "As the stick" (0), not Native (1), whose last mouse angles would be
    // put back over the right stick each frame (patch 0015 keeps a game with no mouse out of it as well). A keyboard
    // player keeps its own choice.
    var lsOptions = coopPath(inst, "longshot_options.ini");
    if (System.IO.File.Exists(lsOptions)) {
        var o = coopSetKey(coopRead(lsOptions), "fullscreen", "0");
        o = coopSetKey(o, "discord_status", "0");
        if (!Context.IsKeyboardPlayer) {
            o = coopSetKey(o, "mouse_camera", "0");
        }
        coopWrite(lsOptions, o + "\r\n");
    }
};
