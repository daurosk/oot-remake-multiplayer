# Shared by the Linux kit's scripts (play.sh, make-puppet-pack.sh, update.sh, the tests).
# Source it: . "$KIT/scripts/kit-lib.sh". The same rules as the Windows kit's coop-ini.ps1 and
# kit-paths.ps1, and as the emulator's own (lichii/hook/coop_settings.cpp).

KIT_GAME_ID=0400486024DEC000
KIT_MAIN_BUILD=E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348

# --- coop.ini ----------------------------------------------------------------------------

# A server address: host or IPv4 (letters, digits, dots, dashes), optionally :port 1-65535.
coop_test_server() {
    local v=$1
    [[ $v =~ ^([A-Za-z0-9._-]{1,253})(:([0-9]{1,5}))?$ ]] || return 1
    if [[ -n ${BASH_REMATCH[3]} ]]; then
        local p=$((10#${BASH_REMATCH[3]}))
        ((p >= 1 && p <= 65535)) || return 1
    fi
    return 0
}

# A player name: 1 to 16 characters (UTF-8 code points), no control characters.
coop_test_name() {
    local v=$1
    [[ -n $v ]] || return 1
    local LC_ALL=C
    case $v in *[$'\x01'-$'\x1f'$'\x7f']*) return 1 ;; esac
    # Code points: bytes that do not continue a UTF-8 sequence (0x80-0xbf).
    local points
    points=$(printf '%s' "$v" | LC_ALL=C tr -d '\200-\277' | wc -c)
    ((points >= 1 && points <= 16))
}

# Sets COOP_SERVER, COOP_PORT and COOP_NAME from an existing coop.ini (missing: empty).
coop_read_ini() {
    COOP_SERVER='' COOP_PORT='' COOP_NAME=''
    [[ -f $1 ]] || return 0
    local line key value
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        [[ $line =~ ^[[:space:]]*(server|port|name)[[:space:]]*=[[:space:]]*(.*[^[:space:]])?[[:space:]]*$ ]] || continue
        key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
        case $key in
            server) COOP_SERVER=$value ;;
            port) COOP_PORT=$value ;;
            name) COOP_NAME=$value ;;
        esac
    done <"$1"
}

# coop_write_ini TEMPLATE OUT SERVER PORT NAME: an empty value leaves that line commented out,
# so co-op is off without an error in the log.
coop_write_ini() {
    local template=$1 out=$2 server=$3 port=$4 name=$5 line marker value
    mkdir -p "$(dirname "$out")"
    {
        while IFS= read -r line || [[ -n $line ]]; do
            line=${line%$'\r'}
            for marker in SERVER PORT NAME; do
                case $marker in
                    SERVER) value=$server ;;
                    PORT) value=$port ;;
                    NAME) value=$name ;;
                esac
                if [[ $line == *"@$marker@"* ]]; then
                    if [[ -n $value ]]; then
                        line=${line//"@$marker@"/"$value"}
                    else
                        line=${line//"@$marker@"/}
                        line="# ${line%"${line##*[![:space:]]}"}"
                    fi
                fi
            done
            printf '%s\n' "$line"
        done <"$template"
    } >"$out.tmp" && mv -f "$out.tmp" "$out"
}

# --- coop-options.ini --------------------------------------------------------------------

# Prints the NAME=value lines for LICHII_COOP_* and EDEN_NX2_* switches (blank lines, # and ;
# comments and auto_update are skipped; anything else is named on stderr).
kit_read_options() {
    [[ -f $1 ]] || return 0
    local line t
    while IFS= read -r line || [[ -n $line ]]; do
        t=${line%$'\r'}
        t=${t#"${t%%[![:space:]]*}"}
        t=${t%"${t##*[![:space:]]}"}
        [[ -z $t || $t == '#'* || $t == ';'* ]] && continue
        [[ $t =~ ^auto_update[[:space:]]*= ]] && continue
        if [[ $t =~ ^((LICHII_COOP|EDEN_NX2)_[A-Z0-9_]+)[[:space:]]*=[[:space:]]*(.*)$ ]]; then
            printf '%s=%s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[3]}"
        else
            printf "coop-options.ini: skipped '%s' (only LICHII_COOP_NAME=value or EDEN_NX2_NAME=value lines)\n" "$t" >&2
        fi
    done <"$1"
}

# coop-options.ini's auto_update: ask (the default, also for a value it does not know), yes, no.
kit_auto_update() {
    local line when=ask
    [[ -f $1 ]] || { echo ask; return; }
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line =~ ^[[:space:]]*auto_update[[:space:]]*=[[:space:]]*(ask|yes|no)[[:space:]]*$ ]]; then
            when=${BASH_REMATCH[1]}
        fi
    done <"$1"
    echo "$when"
}

