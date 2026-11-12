# Hosting for the Linux kit: the join password, the host's coop-options.ini lines, the
# server's UPnP / invite code switches, and reading an invite code in Join.sh. The same rules
# as the Windows kit's scripts/invite.ps1, relay-join.ps1 (Get-ServerHostingArgs),
# kit-paths.ps1 (Read-KitServerAccess) and hosting.ps1 (ConvertTo-JoinAddress, Get-JoinState);
# tools/release/tests/linux_kit_test.sh checks it against the server's own invite vectors.
# Sourced by play.sh and start-server.sh; needs bash 4.

KIT_INVITE_LETTERS=ABCDEFGHJKLMNPQRSTUVWXYZ23456789

# A join password: 1 to 128 bytes, no spaces or control characters.
coop_test_password() {
    local v=$1
    [[ -n $v && ! $v =~ [[:space:][:cntrl:]] ]] || return 1
    local LC_ALL=C
    ((${#v} <= 128))
}

# A join password for this session: 20 letters from the system's random source (every
# letter as likely: tr drops the bytes that are not letters).
kit_new_password() {
    LC_ALL=C tr -dc "$KIT_INVITE_LETTERS" </dev/urandom | head -c 20
}

# kit_server_access FILE: the host's lines of coop-options.ini, in KA_* variables.
# KA_PASSWORD_SET is 0 when no line without a '#' sets server_password (then Host.sh makes
# one each time); KA_PASSWORD '' with KA_PASSWORD_SET=1 means "no password".
kit_server_access() {
    KA_ALLOW='' KA_BIND='' KA_PASSWORD='' KA_PASSWORD_SET=0 KA_UPNP=1 KA_INVITE=1 KA_STUN=''
    KA_RELAY='' KA_RELAY_KEY='' KA_PVP=0 KA_LOOKUP=1
    [[ -f $1 ]] || return 0
    local line key value
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        [[ $line =~ ^[[:space:]]*(server_allow|server_bind|server_password|upnp|invite|stun_servers|relay|relay_key|pvp|public_address_lookup)[[:space:]]*=[[:space:]]*(.*)$ ]] || continue
        key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
        value=${value%"${value##*[![:space:]]}"}
        case $key in
            server_allow) KA_ALLOW=$value ;;
            server_bind) KA_BIND=$value ;;
            server_password) KA_PASSWORD=$value KA_PASSWORD_SET=1 ;;
            upnp) [[ $value =~ ^(no|off|false|0)$ ]] && KA_UPNP=0 || KA_UPNP=1 ;;
            invite) [[ $value =~ ^(no|off|false|0)$ ]] && KA_INVITE=0 || KA_INVITE=1 ;;
            stun_servers) KA_STUN=$value ;;
            relay) KA_RELAY=$value ;;
            relay_key) KA_RELAY_KEY=$value ;;
            pvp) [[ $value =~ ^(on|yes|1|true)$ ]] && KA_PVP=1 || KA_PVP=0 ;;
            public_address_lookup) [[ $value =~ ^(no|off|false|0)$ ]] && KA_LOOKUP=0 ;;
        esac
    done <"$1"
}

