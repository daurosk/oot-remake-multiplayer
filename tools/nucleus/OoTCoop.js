// SPDX-License-Identifier: GPL-3.0-or-later
// Nucleus Co-op handler for OoT Co-op on Longshot: 2 to 4 players on one PC, split-screen, one click.
// How to install it and what it does: tools/nucleus/README.md and docs/play-lan.md, "Split-screen with Nucleus Co-op".
//
// Each player's game runs from its own Nucleus instance folder (links to the game's big files, its own small ones).
// Player 1 starts the kit's co-op server (this PC only, a new join password each session) and plays on it; players
// 2 to 4 join it. Every game reads its own controller itself (LICHII_COOP_PAD, patch 0015), also behind the other
// windows, so no Nucleus input hooks are used.
// Written for Nucleus's script engine (Jint) in plain ES3 style, so tools/nucleus/test_handler.js runs it under
// Windows' own cscript too.

Game.GameName = "OoT Co-op (Longshot)";
Game.GUID = "OoTCoopLongshot";
Game.ExecutableName = "longshot.exe";
Game.MaxPlayers = 4;
Game.MaxPlayersOneMonitor = 4;
Game.Description = "Split-screen OoT Co-op on Longshot. Add the longshot.exe of a kit's co-op game (the kit's " +
    "hookshot\\game folder). Player 1 hosts on this PC, the others join. Each game needs about 10 GB of RAM.";

// The files: the game's big read-only files are linked, its folders too; what a game writes is its own.
Game.SymlinkGame = true;
Game.SymlinkExe = true;          // 450 MB: linked, the exe's folder is still the instance's
Game.SymlinkFolders = true;      // romfs (the game's files) and mods as links, not copies
Game.KeepSymLinkOnExit = true;   // instance folders stay: the next start makes no links (no admin prompt) and keeps
                                 // each game's settings and caches
// user (the saves): never linked or copied by Nucleus; Game.Play links it (player 1 to the game's own saves, the
// others to nucleus-players\playerN). nucleus-players: this handler's folder beside the game.
Game.DirExclusions = ["user", "nucleus-players"];
// Written by Game.Play for each game.
Game.FileSymlinkExclusions = ["coop.ini", "coop-options.ini", "coop-kit.txt", "player-key.txt", "longshot_log.txt",
    "longshot_log.1.txt", "hookshot_log.txt", "longshot_load_start.txt", "longshot_damaged_files.txt"];
// Small files a game writes in place: a copy each. (The big caches stay links: the game saves one through a .tmp
// moved over it, which puts the game's own file in its instance folder and leaves the original as it is.)
Game.FileSymlinkCopyInstead = ["longshot_options.ini", "language.txt", "longshot_pipeline_states.bin",
    "longshot_compute_variants.bin", "longshot_computes.bin", "longshot_module_ids.bin", "mkw_vk_pipelines.stamp",
    "longshot_prepared.bin", "longshot_pipelines.bin.built"];

// Input: each game picks its own controller (LICHII_COOP_PAD) and reads it in the background; no hook DLLs.
Game.SupportsKeyboard = true;
Game.Hook.CustomDllEnabled = false;
Game.Hook.XInputEnabled = false;
Game.Hook.DInputEnabled = false;
Game.Hook.ForceFocus = false;

// Windows: Nucleus places, sizes and unborders them.
Game.SetWindowHook = false;
Game.KeepAspectRatio = false;
Game.PauseBetweenStarts = 15;         // seconds (Nucleus sleeps TimeSpan.FromSeconds): player 1 first, it hosts
                                      // (the first player in hosts, docs/protocol.md)
Game.PauseBetweenProcessGrab = 5;     // seconds
Game.KillProcessesOnClose = ["coop_server"];   // the session's server (and any other coop_server.exe on this PC)

// The session's settings (also in nucleus-players\session.txt, for a start that runs the script anew per player).
var COOP_PORT = 27590;   // not the kit's 27510, nor the test lanes' 27520/27530
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

