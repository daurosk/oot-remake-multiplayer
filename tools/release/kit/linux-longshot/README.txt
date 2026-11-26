Squadala! for Linux and Steam Deck - online co-op: play Ocarina of Time together, each player on their own PC with their own copy of the game.
EXPERIMENTAL: not yet tested on a real Linux PC or Steam Deck. Please tell us what happens: /bugreport on the Squadala! Discord (https://discord.gg/GwtRKAqng5) with the zip from bash start-squadala.sh --logs.
1. Unzip this folder to a place of your own, such as ~/Squadala (not a temporary folder).
2. Open a terminal in that folder and type:  bash start-squadala.sh
   The first time it asks for your game file (drag your .nup into the terminal and press Enter) and builds the game
   from it: 15 to 60 minutes, about 60 GB free, once. After that it starts in seconds, also from the "Squadala"
   entry it adds to your applications menu.
3. In the game: Host or Join in the menu (Multiplayer). Host gives you a code to send your friends; Join takes a friend's code.
Always start with "bash start-squadala.sh", never the longshot program in data/game: you do not need your own Longshot build (the kit builds its own game, with co-op inside), and a game started by hand has no co-op or only part of it.
Steam Deck: do steps 1 and 2 in Desktop Mode (Konsole). Then in Steam: Games > "Add a Non-Steam Game to My Library" > Squadala.
   In Game Mode, Steam + X opens the keyboard for chat and codes.
It updates itself (bash revert.sh goes back); every player needs the same version, on Linux or Windows. Needs a 64-bit PC with Vulkan 1.3 graphics.
Something wrong? The terminal says what to do. Others only names, no Link? bash start-squadala.sh --pack makes the puppet pack again. NVIDIA RTX card? bash start-squadala.sh --dlss builds the game with DLSS (asks for NVIDIA's license once). More help: data/docs/SETUP-LINUX.md. Logs for a bug report: bash start-squadala.sh --logs. Problem reports (masked; sent to the Squadala! team after a game with a problem only if you said yes): bash start-squadala.sh --problem-reports send, nosend, on or off.
Nothing of the game is in this kit, and nothing of yours leaves your PC except the co-op traffic while you play and, only if you said yes, the masked problem reports (--problem-reports off stops them). Nothing goes to Longshot.
Licence: GPL-3.0 (data/licenses/). The source: data/licenses/SOURCE.txt.
