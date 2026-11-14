#!/bin/sh
# OoT co-op for Linux and Steam Deck: start this file. The first time it builds the game from your
# own game file (once), then it starts the game; Host and Join are in the game's menu.
# If double-clicking does nothing, open a terminal here and type:  bash start-oot-coop.sh
here=$(cd "$(dirname "$0")" && pwd)
if ! command -v bash >/dev/null 2>&1; then
    echo "OoT Co-op needs bash (install it with your system's package manager)." >&2
    exit 1
fi
exec bash "$here/scripts/start.sh" "$@"