// A player name the game takes: 1 to 16 characters, no control characters.
function coopName(nick, player) {
    var n = coopTrim(String(nick || "").replace(/[\x00-\x1f\x7f]/g, ""));
    if (n.length > 16) {
        n = coopTrim(n.substring(0, 16));
    }
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

Game.Play = function () {
    var player = Context.PlayerID + 1;                  // 1 to 4
    var game = String(Context.RootInstallFolder);       // the game folder Nucleus was given
    var inst = String(System.IO.Path.GetDirectoryName(Context.ExePath));   // this player's instance folder
    var mine = coopPath(game, "nucleus-players");
    System.IO.Directory.CreateDirectory(mine);
    var sessionFile = coopPath(mine, "session.txt");
    var kit = coopFindKit(game);

    // The session: player 1 makes it and starts the server; the others take it.
    if (player === 1) {
        coopSession = { password: coopNewKey(), pads: 0 };
        coopWrite(sessionFile, coopSession.password + "\r\n");
        if (kit !== "") {
            var server = coopPath(coopPath(kit, "server"), "coop_server.exe");
            Context.StartProcess(server, "--port " + COOP_PORT + " --bind 127.0.0.1 --max-players 4 --password " +
                coopSession.password, false);
        } else {
            Context.Log("OoT Co-op: no kit above " + game + " (server\\coop_server.exe): no server started");
        }
    } else if (coopSession === null) {
        coopSession = { password: coopTrim(coopRead(sessionFile).split(/\r?\n/)[0]), pads: 0 };
    }

    // The saves: player 1's own (the game's user folder), the others' their own folder beside the game. A guest
    // plays from its co-op save folder, filled with the host's save when it joins.
    var saves = player === 1 ? coopPath(game, "user") : coopPath(coopPath(mine, "player" + player), "user");
    System.IO.Directory.CreateDirectory(saves);
    if (!coopLinkFolder(coopPath(inst, "user"), saves)) {
        Context.Log("OoT Co-op: player " + player + ": could not link " + inst + "\\user to " + saves);
    }

    // The player's key (the Lobby's mutes and bans follow it): player 1's own, the others' kept with their saves.
    var keyFile = player === 1 ? coopPath(game, "player-key.txt")
                               : coopPath(coopPath(mine, "player" + player), "player-key.txt");
    if (coopTrim(coopRead(keyFile)) === "") {
        coopWrite(keyFile, coopNewKey() + "\r\n");
    }
    coopWrite(coopPath(inst, "player-key.txt"), coopRead(keyFile));

    // coop.ini: the server on this PC, the session's join password, the player's name.
    coopWrite(coopPath(inst, "coop.ini"),
              "# Written by the Nucleus Co-op handler (tools/nucleus/OoTCoop.js) at each start.\r\n" +
              "server = 127.0.0.1\r\nport = " + COOP_PORT + "\r\nname = " + coopName(Context.Nickname, player) + "\r\n" +
              "password = " + coopSession.password + "\r\n");

    // coop-options.ini: the kit's (else the game's) switches, with this game's controller and keyboard. The game
    // reads it beside longshot.exe, as no kit is above the instance folder (CoopTitle::ApplyKitOptions).
    var options = coopRead(kit !== "" ? coopPath(kit, "coop-options.ini") : coopPath(game, "coop-options.ini"));
    options = coopDropKeys(options, ["LICHII_COOP_PAD", "LICHII_COOP_KEYBOARD"].concat(player === 1 ? [] : ["color"]));
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
    coopWrite(coopPath(inst, "coop-options.ini"),
              "# Written by the Nucleus Co-op handler (tools/nucleus/OoTCoop.js) at each start.\r\n" + options + "\r\n");

    // Longshot's own options: in a window (Nucleus places it), no Discord status from every copy.
    var lsOptions = coopPath(inst, "longshot_options.ini");
    if (System.IO.File.Exists(lsOptions)) {
        var o = coopSetKey(coopRead(lsOptions), "fullscreen", "0");
        coopWrite(lsOptions, coopSetKey(o, "discord_status", "0") + "\r\n");
    }
};
