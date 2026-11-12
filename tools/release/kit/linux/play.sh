#!/usr/bin/env bash
# Writes emulator/user/config/coop.ini (asking for what is missing) and starts the emulator.
# The Linux kit's play.cmd (Windows: scripts/play.ps1; the same rules and coop.ini).
# Usage: ./play.sh                    asks the first time, then reuses the answers
#        ./play.sh change             asks again
#        ./play.sh --server 192.168.1.20 --name Saria [--port 27510]   no questions
#        ./Host.sh (play.sh host)     starts this kit's server too and plays on it (127.0.0.1)
#        ./Join.sh (play.sh join)     asks for the host's address each time
# On every start it looks for a newer co-op release and, as auto_update in coop-options.ini
# says, updates the kit and starts the new kit's play.sh with --no-update.
set -u
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$KIT/scripts/kit-lib.sh"
mode='' server='' name='' port='' no_update=0
while (($#)); do
    case $1 in
        host | join | change) mode=$1 ;;
        --server | -Server) server=${2-}; shift ;;
        --name | -Name) name=${2-}; shift ;;
        --port | -Port) port=${2-}; shift ;;
        --no-update | -NoUpdate) no_update=1 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
    shift
done
emu=$KIT/emulator
app=$emu/lichii.AppImage
ini=$emu/user/config/coop.ini
user=$emu/user
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

# A newer co-op release? One short question to GitHub (update.sh --status), 6 s at most.
# No network, or GitHub down: nothing is said and this kit starts as it is.
if ((!no_update)) && [[ -f $KIT/update.sh ]]; then
    status=$(timeout 6 bash "$KIT/update.sh" --status 2>/dev/null | head -n1)
    if [[ $status =~ ^(newer\ ([^ ]+)|unfinished)$ ]]; then
        if [[ -n ${BASH_REMATCH[2]} ]]; then what="Co-op ${BASH_REMATCH[2]} is out."; else what='An update of this kit did not finish.'; fi
        go=1
        case $(kit_auto_update "$KIT/coop-options.ini") in
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
                exec bash "$KIT/play.sh" "${next[@]}"
            fi
            ((code == 0)) || echo 'The update did not happen; this version starts as it is.'
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

if ((ask_server)) || [[ -z $server ]]; then
    echo 'Server address: the IP of the PC running start-server (it shows the address).'
    echo 'Use 127.0.0.1 if the server runs on this PC. host:port also works.'
    while :; do
        read -r -p "Server address${server:+ [$server]}: " answer || exit 1
        [[ -n $answer ]] && server=$(echo "$answer" | xargs)
        coop_test_server "$server" && break
        echo 'Letters, digits, dots and dashes only, optionally :port.'
    done
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

coop_write_ini "$KIT/scripts/coop.ini.template" "$ini" "$server" "$port" "$name"
echo "Co-op: server $server, port $port, name $name   (./play.sh change to change the name)"

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
if [[ $mode == host ]]; then
    if pgrep -f -- "$KIT/server/coop_server" >/dev/null 2>&1; then
        echo 'The server from this kit is already running.'
    else
        mkdir -p "$KIT/logs"
        echo "Starting the server in the background (UDP port $port); its output: logs/server-*.log."
        echo 'It stops when you close it with ./start-server.sh --stop, or when this PC shuts down.'
        nohup bash "$KIT/start-server.sh" "$port" >/dev/null 2>&1 &
        sleep 2
    fi
fi

# Switches from coop-options.ini (the lines without a '#' in front). A relative path that
# exists in the kit becomes absolute (the emulator runs in emulator/).
while IFS= read -r kv; do
    key=${kv%%=*} value=${kv#*=}
    if [[ -n $value && $value != /* && -e $KIT/$value ]]; then value=$KIT/$value; fi
    export "$key=$value"
    echo "Option: $key=$value"
done < <(kit_read_options "$KIT/coop-options.ini")

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
nohup "./lichii.AppImage" >/dev/null 2>&1 &
echo 'Emulator started. Load the game; the co-op panel is in the top-left corner of the game.'
