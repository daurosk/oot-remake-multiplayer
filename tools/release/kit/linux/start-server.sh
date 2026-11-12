#!/usr/bin/env bash
# Runs the co-op server from this kit and writes its output to logs/server-<date>.log.
# The Linux kit's start-server.cmd (Windows: scripts/start-server.ps1; the same rules).
# Usage: ./start-server.sh [port] [--allow LIST] [--bind ADDRESS] [more coop_server options]
#        ./start-server.sh --answer CODE   gives a friend's ANSWER CODE to the running server
#        ./start-server.sh --stop          stops this kit's server (Host.sh starts it in the background)
# The join password: server_password in coop-options.ini, or (no such line, the usual way) a
# new random one for this session, which goes into the INVITE CODE. With a password the server
# also asks the router to forward its port (UPnP) and shows the invite code (upnp = no and
# invite = no in coop-options.ini turn them off). The invite code holds the password, so it is
# shown here and kept in logs/invite-code.txt (only for you) while the server runs, never in
# the server's log.
set -u
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$KIT/scripts/kit-lib.sh"
. "$KIT/scripts/hosting-lib.sh"
exe=$KIT/server/coop_server
input=$KIT/logs/.server-input
invite_file=$KIT/logs/invite-code.txt
case ${1-} in
    --stop)
        if pkill -f -- "$exe --port" 2>/dev/null; then echo 'Stopped the server of this kit.'; else echo 'The server of this kit is not running.'; fi
        exit 0 ;;
    --answer)
        code=${2-}
        [[ -n $code ]] || { echo 'Usage: ./start-server.sh --answer <the ANSWER CODE your friend sent>'; exit 2; }
        if [[ -p $input ]] && pgrep -f -- "$exe --port" >/dev/null 2>&1; then
            # Opened read-write so this never waits for a reader.
            printf '%s\n' "$code" 1<>"$input"
            echo 'Given to the server. Its lines (logs/server-*.log) say whether it was used.'
            exit 0
        fi
        echo 'The server of this kit does not run in the background. Paste the code into the terminal'
        echo 'where it runs (./start-server.sh) and press Enter.'
        exit 1 ;;
esac
background=0
port=@PORT@ port_given=0 allow='' bind=''
rest=()
while (($#)); do
    case $1 in
        --background) background=1 ;;
        --allow) allow=${2-}; shift ;;
        --bind) bind=${2-}; shift ;;
        *) if [[ $1 =~ ^[0-9]+$ && ${#rest[@]} == 0 && $port_given == 0 ]]; then port=$1 port_given=1; else rest+=("$1"); fi ;;
    esac
    shift
done
[[ -f $exe ]] || { echo "Not found: $exe"; exit 1; }
[[ -x $exe ]] || chmod +x "$exe" 2>/dev/null
((10#$port >= 1 && 10#$port <= 65535)) || { echo 'The port must be 1 to 65535.'; exit 1; }

# In the background (Host.sh), the lines below go to logs/server-console.txt, only for you.
console=$KIT/logs/server-console.txt
if ((background)); then
    mkdir -p "$KIT/logs"
    rm -f "$console"
    (umask 077 && : >"$console")
    exec >"$console" 2>&1
fi
kit_server_access "$KIT/coop-options.ini"
[[ -z $allow ]] && allow=$KA_ALLOW
[[ -z $bind ]] && bind=$KA_BIND
access=()
IFS=, read -r -a ranges <<<"$allow"
for a in "${ranges[@]}"; do
    a=${a//[[:space:]]/}
    [[ -z $a ]] && continue
    [[ $a =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}(/[0-9]{1,2})?$ ]] || { echo "server_allow: '$a' is not an IPv4 address (25.1.2.3) or range (26.0.0.0/8)."; exit 1; }
    access+=(--allow "$a")
done
bind=${bind//[[:space:]]/}
if [[ -n $bind ]]; then
    [[ $bind =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || { echo "server_bind: '$bind' is not an IPv4 address (25.1.2.3)."; exit 1; }
    access+=(--bind "$bind")
fi

password=$KA_PASSWORD made_password=0
if ((!KA_PASSWORD_SET)); then
    password=${OOT_COOP_SESSION_PASSWORD:-$(kit_new_password)}
    made_password=1
fi
if [[ -n $password ]]; then
    coop_test_password "$password" || { echo 'server_password: no spaces, at most 128 characters. The server was not started.'; exit 1; }
    export OOT_COOP_SERVER_PASSWORD=$password
    access+=(--password-env OOT_COOP_SERVER_PASSWORD)
fi
kit_hosting_args "$password"
access+=("${HOSTING_ARGS[@]}")
[[ -n $KA_RELAY_KEY ]] && export OOT_COOP_RELAY_KEY=$KA_RELAY_KEY
((KA_PVP)) && access+=(--pvp on)

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
if ((made_password)); then
    echo 'Join password: a new random one for this session; it is inside the invite code (server_password in coop-options.ini sets your own).'
    echo "Join password for this session: $password   (only needed when a friend joins by address instead of the invite code)"
elif [[ -n $password ]]; then
    echo 'Join password: on (server_password in coop-options.ini). Send it to your friends with the address.'
else
    echo 'Join password: none (set one with server_password in coop-options.ini).'
fi
if ((KA_PVP)); then echo 'PvP: on (pvp in coop-options.ini): players can hurt each other. /pvp off in the game chat turns it off.'
else echo 'PvP: off (players cannot hurt each other; pvp = on in coop-options.ini, or /pvp on in the game chat).'; fi
for line in "${HOSTING_NOTES[@]}"; do echo "$line"; done
if ((background)); then echo 'It runs in the background: ./start-server.sh --stop stops it.'; else echo 'Ctrl+C stops the server.'; fi
echo

# This kit's co-op release (VERSION.txt), named when the server refuses a game of another
# version, so the players know which one to update to.
release=''
if [[ -f $KIT/VERSION.txt ]]; then
    IFS= read -r first <"$KIT/VERSION.txt"
    [[ $first =~ \(release\ ([^\)]+)\) ]] && release=${BASH_REMATCH[1]}
fi

# In the background (Host.sh) the server reads typed lines (answer codes) from a named pipe
# that ./start-server.sh --answer writes to; opened read-write, so it never ends.
if ((background)); then
    rm -f "$input"
    mkfifo -m 600 "$input" && exec 0<>"$input"
fi
rm -f "$invite_file"
trap 'rm -f "$invite_file"; if ((background)); then rm -f "$input" "$console"; fi' EXIT

"$exe" --port "$port" "${access[@]}" "${rest[@]}" 2>&1 | while IFS= read -r text; do
    if [[ -n $release && $text =~ refused\ a\ connection,\ reason\ 1([^0-9]|$) ]]; then
        text+=". This server is co-op $release: every player needs $release (./update.sh updates to the newest release)."
    fi
    logged=$text
    if [[ $text =~ invite\ code:\ ([A-Z2-9-]+) ]]; then
        code=${BASH_REMATCH[1]}
        (umask 077 && printf '%s\n' "$code" >"$invite_file")
        kit_copy "$code" && text+='   (copied to the clipboard)'
        logged='coop_server: invite code: (not in the log: it holds the join password; logs/invite-code.txt has it)'
    fi
    printf '%s %s\n' "$(date +%H:%M:%S.%3N)" "$text"
    printf '%s %s\n' "$(date +%H:%M:%S.%3N)" "$logged" >>"$log"
done
code=${PIPESTATUS[0]}
printf '%s (server exited with code %s)\n' "$(date +%H:%M:%S.%3N)" "$code" >>"$log"
exit "$code"
