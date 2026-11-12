#!/usr/bin/env bash
# Joins a game: asks for the host's INVITE CODE or address (Enter keeps the last one), the join
# password when you give an address, and your player name the first time, then starts the
# emulator connected to that server and says when you are connected.
exec bash "$(dirname "${BASH_SOURCE[0]}")/play.sh" join "$@"
