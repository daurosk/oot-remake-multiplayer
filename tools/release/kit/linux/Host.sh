#!/usr/bin/env bash
# Hosts a game: starts the co-op server from this kit in the background, then the emulator
# connected to it. The first time it asks for your player name.
# Other players use Join.sh with this PC's address (SETUP.md).
exec bash "$(dirname "${BASH_SOURCE[0]}")/play.sh" host "$@"
