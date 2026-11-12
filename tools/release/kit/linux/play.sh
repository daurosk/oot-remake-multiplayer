#!/usr/bin/env bash
# Writes emulator/user/config/coop.ini (asking for what is missing) and starts the emulator.
# The Linux kit's play.cmd (Windows: scripts/play.ps1; the same rules and coop.ini).
# Usage: ./play.sh                    asks the first time, then reuses the answers
#        ./play.sh change             asks again
#        ./play.sh --server 192.168.1.20 --name Saria [--port 27510] [--password PW]   no questions
#        ./Host.sh (play.sh host)     starts this kit's server too and plays on it (127.0.0.1);
#                                     a new join password each time, and an INVITE CODE to send
#        ./Join.sh (play.sh join)     asks for the host's INVITE CODE or address each time
# On every start it looks for a newer co-op release and, as auto_update in coop-options.ini
# says, updates the kit and starts the new kit's play.sh with --no-update.
set -u
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$KIT/scripts/kit-lib.sh"
. "$KIT/scripts/hosting-lib.sh"
mode='' server='' name='' port='' password='' password_given=0 no_update=0
while (($#)); do
    case $1 in
        host | join | change) mode=$1 ;;
        --server | -Server) server=${2-}; shift ;;
        --name | -Name) name=${2-}; shift ;;
        --port | -Port) port=${2-}; shift ;;
        --password | -Password) password=${2-}; password_given=1; shift ;;
        --no-update | -NoUpdate) no_update=1 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
    shift
done
emu=$KIT/emulator
app=$emu/lichii.AppImage
ini=$emu/user/config/coop.ini
user=$emu/user
options=$KIT/coop-options.ini
helper_port=27512
[[ -f $app ]] || { echo "Not found: $app"; exit 1; }

# Yes or no, yes when no key comes in $2 seconds (an unattended start still updates).
read_yes_no() {
    local answer
    if [[ -t 0 ]]; then
        printf '%s [Y/n] (yes in %s s) ' "$1" "$2"
        read -r -t "$2" -n 1 answer || answer=''
        echo
    else
        echo "$1 [Y/n]"
        read -r answer || answer=''
    fi
    [[ ! $answer =~ ^[nN] ]]
}
# Waits for a key, $1 seconds at most, so a message is read before the game window covers it.
wait_key() {
    [[ -t 0 ]] || return 0
    printf 'Press a key to go on (goes on in %s s) ' "$1"
    read -r -s -t "$1" -n 1 _ || true
    echo
}

# A newer co-op release? One short question to GitHub (update.sh --status), 15 s at most.
# No answer is said in one line with what to do, and this kit starts as it is.
if ((!no_update)) && [[ -f $KIT/update.sh ]]; then
    status=$(timeout 15 bash "$KIT/update.sh" --status 2>/dev/null | head -n1)
    (($? == 124)) && status='failed GitHub did not answer in 15 s'
    if [[ $status == 'failed '* ]]; then
        echo "Could not check for a new co-op version: ${status#failed }."
        echo 'Every player needs the same version. To check again, run ./update.sh; releases: https://github.com/daurosk/oot-remake-multiplayer/releases'
    fi
    if [[ $status =~ ^(newer\ ([^ ]+)|unfinished)$ ]]; then
        if [[ -n ${BASH_REMATCH[2]} ]]; then what="Co-op ${BASH_REMATCH[2]} is out."; else what='An update of this kit did not finish.'; fi
        go=1
        case $(kit_auto_update "$options") in
            yes) echo "$what Updating (auto_update = yes in coop-options.ini)." ;;
            no) echo "$what Close the games and run ./update.sh to update (auto_update = no)."; go=0 ;;
            *) read_yes_no "$what Update now? Your saves, keys and settings are kept." 10 || go=0 ;;
        esac
        if ((go)); then
            # The update replaces this script, so it runs from a copy; then the new play.sh starts.
            copy=$(mktemp "${TMPDIR:-/tmp}/oot-coop-update-XXXXXX.sh")
            cp "$KIT/update.sh" "$copy"
            bash "$copy" --kit "$KIT" --launch
            code=$?
            rm -f "$copy"
            if ((code == 3)); then
                next=(--no-update)
                [[ -n $mode ]] && next+=("$mode")
                [[ -n $server ]] && next+=(--server "$server")
                [[ -n $name ]] && next+=(--name "$name")
                [[ -n $port ]] && next+=(--port "$port")
                ((password_given)) && next+=(--password "$password")
                exec bash "$KIT/play.sh" "${next[@]}"
            fi
            if ((code != 0)); then
                echo 'The update did not happen (above: why, and how to update by hand); this version starts as it is.'
                wait_key 60
            fi
        fi
    fi
