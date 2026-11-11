#!/usr/bin/env bash
# Joins a game: asks for the host's address (Enter keeps the last one) and your player name
# the first time, then starts the emulator connected to that server.
exec bash "$(dirname "${BASH_SOURCE[0]}")/play.sh" join "$@"
