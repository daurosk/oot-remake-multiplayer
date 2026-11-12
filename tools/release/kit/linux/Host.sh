#!/usr/bin/env bash
# Hosts a game: starts the co-op server from this kit in the background, then the emulator
# connected to it. The first time it asks for your player name. Each time it makes a new
# join password and shows an INVITE CODE (copied to the clipboard when it can): send that to
# your friends, who paste it into Join.sh or Join.bat. Players on your network or VPN can also
# join with this PC's address (SETUP.md, PLAY-ONLINE.md).
exec bash "$(dirname "${BASH_SOURCE[0]}")/play.sh" host "$@"