fi

coop_read_ini "$ini"
ask=0
[[ $mode == change ]] && ask=1
server_given=0
[[ -n $server ]] && server_given=1
ask_server=$ask
[[ $mode == join && $server_given == 0 ]] && ask_server=1
[[ $mode == host ]] && server=127.0.0.1
[[ -z $server ]] && server=$COOP_SERVER
[[ $mode == join && $server_given == 0 && $server =~ ^(127\.|localhost) ]] && server=''
[[ -z $name ]] && name=$COOP_NAME
[[ -z $port ]] && port=${COOP_PORT:-@PORT@}

# An invite code (hosting-lib.sh): the host's internet address and the join password. The
# join helper (coop_server --join) carries the game's traffic; the game talks to it here.
invite=0
use_invite() {
    if kit_invite_decode "$1"; then
        invite=1 server=127.0.0.1 port=$helper_port
        return 0
    fi
    echo "Not used: $INV_ERROR."
    return 1
}
if ((ask_server)) || [[ -z $server ]]; then
    echo "Paste the INVITE CODE the host sent you (Ctrl+Shift+V in most terminals), or the host's"
    echo 'address. The host sees both when the server starts. Enter keeps the one in [ ].'
    while :; do
        read -r -p "Invite code or host's address${server:+ [$server]}: " answer || exit 1
        typed=${answer:-$server}
        if kit_is_invite "$typed"; then
            use_invite "$typed" && break
            continue
        fi
        if [[ $typed == *@* ]]; then
            echo 'A relay join code (CODE@relay) works only in the Windows kit for now; ask the host for the invite code or address.'
            continue
        fi
        if parsed=$(kit_join_address "$typed") && coop_test_server "${parsed%% *}"; then
            server=${parsed%% *}
            if [[ -n ${parsed#* } ]]; then port=${parsed#* }; elif [[ -n $answer ]]; then port=@PORT@; fi
            break
        fi
        echo "'$typed' is not an address. It looks like 26.12.34.56 or 192.168.1.20 (four numbers"
        echo 'with dots), sometimes with :port at the end. Ask the host to copy it from their server lines.'
    done
elif ((server_given)) && kit_is_invite "$server"; then
    use_invite "$server" || exit 1
fi
if ((ask)) || [[ -z $name ]]; then
    while :; do
        read -r -p "Your player name, 1 to 16 characters${name:+ [$name]}: " answer || exit 1
        [[ -n $answer ]] && name=$(echo "$answer" | xargs)
        coop_test_name "$name" && break
        echo '1 to 16 characters, and different from the other player.'
    done
fi
coop_test_server "$server" || { echo "Bad server address: $server"; exit 1; }
coop_test_name "$name" || { echo "Bad name: $name"; exit 1; }
[[ $port =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || { echo "Bad port: $port"; exit 1; }

# The join password (protocol 9). Host.sh: server_password in coop-options.ini, or with no
# such line a new random one for this session, put into the invite code by the server. Join.sh
# and play.sh change: asked each time, Enter keeps the saved one, '-' clears it.
kit_server_access "$options"
((password_given)) || password=$COOP_PASSWORD
if ((invite)); then
    password=$INV_PASSWORD
elif ((password_given)); then
    :
elif [[ $mode == host ]]; then
    password=$KA_PASSWORD
    if ((!KA_PASSWORD_SET)); then
        password=$(kit_new_password)
        export OOT_COOP_SESSION_PASSWORD=$password
        echo 'Join password: a new random one for this session (it is inside the invite code; nobody types it).'
    elif [[ -z $password ]]; then
        echo 'coop-options.ini says no join password (server_password =): no invite code and no router forward.'
        echo 'Anyone who finds your server can join. Delete that line to get a password and an invite code.'
    elif ! coop_test_password "$password"; then
        echo 'coop-options.ini: server_password has a space or is longer than 128 characters; the server was not started.'
        exit 1
    fi
elif [[ $mode == join ]] || ((ask)); then
    echo 'Join password: only if the host gave you one. Enter keeps the one in [ ], - for none.'
    while :; do
        read -r -p "Join password${password:+ [saved]}: " answer || exit 1
        answer=${answer#"${answer%%[![:space:]]*}"}
        answer=${answer%"${answer##*[![:space:]]}"}
        if [[ $answer == - ]]; then password=''; elif [[ -n $answer ]]; then password=$answer; fi
        [[ -z $password ]] || coop_test_password "$password" && break
        echo 'A join password has no spaces and at most 128 characters. Ask the host for it again.'
        password=''
    done
fi
if [[ -n $password ]] && ! coop_test_password "$password"; then echo 'coop.ini: the join password has a space or is too long.'; exit 1; fi
# Host.sh with a server of an earlier Host.sh still running: it has another session's password,
# so this game could not join it. Stop before coop.ini gets the new one.
if [[ $mode == host && -n ${OOT_COOP_SESSION_PASSWORD-} ]] && pgrep -f -- "$KIT/server/coop_server --port" >/dev/null 2>&1; then
    echo 'The server from this kit is still running from an earlier Host.sh. Stop it with'
    echo './start-server.sh --stop, then start Host.sh again.'
    exit 1
fi

coop_write_ini "$KIT/scripts/coop.ini.template" "$ini" "$server" "$port" "$name" "$password"
echo "Co-op: server $server, port $port, name $name, join password $([[ -n $password ]] && echo set || echo none)   (./play.sh change to change the name)"

if ! compgen -G "$user/keys/*.keys" >/dev/null; then
    echo "No keys in $user/keys yet. SETUP.md says what goes where."
fi
if pgrep -f -- "$app" >/dev/null 2>&1; then
    echo 'This emulator is already running; close it first to use new settings.'
    exit 1
fi
# A copied kit: point the emulator's saved folders at this copy.
fixed=$(kit_repair_paths "$user")
[[ -n $fixed ]] && echo "This kit was copied: pointed $(echo $fixed | tr ' ' ',') at $user"
# No code or cheat mods with co-op (issue 136): stop before anything starts, and say where.
mapfile -t mods < <(kit_mods "$user")
if ((${#mods[@]})); then
    echo
    echo "These mods for Ocarina of Time change the game's code or are cheats:"
    printf '  %s\n' "${mods[@]}"
    echo 'Code and cheat mods are not supported in co-op; texture and model mods are fine.'
    echo 'Remove those folders, or turn them off: in the emulator, right-click the game,'
    echo 'Properties, Add-Ons, untick them. Then start again.'
    exit 1
fi

# Invite code: the join helper, in the background, logging to logs/join-<date>.log. The join
# password goes to it in the environment only, never on a command line or in a log. It stops
# when the emulator closes.
helper_pid='' join_log=''
if ((invite)); then
    if pgrep -f -- "$KIT/server/coop_server" >/dev/null 2>&1; then
        echo 'A co-op program of this kit (a join helper or a server) is still running. Stop it'
        echo '(./start-server.sh --stop, or pkill -f coop_server), then start Join.sh again.'
        exit 1
    fi
    mkdir -p "$KIT/logs"
    join_log=$KIT/logs/join-$(date +%Y%m%d-%H%M%S).log
    helper=("$KIT/server/coop_server" --join "$INV_ADDRESS:$INV_PORT" --game-port "$helper_port" --password-env OOT_COOP_JOIN_PASSWORD)
    IFS=, read -r -a stun <<<"$KA_STUN"
    for s in "${stun[@]}"; do
        s=${s//[[:space:]]/}
        [[ $s =~ ^[A-Za-z0-9._-]{1,253}(:[0-9]{1,5})?$ ]] && helper+=(--stun "$s")
    done
    [[ -x ${helper[0]} ]] || chmod +x "${helper[0]}" 2>/dev/null
    OOT_COOP_JOIN_PASSWORD=$INV_PASSWORD nohup "${helper[@]}" </dev/null >"$join_log" 2>&1 &
    helper_pid=$!
    echo "Invite code accepted. The join helper runs in the background (log: logs/${join_log##*/})."
    echo "If the host's router does not let the game in directly, this terminal shows an ANSWER CODE:"
    echo 'send it to the host, who gives it to their server.'
    sleep 1
fi

if [[ $mode == host ]]; then
    if pgrep -f -- "$KIT/server/coop_server --port" >/dev/null 2>&1; then
        echo 'The server from this kit is already running.'
    else
        server_args=("$port" --background)
        if [[ -n $KA_ALLOW ]]; then
            [[ $KA_ALLOW =~ ^[0-9./,\ ]+$ ]] || { echo "coop-options.ini: server_allow '$KA_ALLOW' is not a list of IPv4 addresses (25.1.2.3,26.0.0.0/8); the server was not started."; exit 1; }
            server_args+=(--allow "${KA_ALLOW// /}")
        fi
        if [[ -n $KA_BIND ]]; then
            [[ $KA_BIND =~ ^[0-9.]+$ ]] || { echo "coop-options.ini: server_bind '$KA_BIND' is not an IPv4 address (25.1.2.3); the server was not started."; exit 1; }
            server_args+=(--bind "$KA_BIND")
        fi
        mkdir -p "$KIT/logs"
        # start-server.sh --background writes its terminal lines here (only for you: they hold
        # the join password) and removes the file when the server stops.
        out=$KIT/logs/server-console.txt
        rm -f "$out" "$KIT/logs/invite-code.txt"
        echo "Starting the server in the background (UDP port $port); its log: logs/server-*.log."
        echo 'It stops with ./start-server.sh --stop, or when this PC shuts down.'
        nohup bash "$KIT/start-server.sh" "${server_args[@]}" >/dev/null 2>&1 &
        # The first lines: the join password, UPnP and the invite code (10 s at most).
        for ((i = 0; i < 20; i++)); do
            sleep 0.5
            grep -q 'invite code: \|invite: no invite code\|server exited\|was not started' "$out" 2>/dev/null && break
            ((KA_INVITE)) && [[ -n $password ]] || ((i < 3)) || break
        done
        grep -v '^Join password for this session\|^Co-op server on\|^  \|^Players on this PC' "$out" | grep -i 'upnp\|invite\|password\|not started\|exited' | grep -v 'invite code: [A-Z2-9]' | sed 's/^/  server: /'
        if [[ -f $KIT/logs/invite-code.txt ]]; then
            code=$(head -n1 "$KIT/logs/invite-code.txt")
            echo
            echo "INVITE CODE (send it to your friends; they paste it into Join.sh or Join.bat):"
            echo "  $code"
            kit_copy "$code" && echo '  (copied to the clipboard)'
            echo 'It is also in logs/invite-code.txt while the server runs. An ANSWER CODE from a friend:'
            echo '  ./start-server.sh --answer <code>'
            echo
        fi
    fi
fi

# Switches from coop-options.ini (the lines without a '#' in front). A relative path that
# exists in the kit becomes absolute (the emulator runs in emulator/).
while IFS= read -r kv; do
    key=${kv%%=*} value=${kv#*=}
    if [[ -n $value && $value != /* && -e $KIT/$value ]]; then value=$KIT/$value; fi
    export "$key=$value"
    echo "Option: $key=$value"
done < <(kit_read_options "$options")

pack=$(kit_load_dir "$user")/$KIT_GAME_ID/coop-puppet/coop_puppet.json
if [[ -f $pack ]]; then
    echo 'Puppet pack: found.'
else
    echo 'Puppet pack: not made yet, so the other players show only as markers.'
    echo '  Make it once with ./make-puppet-pack.sh (SETUP.md, "The puppet pack").'
    if [[ -n ${LICHII_COOP_PUPPET-} ]]; then unset LICHII_COOP_PUPPET; echo 'Option: LICHII_COOP_PUPPET off (no pack)'; fi
fi

# The emulator uses user/ in the folder it is started in (portable), so start it in emulator/.
[[ -x $app ]] || chmod +x "$app" 2>/dev/null
cd "$emu" || exit 1
started=$(date +%s)
nohup "./lichii.AppImage" >/dev/null 2>&1 &
game=$!
echo 'Emulator started. Load the game; the co-op panel is in the top-left corner of the game.'
# The join helper ends with the emulator.
if [[ -n $helper_pid ]]; then
    nohup bash -c 'while kill -0 "$1" 2>/dev/null; do sleep 3; done; kill "$2" 2>/dev/null' _ "$game" "$helper_pid" >/dev/null 2>&1 &
fi

# Join.sh: watch the emulator's log until the join worked or clearly did not, and say so in
# plain words. Ends when the emulator closes, after 15 minutes, or once connected; Ctrl+C here
# does not touch the game.
[[ $mode == join ]] || exit 0
[[ -t 1 ]] || exit 0
log=$user/log/eden_log.txt
if ((invite)); then shown='the host (through the join helper)'; else shown=$server:$port; fi
echo "This terminal says when you are connected to $shown (Ctrl+C stops watching; the game goes on)."
trying='' helped=0 answer_shown=''
while kill -0 "$game" 2>/dev/null && (($(date +%s) - started < 900)); do
    sleep 2
    if [[ -n $join_log && -z $answer_shown ]] && grep -q 'answer code: ' "$join_log" 2>/dev/null; then
        answer_shown=$(grep -o 'answer code: [A-Z2-9-]*' "$join_log" | tail -n1)
        answer_shown=${answer_shown#answer code: }
        echo
        echo "ANSWER CODE (send it to the host; on Linux they run ./start-server.sh --answer <code>,"
        echo "on Windows they paste it into the server window):"
        echo "  $answer_shown"
        kit_copy "$answer_shown" && echo '  (copied to the clipboard)'
    fi
    state=''
    if [[ -f $log ]] && (($(stat -c %Y "$log" 2>/dev/null || echo 0) >= started - 2)); then state=$(kit_join_state "$log"); fi
    case $state in
        welcomed) echo "Connected to $shown. Have fun!"; exit 0 ;;
        refused:*)
            echo "The host's server said no: ${state#refused: }."
            echo 'name taken: pick another name with ./play.sh change. wrong protocol version: everyone runs ./update.sh (Update.bat).'
            echo 'wrong join password: start Join.sh again and type the password the host sent you (- for none).'
            exit 1 ;;
        trying)
            if [[ -z $trying ]]; then trying=$(date +%s)
            elif ((!helped && $(date +%s) - trying >= 30)); then
                echo
                if ((invite)); then
                    echo "Not connected yet. The join helper's log (logs/${join_log##*/}) says what is going on."
                else
                    echo 'Not connected yet. The three most likely reasons:'
                    echo "  1. Wrong address. Ask the host to read it again from their server lines. You typed: $shown"
                    echo "  2. The host's firewall: the host opens UDP port $port (Linux: sudo ufw allow $port/udp)."
                    echo "  3. Not the same network, or the host's router does not forward UDP port $port. Easier:"
                    echo '     the host sends you the invite code from Host.sh, or you all join one VPN network (PLAY-ONLINE.md).'
                fi
                echo 'The game keeps trying by itself; this terminal says when it connects.'
                helped=1
            fi ;;
    esac
done
exit 0
