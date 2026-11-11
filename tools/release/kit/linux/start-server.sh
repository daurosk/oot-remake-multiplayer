#!/usr/bin/env bash
# Runs the co-op server from this kit and writes its output to logs/server-<date>.log.
# Usage: ./start-server.sh [port] [more coop_server options]
#        ./start-server.sh --stop     stops this kit's server (Host.sh starts it in the background)
set -u
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exe=$KIT/server/coop_server
if [[ ${1-} == --stop ]]; then
    if pkill -f -- "$exe" 2>/dev/null; then echo 'Stopped the server of this kit.'; else echo 'The server of this kit is not running.'; fi
    exit 0
fi
port=@PORT@
if [[ ${1-} =~ ^[0-9]+$ ]]; then port=$1; shift; fi
[[ -f $exe ]] || { echo "Not found: $exe"; exit 1; }
[[ -x $exe ]] || chmod +x "$exe" 2>/dev/null
((10#$port >= 1 && 10#$port <= 65535)) || { echo 'The port must be 1 to 65535.'; exit 1; }

mkdir -p "$KIT/logs"
log=$KIT/logs/server-$(date +%Y%m%d-%H%M%S).log
echo "Co-op server on UDP port $port. Players on other PCs use one of these addresses:"
if command -v ip >/dev/null; then
    ip -4 -o addr show 2>/dev/null | awk '$4 !~ /^(127|169\.254)\./ { split($4, a, "/"); printf "  %-16s (%s)\n", a[1], $2 }'
elif command -v hostname >/dev/null; then
    for a in $(hostname -I 2>/dev/null); do echo "  $a"; done
else
    echo '  (run "ip addr" to see them)'
fi
echo "Players on this PC use 127.0.0.1. Log: $log"
echo 'Ctrl+C stops the server.'
echo

# This kit's co-op release (VERSION.txt), named when the server refuses a game of another
# version, so the players know which one to update to.
release=''
if [[ -f $KIT/VERSION.txt ]]; then
    IFS= read -r first <"$KIT/VERSION.txt"
    [[ $first =~ \(release\ ([^\)]+)\) ]] && release=${BASH_REMATCH[1]}
fi

"$exe" --port "$port" "$@" 2>&1 | while IFS= read -r text; do
    if [[ -n $release && $text =~ refused\ a\ connection,\ reason\ 1([^0-9]|$) ]]; then
        text+=". This server is co-op $release: every player needs $release (./update.sh updates to the newest release)."
    fi
    line="$(date +%H:%M:%S.%3N) $text"
    printf '%s\n' "$line"
    printf '%s\n' "$line" >>"$log"
done
code=${PIPESTATUS[0]}
printf '%s (server exited with code %s)\n' "$(date +%H:%M:%S.%3N)" "$code" >>"$log"
exit "$code"