# --- The emulator's folders ----------------------------------------------------------------

# A value of the emulator's qt-config.ini ([Data Storage] paths), or nothing.
kit_qt_value() {
    local ini=$1/config/qt-config.ini line
    [[ -f $ini ]] || return 0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line == "$2="* ]]; then
            printf '%s\n' "${line#*=}"
            return 0
        fi
    done <"$ini"
}

# The mods folder the emulator reads: qt-config.ini's load_directory, else user/load.
kit_load_dir() {
    local v
    v=$(kit_qt_value "$1" load_directory)
    if [[ -n $v ]]; then printf '%s\n' "${v%/}"; else printf '%s\n' "$1/load"; fi
}
kit_sdmc_dir() {
    local v
    v=$(kit_qt_value "$1" sdmc_directory)
    if [[ -n $v ]]; then printf '%s\n' "${v%/}"; else printf '%s\n' "$1/sdmc"; fi
}

# A copied kit: qt-config.ini keeps the first copy's absolute folders; points every
# *_directory=<somewhere>/emulator/user[/sub] at this kit's user folder. A folder the player
# chose elsewhere is left alone. Prints the keys it changed.
kit_repair_paths() {
    local user=$1 ini=$1/config/qt-config.ini line changed=() out
    [[ -f $ini ]] || return 0
    out=$(mktemp)
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line =~ ^([A-Za-z_]+_directory)=(.+)/emulator/user(/[^/]*)?/?$ ]]; then
            local key=${BASH_REMATCH[1]} sub=${BASH_REMATCH[3]} want
            want="$user$sub"
            [[ ${BASH_REMATCH[2]}/emulator/user$sub == "$user$sub" ]] || {
                line="$key=$want/"
                changed+=("$key")
            }
        fi
        printf '%s\n' "$line"
    done <"$ini" >"$out"
    if ((${#changed[@]})); then
        mv -f "$out" "$ini"
        printf '%s\n' "${changed[@]}"
    else
        rm -f "$out"
    fi
}

# --- Mods (issue 136): code and cheat mods are not supported, texture/model mods are ---------

# The add-on names turned off for the game in the emulator's Add-Ons ([DisabledAddOns]).
kit_disabled_addons() {
    local ini=$1/config/qt-config.ini line in=0 game
    [[ -f $ini ]] || return 0
    game=$(printf '%d' "0x$KIT_GAME_ID")
    declare -A title=() names=()
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line =~ ^\[(.+)\][[:space:]]*$ ]]; then
            [[ ${BASH_REMATCH[1]} == DisabledAddOns ]] && in=1 || in=0
            continue
        fi
        ((in)) || continue
        if [[ $line =~ ^([0-9]+)\\title_id=([0-9]+)[[:space:]]*$ ]]; then
            title[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
        elif [[ $line =~ ^([0-9]+)\\disabled\\[0-9]+\\d=(.+)$ ]]; then
            names[${BASH_REMATCH[1]}]+="${BASH_REMATCH[2]%"${BASH_REMATCH[2]##*[![:space:]]}"}"$'\n'
        fi
    done <"$ini"
    local i
    for i in "${!title[@]}"; do
        [[ ${title[$i]} == "$game" ]] && printf '%s' "${names[$i]-}"
    done
}

# True when DIR has a non-empty subfolder named one of the given parts (any case).
kit_mod_part() {
    local dir=$1 part sub
    shift
    for part in "$@"; do
        for sub in "$dir"/*/; do
            sub=${sub%/}
            [[ -d $sub ]] || continue
            local n=${sub##*/}
            [[ ${n,,} == "$part" ]] || continue
            [[ -n $(ls -A "$sub" 2>/dev/null) ]] && return 0
        done
    done
    return 1
}

# The byte ranges an IPS or IPS32 file writes, "begin end" per line (file offsets); nothing
# when it is not one or has no end mark (the emulator then applies none of it either).
kit_ips_ranges() {
    od -An -v -tu1 "$1" 2>/dev/null | awk '
        { for (i = 1; i <= NF; i++) b[n++] = $i }
        END {
            if (n < 5) exit
            m = sprintf("%c%c%c%c%c", b[0], b[1], b[2], b[3], b[4])
            if (m == "PATCH") w = 3; else if (m == "IPS32") w = 4; else exit
            i = 5; out = ""
            while (i + w <= n) {
                if (w == 3 && b[i] == 69 && b[i+1] == 79 && b[i+2] == 70) { printf "%s", out; exit }
                if (w == 4 && b[i] == 69 && b[i+1] == 69 && b[i+2] == 79 && b[i+3] == 70) { printf "%s", out; exit }
                off = 0; for (k = 0; k < w; k++) off = off * 256 + b[i+k]
                i += w
                if (i + 2 > n) exit
                size = b[i] * 256 + b[i+1]; i += 2
                if (size == 0) { if (i + 3 > n) exit; size = b[i] * 256 + b[i+1]; i += 3 } else i += size
                if (size > 0) out = out sprintf("%d %d\n", off, off + size)
            }
        }'
}

# An IPSwitch (.pchtxt) file: first line "build <id>", then "begin end" per enabled write.
kit_pchtxt_ranges() {
    awk '
        BEGIN { on = -1; shift = 0; build = "" }
        function hexval(s,   i, c, v) { v = 0; s = toupper(s); sub(/^0X/, "", s)
            for (i = 1; i <= length(s); i++) { c = index("0123456789ABCDEF", substr(s, i, 1)); if (!c) break; v = v * 16 + c - 1 } return v }
        { sub(/\r$/, ""); line = $0; gsub(/^[ \t]+|[ \t]+$/, "", line) }
        line == "" || line ~ /^\/\// || line ~ /^#/ { next }
        line ~ /^@/ && length(line) > 2 {
            c = toupper(substr(line, 2, 1))
            if (c == "N") build = substr(line, 9)
            else if (c == "S") exit
            else if (c == "E") on = 1
            else if (c == "D") on = 0
            else if (line ~ /^@flag offset_shift/) { split(line, a, /[ \t]+/); shift = (a[3] ~ /^0[xX]/) ? hexval(a[3]) : a[3] + 0 }
            next
        }
        on != 1 { next }
        match(line, /^[0-9A-Fa-f]+/) {
            off = hexval(substr(line, 1, RLENGTH)) + shift
            q = index(line, "\""); if (!q) q = index(line, "\047")
            if (q) { rest = substr(line, q + 1); e = index(rest, substr(line, q, 1)); len = e ? e - 1 : length(rest) }
            else { n = split(line, parts, /[ \t\/]+/); len = int(length(parts[n]) / 2) }
            if (len > 0) ranges = ranges sprintf("%d %d\n", off, off + len)
        }
        END { printf "build %s\n%s", build, ranges }' "$1"
}

# True when a mod folder is Dark's Dynamic FPS, which Fritchii skips for its built-in (unless the
# built-in is off in Add-Ons): an exefs patch for this build writing into Dark's two ranges
# (main+0x6e6cc4 4 bytes, main+0x3c46200 120 bytes; file offsets +0x100).
kit_dynamic_fps_mod() {
    local dir=$1 off=$2 exefs f name ranges want
    grep -qxF 'Dynamic FPS by Dark (built-in)' <<<"$off" && return 1
    exefs=
    for f in "$dir"/*/; do
        f=${f%/}
        name=${f##*/}
        [[ ${name,,} == exefs ]] && exefs=$f
    done
    [[ -n $exefs ]] || return 1
    want=$(printf '%-64s' "$KIT_MAIN_BUILD" | tr ' ' 0)
    for f in "$exefs"/*; do
        [[ -f $f ]] || continue
        name=${f##*/}
        case ${name,,} in
            *.ips)
                [[ $(printf '%-64s' "${name%%.*}" | tr ' ' 0) == "$want" ]] || continue  # as written, like the emulator
                ranges=$(kit_ips_ranges "$f")
                ;;
            *.pchtxt)
                ranges=$(kit_pchtxt_ranges "$f")
                local b
                b=$(head -n1 <<<"$ranges")
                b=${b#build }
                [[ $(printf '%-64s' "${b^^}" | tr ' ' 0) == "$want" ]] || continue
                ranges=$(tail -n +2 <<<"$ranges")
                ;;
            *) continue ;;
        esac
        if awk 'BEGIN { r[0] = 7237060; e[0] = 7237064; r[1] = 63202048; e[1] = 63202168 }
                NF == 2 { for (k = 0; k < 2; k++) if ($1 < e[k] && r[k] < $2) hit = 1 }
                END { exit !hit }' <<<"$ranges"; then
            return 0
        fi
    done
    return 1
}