# kit_hosting_args PASSWORD: the server's --upnp / --invite / --stun / --relay-via switches in
# HOSTING_ARGS and what to tell the host in HOSTING_NOTES (both arrays). UPnP, the invite
# code and the relay all need a join password. Call kit_server_access first.
kit_hosting_args() {
    local password=$1 s
    HOSTING_ARGS=() HOSTING_NOTES=()
    if ((KA_UPNP)); then
        if [[ -z $password ]]; then
            HOSTING_NOTES+=('The router forward (UPnP) is NOT used: it opens your PC to the whole internet while the server'
                'runs, so it needs a join password. coop-options.ini has an empty server_password line: delete it'
                '(Host.sh then makes a password each time) or set one.')
        else
            HOSTING_ARGS+=(--upnp)
            HOSTING_NOTES+=("UPnP: asking your router to forward the game's port while the server runs. The server's lines"
                'say whether it did; the forward ends when the server stops (upnp = no turns it off).')
        fi
    fi
    if ((KA_INVITE)); then
        if [[ -z $password ]]; then
            HOSTING_NOTES+=('No invite code: it carries the join password, and coop-options.ini says none (an empty'
                'server_password line). Delete that line to get one.')
        else
            HOSTING_ARGS+=(--invite)
            local IFS=,
            for s in $KA_STUN; do
                s=${s//[[:space:]]/}
                [[ -z $s ]] && continue
                if [[ $s =~ ^[A-Za-z0-9._-]{1,253}(:[0-9]{1,5})?$ ]]; then HOSTING_ARGS+=(--stun "$s")
                else HOSTING_NOTES+=("stun_servers: '$s' is not a name:port; not used."); fi
            done
            unset IFS
            HOSTING_NOTES+=('INVITE CODE: shows in a few seconds. Send it to your friends (only to friends: it holds your'
                'internet address and the join password); they paste it into Join.sh or Join.bat. It stops working'
                'when the server stops. If a friend gets an ANSWER CODE, run ./start-server.sh --answer <code>.')
        fi
    fi
    if [[ -n $KA_RELAY ]]; then
        if [[ ! $KA_RELAY =~ ^[A-Za-z0-9._-]{1,253}(:[0-9]{1,5})?$ ]]; then
            HOSTING_NOTES+=("relay = '$KA_RELAY' is not a relay address (a name or IPv4 address, optional :port); not used.")
        elif [[ -z $password ]]; then
            HOSTING_NOTES+=('relay = ... is NOT used: the join code only finds your server, the join password keeps'
                'strangers out, so set server_password in coop-options.ini as well (server_allow is not enough).')
        else
            HOSTING_ARGS+=(--relay-via "$KA_RELAY")
            [[ -n $KA_RELAY_KEY ]] && HOSTING_ARGS+=(--relay-key-env OOT_COOP_RELAY_KEY)
            HOSTING_NOTES+=("Relay (relay = $KA_RELAY): your join code shows in the server's lines (CODE@relay).")
        fi
    fi
    return 0
}

# What was pasted as bare invite letters (upper case; spaces, dashes, dots and quotes
# dropped), or nothing.
kit_invite_letters() {
    local t=$1
    t=${t//[[:space:]\-.\'\"]/}
    t=${t^^}
    [[ -n $t && $t =~ ^[A-HJ-NP-Z2-9]+$ ]] && printf '%s' "$t"
}

# An invite (or answer) code rather than an address or a relay join code.
kit_is_invite() {
    local l
    l=$(kit_invite_letters "$1")
    ((${#l} >= 30))
}

kit_invite_public() {  # A B C: the first three numbers of an IPv4 address
    local a=$1 b=$2 c=$3
    ((a == 0 || a == 10 || a == 127 || a >= 224)) && return 1
    ((a == 100 && (b & 0xC0) == 64)) && return 1
    ((a == 169 && b == 254)) && return 1
    ((a == 172 && (b & 0xF0) == 16)) && return 1
    ((a == 192 && b == 168)) && return 1
    ((a == 192 && b == 0 && c == 0)) && return 1
    ((a == 198 && (b & 0xFE) == 18)) && return 1
    return 0
}

# kit_invite_decode TEXT: 0 with INV_ADDRESS, INV_PORT and INV_PASSWORD set, or 1 with
# INV_ERROR (one line for the player). The server makes the codes (server/nat_codes.cpp):
# version and kind, the host's internet address and port, the join password, then the first
# 4 bytes of the SHA-256 of all that.
kit_invite_decode() {
    INV_ADDRESS='' INV_PORT='' INV_PASSWORD='' INV_ERROR=''
    local wrong='a letter of the invite code is wrong or missing; ask the host to copy it again'
    local bad='the invite code does not make sense; ask the host for a new one'
    local letters bits='' c v i j n body
    letters=$(kit_invite_letters "$1")
    if ((${#letters} < 30)); then INV_ERROR='this is not an invite code (letters A-Z and 2-9 in groups of five; paste all of it)'; return 1; fi
    n=$((${#letters} * 5 / 8))
    if (((n * 8 + 4) / 5 != ${#letters})); then INV_ERROR=$wrong; return 1; fi
    for ((i = 0; i < ${#letters}; i++)); do
        c=${letters:i:1}
        v=${KIT_INVITE_LETTERS%%"$c"*}
        v=${#v}
        for ((j = 4; j >= 0; j--)); do bits+=$(((v >> j) & 1)); done
    done
    if [[ ${bits:n*8} == *1* ]]; then INV_ERROR=$wrong; return 1; fi
    if ((n < 12)); then INV_ERROR=$wrong; return 1; fi
    local -a bytes=()
    for ((i = 0; i < n; i++)); do bytes+=($((2#${bits:i*8:8}))); done
    body=$((n - 4))
    if (((bytes[0] >> 4) == 2)); then INV_ERROR="this is an ANSWER code: it goes to the host (./start-server.sh --answer on Linux, the server window on Windows), not into Join.sh"; return 1; fi
    local escaped='' digest want=''
    for ((i = 0; i < body; i++)); do escaped+=$(printf '\\x%02x' "${bytes[i]}"); done
    digest=$(printf "$escaped" | sha256sum | cut -c1-8)
    for ((i = body; i < n; i++)); do want+=$(printf '%02x' "${bytes[i]}"); done
    if [[ $digest != "$want" ]]; then INV_ERROR=$wrong; return 1; fi
    local kind=${bytes[0]} port=$((bytes[5] * 256 + bytes[6])) password='' length
    if ((kind != 0x11 && kind != 0x12)); then INV_ERROR='the invite code is from a newer co-op version; everyone runs ./update.sh (Update.bat on Windows)'; return 1; fi
    if ((kind == 0x11)); then
        if ((body != 20 || (bytes[19] & 0x0F))); then INV_ERROR=$bad; return 1; fi
        for ((i = 0; i < 20; i++)); do password+=${KIT_INVITE_LETTERS:$((2#${bits:56+i*5:5})):1}; done
    else
        length=${bytes[7]}
        if ((body != 8 + length || length < 1)); then INV_ERROR=$bad; return 1; fi
        escaped=''
        for ((i = 8; i < 8 + length; i++)); do
            if ((bytes[i] == 0)); then INV_ERROR=$bad; return 1; fi
            escaped+=$(printf '\\x%02x' "${bytes[i]}")
        done
        password=$(printf "$escaped")
    fi
    if ((port < 1)) || [[ $password =~ [[:space:]] ]]; then INV_ERROR=$bad; return 1; fi
    if ! kit_invite_public "${bytes[1]}" "${bytes[2]}" "${bytes[3]}"; then
        INV_ERROR='the address in the invite code is not an internet address; ask the host for a new one'
        return 1
    fi
    INV_ADDRESS="${bytes[1]}.${bytes[2]}.${bytes[3]}.${bytes[4]}" INV_PORT=$port INV_PASSWORD=$password
    return 0
}

# kit_join_address TEXT: what a player pasted for the host's address, made plain (spaces,
# quotes, udp://, a trailing dot or slash dropped; localhost is 127.0.0.1). Prints
# "SERVER PORT" (PORT empty when none was given), or fails when it is not an address.
kit_join_address() {
    local t=$1 server port
    t=${t//[[:space:]\"\'<>]/}
    shopt -s nocasematch
    [[ $t =~ ^(udp|http|https):// ]] && t=${t#*://}
    shopt -u nocasematch
    while [[ $t == */ || $t == *. || $t == *, || $t == *\; ]]; do t=${t%?}; done
    [[ $t =~ ^([A-Za-z0-9._-]{1,253})(:([0-9]{1,5}))?$ ]] || return 1
    server=${BASH_REMATCH[1]} port=${BASH_REMATCH[3]}
    server=${server%.}
    [[ -n $server ]] || return 1
    if [[ -n $port ]] && ((10#$port < 1 || 10#$port > 65535)); then return 1; fi
    [[ ${server,,} == localhost ]] && server=127.0.0.1
    if [[ $server =~ ^[0-9.]+$ ]]; then
        [[ $server =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ ]] || return 1
        local k
        for k in 1 2 3 4; do ((10#${BASH_REMATCH[k]} <= 255)) || return 1; done
    fi
    printf '%s %s\n' "$server" "$port"
}

# How far a join got, from the emulator's log: welcomed, "refused: <why>", trying (co-op
# started, no answer yet) or nothing (co-op has not started yet).
kit_join_state() {
    [[ -f $1 ]] || return 0
    local line state=''
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line == *'coop: on, server '* ]]; then state=trying
        elif [[ $line == *'coop: welcomed as player'* ]]; then state=welcomed
        elif [[ $line =~ coop:\ server\ (refused:\ .*[^[:space:]])[[:space:]]*$ ]]; then state=${BASH_REMATCH[1]}
        fi
    done <"$1"
    printf '%s' "$state"
}

# Puts TEXT on the clipboard if a clipboard tool is there (Wayland or X11); fails otherwise.
kit_copy() {
    if [[ -n ${WAYLAND_DISPLAY-} ]] && command -v wl-copy >/dev/null; then printf '%s' "$1" | wl-copy 2>/dev/null && return 0; fi
    if [[ -n ${DISPLAY-} ]] && command -v xclip >/dev/null; then printf '%s' "$1" | xclip -selection clipboard 2>/dev/null && return 0; fi
    if [[ -n ${DISPLAY-} ]] && command -v xsel >/dev/null; then printf '%s' "$1" | xsel -b -i 2>/dev/null && return 0; fi
    return 1
}
