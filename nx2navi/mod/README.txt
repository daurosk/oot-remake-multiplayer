OoT Co-op for NX2navi
=====================

Install: put this folder ("OoT Co-op") into the game's mods folder, so that
<game folder>\mods\OoT Co-op\mod.json exists, and enable the mod in the launcher.

Play, from inside the game (an NX2navi build with plugin interface 3 or newer):
the game's title screen has one more entry, "Multiplayer" ("Multijoueur" in a
French game). It opens a page of the game's own menu:
  Host over Local Network   your name, the port, an optional join password and
        the address to give to your friends (this PC's home network address
        first; the caption says what each address is); Start starts the server on
        this PC (no window; it stops with the game) and the session. Then load
        your save with Continue.
  Join over Local Network   your name, the host's address, the port, the
        password; Join connects, takes the host's save and restarts the game
        on it: choose Continue and you stand next to the host. Your own saves
        are never touched (the host's save goes into coop-save in this folder).
  Host / Join over Steam    done inside the game: title screen, Multiplayer.
Typing: choose a field, type on the keyboard, Enter confirms, Esc cancels. With
a gamepad alone: up and down change the last character, right adds one, left
removes one, A confirms, B cancels.

Play, from the launcher: its home screen gets a row "Multiplayer".
  Host  starts the co-op server on this PC (its own window) and the game.
        "Addresses For Friends" shows what your friends type.
  Join  asks for the host's address (and the join password, if the host set
        one) and starts the game connected to it. "Use The Host's Save" (on)
        does what the in-game Join does: the game starts, takes the host's
        save and starts once more on it.
Every player needs the same game version (1.1.1) and the same co-op build.
The host opens UDP port 27510 (or the port you set); players who join open
nothing. Prefer a virtual LAN (Radmin VPN, Hamachi, Tailscale) to port
forwarding, and set a join password.

What works with every NX2navi build: joining a session, your position, scene
and animation sent to the others, and the others' joins, leaves, chat and
positions written to coop-nx2navi.log in this folder. Players on the
emulator build see you.

With an NX2navi build that has its plugin interface (newer builds), the game
also shows a co-op panel at the bottom left: the connection, where you are, and
each other player's scene; in your scene also the distance and a direction
(north, north-east, ...). The direction uses the world's axes (north taken
as world -Z), not your camera. Joins, leaves and chat show for 6 seconds.
  F9     hides or shows the panel
  Enter  opens the chat box; type, Enter sends, Esc cancels, Backspace
         deletes (the game gets no input while the box is open)
"In-Game Overlay" in the Host and Join menus turns the panel off (coop.ini:
overlay = 0). An older NX2navi build plays without the panel; the log says
"this game has no plugin interface".
With the same builds the other players in your area are drawn as a second
Link ("Show Other Players"; the first Host or Join makes the models from your
own game files), and your game can go to another player:
  /teleport <name>   in the chat box takes you to that player, in their area
                     if it is another one (/teleport alone lists the players)
  "Join At The Host" puts you next to the host once, when you get into the
                     game while the host is playing (coop.ini: join_at_host)
  "Follow The Host Through Exits": when the host walks into another area,
                     your game loads it too, at the entrance the host took
                     (coop.ini: follow = host, anyone or off)
  After a Game Over your game brings you back to the host by itself.
Only the host picks a save: a player who joins gets the host's newest save
(what Continue loads for the host) before loading anything. If the host has no
save yet, each player loads one of their own and is taken to the host.
With plugin interface 4 each other player's name shows over their head, in
their colour (coop.ini: name_tags = 0 turns it off).
"Shared Enemies" is on (every player should use the same setting): an enemy has
the same place, life and death in every player's game, your hits on another
player's enemy count, and an enemy goes for the player whose game runs it (the
one who met it, or the one clearly nearer once it is out of a fight). Boss
fights with it were not tried yet; turn it off if one misbehaves. Townspeople,
horses and cows are each game's own. Not in this port: the other players' Navi,
shared objects and drops (each game drops its own). "Shared Progress
(experimental)" is off unless you turn it on; it shares items only, writes into
your running save and is not verified: back up your save before you turn it on.

The game started with the launcher's normal Play loads the mod's DLL too, and
it does nothing (no connection, nothing read from the game) until you host or
join from the title screen's Multiplayer entry. NX2navi's own co-op (its Co-op
tab) is another system; do not use both in one game.

Files this folder gets while you play: coop.ini and settings.json (the menus'
settings; a password typed in the game is kept protected for your Windows
user, never as clear text), coop-nx2navi.log (the game side's log),
coop-server.log (the server a host started in the game), coop-save\ (the
host's save of the session you joined; your own saves are elsewhere and are
not touched). Shipped: bin\coop_nx2navi.dll, bin\oot-coop-server.exe.

Source and licence: https://github.com/daurosk/oot-remake-multiplayer (GPL-3.0,
LICENSE.txt). ENet is under the MIT licence, NX2navi's plugin_api.h under the
permission in its own header (THIRD_PARTY_NOTICES.md).
This is a fan project, not affiliated with or endorsed by Nintendo.