# Prints one line per code or cheat mod the emulator would load for the game: each folder in
# <load>/0400486024DEC000 with a non-empty exefs or cheats folder (romfs alone, textures and
# models, is fine), loose cheat_* files there, and the SD card's atmosphere/contents/<game>/exefs.
# Mods turned off in Add-Ons, co-op's own coop-puppet and Dark's Dynamic FPS do not count.
kit_mods() {
    local user=$1 off load sd mod f
    off=$(kit_disabled_addons "$user")
    load=$(kit_load_dir "$user")/$KIT_GAME_ID
    if [[ -d $load ]]; then
        for mod in "$load"/*/; do
            mod=${mod%/}
            [[ -d $mod ]] || continue
            [[ ${mod##*/} == coop-puppet ]] && continue
            grep -qxF -- "${mod##*/}" <<<"$off" && continue
            kit_dynamic_fps_mod "$mod" "$off" && continue
            kit_mod_part "$mod" exefs cheats && printf '%s\n' "$mod"
        done
        for f in "$load"/cheat_*; do
            [[ -f $f ]] || continue
            grep -qxF -- "${f##*/}" <<<"$off" || printf '%s\n' "$f"
        done
    fi
    sd=$(kit_sdmc_dir "$user")/atmosphere/contents/$KIT_GAME_ID
    if [[ -d $sd ]] && ! grep -qxF SDMC <<<"$off" && kit_mod_part "$sd" exefs; then
        printf '%s\n' "$sd"
    fi
}

# --- Versions (update.sh) ----------------------------------------------------------------

# kit_version_compare A B: prints -1, 0 or 1. v0.1.0 > v0.1.0-beta2; beta2 < beta10.
# Something that is not a version counts as the oldest.
kit_version_compare() {
    local a=$1 b=$2 re='^v?([0-9]+)\.([0-9]+)\.([0-9]+)(-([A-Za-z0-9.]+))?$'
    local ok_a=0 ok_b=0 ca=() cb=() pa='' pb=''
    if [[ $a =~ $re ]]; then ok_a=1; ca=("${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"); pa=${BASH_REMATCH[5]}; fi
    if [[ $b =~ $re ]]; then ok_b=1; cb=("${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"); pb=${BASH_REMATCH[5]}; fi
    if ((!ok_a || !ok_b)); then echo $((ok_a - ok_b)); return; fi
    local i
    for i in 0 1 2; do
        if ((10#${ca[$i]} != 10#${cb[$i]})); then
            ((10#${ca[$i]} < 10#${cb[$i]})) && echo -1 || echo 1
            return
        fi
    done
    [[ -z $pa && -z $pb ]] && { echo 0; return; }
    [[ -z $pa ]] && { echo 1; return; }
    [[ -z $pb ]] && { echo -1; return; }
    # Runs of digits compare as numbers, the rest as text: beta2 < beta10 < betab.
    awk -v a="$pa" -v b="$pb" '
        function run(s, i,   c) { c = substr(s, i, 1); return c ~ /[0-9]/ }
        BEGIN {
            i = 1; j = 1
            while (i <= length(a) && j <= length(b)) {
                if (run(a, i) && run(b, j)) {
                    x = ""; while (i <= length(a) && run(a, i)) x = x substr(a, i++, 1)
                    y = ""; while (j <= length(b) && run(b, j)) y = y substr(b, j++, 1)
                    if (x + 0 != y + 0) { print (x + 0 < y + 0) ? -1 : 1; exit }
                } else {
                    x = substr(a, i++, 1); y = substr(b, j++, 1)
                    if (x != y) { print (x < y) ? -1 : 1; exit }
                }
            }
            if (i <= length(a)) print 1; else if (j <= length(b)) print -1; else print 0
        }'
}

# The release this kit was made from: the first line of VERSION.txt
# ("kit: oot-coop-v0.1.0-beta7-linux (release v0.1.0-beta7)"); empty when none.
kit_version() {
    local first
    [[ -f $1/VERSION.txt ]] || return 0
    IFS= read -r first <"$1/VERSION.txt" || true
    [[ $first =~ \(release\ ([^\)]+)\) ]] && printf '%s\n' "${BASH_REMATCH[1]}"
}
