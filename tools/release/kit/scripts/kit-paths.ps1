# Keeps a kit copy pointed at its own folders. Dot-source it.
#
# The emulator saves its NAND, SD, load (mods), dump and TAS folders in
# user\config\qt-config.ini as absolute paths. A kit folder copied after its first run
# (local-A copied to local-B, or a kit unzipped over an old one) keeps reading the
# first copy's saves and mods. Repair-KitPaths points every such line that names a
# kit's emulator\user folder at this copy's own; a folder the player chose elsewhere
# is left as it is.

function Repair-KitPaths([string]$User) {
    $ini = Join-Path $User 'config\qt-config.ini'
    if (-not (Test-Path -LiteralPath $ini)) { return @() }
    $ours = ([System.IO.Path]::GetFullPath($User)).Replace('\', '/').TrimEnd('/')
    $fixed = @()
    $lines = [System.IO.File]::ReadAllLines($ini)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch '^(\w+_directory)=(.+?)/emulator/user(/[^/]*)?/?$') { continue }
        $key = $Matches[1]; $sub = $Matches[3]
        $want = "$ours$sub"
        $now = $lines[$i].Substring($key.Length + 1)
        if ($now -ne $want) {
            $lines[$i] = "$key=$want"
            $fixed += $key
        }
    }
    if ($fixed.Count) {
        # The emulator writes this file without a byte order mark; keep it so.
        [System.IO.File]::WriteAllText($ini, (($lines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
    }
    return $fixed
}

# The mods folder the emulator reads: qt-config.ini's load_directory, else user\load.
function Get-KitLoadDir([string]$User) {
    $ini = Join-Path $User 'config\qt-config.ini'
    if (Test-Path -LiteralPath $ini) {
        foreach ($line in [System.IO.File]::ReadAllLines($ini)) {
            if ($line -match '^load_directory=(.+)$') { return [System.IO.Path]::GetFullPath($Matches[1]) }
        }
    }
    return Join-Path $User 'load'
}

# Code and cheat mods the emulator would load for Ocarina of Time, which co-op does not allow
# (issue 136: a mod's code patches can sit where co-op's own go, and cheats change the game under
# the sync). Mods that only replace game files (romfs: textures, models) are allowed (Alex,
# 2026-11-10: texture fixes). Looked at the way Fritchii's Add-Ons list does: each folder in
# <load>\0400486024DEC000 with a non-empty exefs or cheats folder, loose cheat_* files there,
# and the SD card's atmosphere\contents\0400486024DEC000\exefs (called "SDMC" there). A mod turned off in the game's
# Add-Ons (qt-config.ini [DisabledAddOns]) does not count, and neither does co-op's own puppet
# pack (coop-puppet). Returns one line per mod: its folder.
$KitGameId = '0400486024DEC000'
function Get-KitSdmcDir([string]$User) {
    $ini = Join-Path $User 'config\qt-config.ini'
    if (Test-Path -LiteralPath $ini) {
        foreach ($line in [System.IO.File]::ReadAllLines($ini)) {
            if ($line -match '^sdmc_directory=(.+)$') { return [System.IO.Path]::GetFullPath($Matches[1]) }
        }
    }
    return Join-Path $User 'sdmc'
}
function Get-KitDisabledAddons([string]$User) {
    $ini = Join-Path $User 'config\qt-config.ini'
    $names = @()
    if (-not (Test-Path -LiteralPath $ini)) { return $names }
    $game = [Convert]::ToUInt64($KitGameId, 16).ToString()
    $inSection = $false
    $titles = @{}
    $disabled = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($ini)) {
        if ($line -match '^\[(.+)\]\s*$') { $inSection = $Matches[1] -eq 'DisabledAddOns'; continue }
        if (-not $inSection) { continue }
        if ($line -match '^(\d+)\\title_id=(\d+)\s*$') { $titles[$Matches[1]] = $Matches[2] }
        elseif ($line -match '^(\d+)\\disabled\\\d+\\d=(.+?)\s*$') {
            if (-not $disabled.ContainsKey($Matches[1])) { $disabled[$Matches[1]] = @() }
            $disabled[$Matches[1]] += $Matches[2]
        }
    }
    # (Fritchii also takes the disabled list of an update's parent title; Ocarina has none.)
    foreach ($i in $titles.Keys) {
        if ($titles[$i] -eq $game -and $disabled.ContainsKey($i)) { $names += $disabled[$i] }
    }
    return $names
}
function Test-KitModPart([string]$Dir, [string[]]$Parts) {
    foreach ($part in $Parts) {
        $sub = Get-ChildItem -LiteralPath $Dir -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ieq $part } | Select-Object -First 1
        if ($sub -and (Get-ChildItem -LiteralPath $sub.FullName -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) { return $true }
    }
    return $false
}
# Dark's Dynamic FPS mod, which Fritchii v5.7 has built in: a mod folder with an exefs patch
# for this build that writes into Dark's two ranges (main+0x6e6cc4, 4 bytes, and
# main+0x3c46200, 120 bytes; patch offsets count the 0x100-byte NSO header) is neither loaded
# nor listed in Add-Ons, unless the built-in is turned off there (patch_manager.cpp,
# IsBuiltinDynamicFpsMod). Co-op's hook allows it too, so such a folder is no mod here.
$KitMainBuild = 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348'
$KitDynamicFpsName = 'Dynamic FPS by Dark (built-in)'
$KitDynamicFpsRanges = @(@(0x6e6dc4, 0x6e6dc8), @(0x3c46300, 0x3c46378))  # file offsets (+0x100)
# The byte ranges an IPS or IPS32 file writes, as @(begin, end) file offsets; none when it is
# not one or has no EOF mark (Fritchii then applies none of it either).
function Get-KitIpsRanges([string]$Path) {
    $b = [System.IO.File]::ReadAllBytes($Path)
    $magic = [System.Text.Encoding]::ASCII.GetString($b, 0, [Math]::Min(5, $b.Length))
    if ($magic -ne 'PATCH' -and $magic -ne 'IPS32') { return @() }
    $w = if ($magic -eq 'IPS32') { 4 } else { 3 }
    $ranges = @()
    $i = 5
    while ($i + $w -le $b.Length) {
        $tag = [System.Text.Encoding]::ASCII.GetString($b, $i, $w)
        if (($w -eq 3 -and $tag -eq 'EOF') -or ($w -eq 4 -and $tag -eq 'EEOF')) { return , $ranges }
        $off = 0L
        for ($k = 0; $k -lt $w; $k++) { $off = $off * 256 + $b[$i + $k] }
        $i += $w
        if ($i + 2 -gt $b.Length) { return @() }
        $size = $b[$i] * 256 + $b[$i + 1]
        $i += 2
        if ($size -eq 0) {
            if ($i + 3 -gt $b.Length) { return @() }
            $size = $b[$i] * 256 + $b[$i + 1]
            $i += 3
        } else { $i += $size }
        if ($size -gt 0) { $ranges += , @($off, ($off + $size)) }
    }
    return @()
}
# An IPSwitch (.pchtxt) file: its build id and the ranges its enabled patches write.
function Get-KitPchtxt([string]$Path) {
    $build = ''
    $shift = 0L
    $ranges = @()
    $on = $null
    foreach ($raw in [System.IO.File]::ReadAllLines($Path)) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith('//') -or $line.StartsWith('#')) { continue }
        if ($line.Length -gt 2 -and $line[0] -eq '@') {
            switch -regex ($line) {
                '^@[nN]' { $build = $line.Substring([Math]::Min(8, $line.Length)).Trim() }
                '^@[sS]' { return @{ Build = $build; Ranges = $ranges } }
                '^@[eE]' { $on = $true }
                '^@[dD]' { $on = $false }
                '^@flag offset_shift\s+(\S+)' { $shift = [Convert]::ToInt64($Matches[1], $(if ($Matches[1] -match '^0[xX]') { 16 } else { 10 })) }
            }
            continue
        }
        if ($null -eq $on -or -not $on) { continue }
        if ($line -notmatch '^([0-9A-Fa-f]+)') { continue }
        $off = [Convert]::ToInt64($Matches[1], 16) + $shift
        $q = $line.IndexOfAny([char[]]@('"', "'"))
        if ($q -ge 0) {
            $end = $line.IndexOf($line[$q], $q + 1)
            $len = if ($end -gt $q) { $end - $q - 1 } else { $line.Length - $q - 1 }
        } else {
            $hex = ($line -split '[\s/]+')[-1]
            $len = [int][Math]::Floor($hex.Length / 2)
        }
        if ($len -gt 0) { $ranges += , @($off, ($off + $len)) }
    }
    return @{ Build = $build; Ranges = $ranges }
}
function Test-KitDynamicFpsMod([string]$Dir, [string[]]$Off) {
    if ($Off -contains $KitDynamicFpsName) { return $false }  # the built-in is off: the mod loads
    $exefs = Get-ChildItem -LiteralPath $Dir -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ieq 'exefs' } | Select-Object -First 1
    if (-not $exefs) { return $false }
    $want = $KitMainBuild.PadRight(64, '0')
    foreach ($f in Get-ChildItem -LiteralPath $exefs.FullName -File -ErrorAction SilentlyContinue) {
        $ranges = @()
        if ($f.Extension -eq '.ips') {
            $name = $f.Name.Substring(0, $f.Name.IndexOf('.'))
            if ($name.PadRight(64, '0') -cne $want) { continue }  # the emulator compares as written
            $ranges = Get-KitIpsRanges $f.FullName
        } elseif ($f.Extension -eq '.pchtxt') {
            $p = Get-KitPchtxt $f.FullName
            if ($p.Build.PadRight(64, '0') -ne $want) { continue }
            $ranges = $p.Ranges
        } else { continue }
        foreach ($r in $ranges) {
            foreach ($d in $KitDynamicFpsRanges) {
                if ($r[0] -lt $d[1] -and $d[0] -lt $r[1]) { return $true }
            }
        }
    }
    return $false
}

function Get-KitMods([string]$User) {
    $found = @()
    $off = @(Get-KitDisabledAddons $User)
    $load = Join-Path (Get-KitLoadDir $User) $KitGameId
    if (Test-Path -LiteralPath $load) {
        foreach ($mod in Get-ChildItem -LiteralPath $load -Directory -ErrorAction SilentlyContinue) {
            if ($mod.Name -eq 'coop-puppet' -or $off -contains $mod.Name) { continue }
            if (Test-KitDynamicFpsMod $mod.FullName $off) { continue }  # Fritchii uses its built-in instead
            if (Test-KitModPart $mod.FullName @('exefs', 'cheats')) { $found += $mod.FullName }  # romfs alone is fine
        }
        # Loose cheat files (cheat_*), which the emulator lists as "Cheats" by file name.
        foreach ($f in Get-ChildItem -LiteralPath $load -File -Filter 'cheat_*' -ErrorAction SilentlyContinue) {
            if ($off -notcontains $f.Name) { $found += $f.FullName }
        }
    }
    $sd = Join-Path (Get-KitSdmcDir $User) "atmosphere\contents\$KitGameId"
    if ((Test-Path -LiteralPath $sd) -and $off -notcontains 'SDMC' -and
        (Test-KitModPart $sd @('exefs'))) { $found += $sd }
    return $found
}

# Reads coop-options.ini: NAME=value lines for LICHII_COOP_* switches and the emulator's
# own EDEN_NX2_* switches. Blank lines and lines starting with # or ; are skipped, and so
# are auto_update and verify_signature (the updater reads them, the emulator does not). 'color = <colour>'
# is passed as LICHII_COOP_COLOR when Test-CoopColor (coop-ini.ps1) takes it. Returns an ordered table.
function Read-KitOptions([string]$Path) {
    $values = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Path)) { return $values }
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#') -or $t.StartsWith(';') -or $t -match '^(auto_update|verify_signature|server_allow|server_bind|server_port|server_password|max_players|public_address_lookup|upnp|invite|stun_servers|relay|relay_key|relay_room|approve_joins|pvp)\s*=') { continue }
        if ($t -match '^color\s*=\s*(.*?)\s*$') {
            $c = $Matches[1] -replace '\s+[#;].*$', ''  # a comment after it: 'color = blue  # mine'
            if (-not $c) { continue }  # none chosen: the player number's colour
            if ((Get-Command Test-CoopColor -ErrorAction SilentlyContinue) -and -not (Test-CoopColor $c)) {
                Write-Host "coop-options.ini: color '$c' is not a colour (red, blue, ... or #RRGGBB); the host's blue or the joiners' red is used"
                continue
            }
            # The same text for every reader (the self tunic, the emulator, the module): quotes, 0x, #RGB taken.
            if (Get-Command ConvertTo-CoopColor -ErrorAction SilentlyContinue) { $c = ConvertTo-CoopColor $c }
            $values['LICHII_COOP_COLOR'] = $c
            continue
        }
        if ($t -match '^((LICHII_COOP|EDEN_NX2)_[A-Z0-9_]+)\s*=\s*(.*)$') { $values[$Matches[1]] = $Matches[3] }
        else { Write-Host "coop-options.ini: skipped '$t' (only LICHII_COOP_NAME=value or EDEN_NX2_NAME=value lines)" }
    }
    return $values
}

# The running server's session, for a host who closed the game and starts Host.bat again
# (play.ps1 rejoins instead of making a new password). start-server.ps1 writes
# logs\host-session.txt just before the server starts and removes it when the server ends:
# when it was written, the port and the join password. The file holds the password, so it is
# not a .log (collect-logs.ps1 takes only those).
function Get-KitHostSessionFile([string]$Kit) { Join-Path $Kit 'logs\host-session.txt' }
function Write-KitHostSession([string]$Kit, [int]$Port, [string]$Password) {
    $text = "started = $([DateTime]::UtcNow.Ticks)`r`nport = $Port`r`npassword = $Password`r`n"
    $file = Get-KitHostSessionFile $Kit
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
    [System.IO.File]::WriteAllText($file, $text, [System.Text.UTF8Encoding]::new($false))
    return $text
}
# The saved session when it can belong to one of $Servers (this kit's running coop_server.exe
# processes: objects with StartTime and, when known, CommandLine), else $null: the server
# started after the file was written and, when its command line is known, it is a server on the
# saved port (not a join helper, not a server started by hand on another port).
function Read-KitHostSession([string]$Kit, $Servers) {
    $file = Get-KitHostSessionFile $Kit
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    $v = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($file)) {
        if ($line -match '^\s*(started|port|password)\s*=\s*(.*?)\s*$') { $v[$Matches[1]] = $Matches[2] }
    }
    if ("$($v.started)" -notmatch '^\d{1,19}$' -or "$($v.port)" -notmatch '^\d{1,5}$' -or -not $v.password) { return $null }
    $port = [int]$v.port
    if ($port -lt 1 -or $port -gt 65535 -or $v.password -match '\s' -or $v.password.Length -gt 128) { return $null }
    $written = [DateTime]::new([long]$v.started, [DateTimeKind]::Utc).AddSeconds(-2)
    foreach ($s in @($Servers)) {
        if (-not $s -or -not $s.StartTime) { continue }
        if (([DateTime]$s.StartTime).ToUniversalTime() -lt $written) { continue }
        $cmd = "$($s.CommandLine)"
        if ($cmd -and ($cmd -match '--join\b' -or $cmd -notmatch "--port`"?\s+`"?$port\b")) { continue }
        return [pscustomobject]@{ Port = $port; Password = $v.password }
    }
    return $null
}

# coop-options.ini's server_allow and server_bind (the host's server only; start-server.ps1
# checks them): an ordered table with Allow and Bind, '' when not set. Password is the join
# password (server_password): $null when no line without a '#' sets it, or the line is empty
# (every session has a password: Host.bat then makes a new random one). Upnp (on unless upnp = no), Invite (on unless invite =
# no), Stun (stun_servers), Relay (relay = address[:port]), RelayKey (relay_key) and RelayRoom (relay_room, a
# room name on the relay) are the router forward, the invite code and the relay (docs/play-online.md). ApproveJoins:
# $true for 'approve_joins = on': each new player waits for the host's /allow or /deny (issue 261). Pvp: $true
# only for 'pvp = on' (or yes, 1, true); anything else, or no line, is off. Relay: the project's own
# ($script:KitDefaultRelay, as the game's CoopTitleMenu::kDefaultRelay) without a relay line, or with an empty one: it is
# never removed. SameNetworkOpen: players on this network join without the password, unless 'same_network_password =
# on' (docs/protocol.md, "Same network").
$script:KitDefaultRelay = '159.54.182.85:27530'
function Read-KitServerAccess([string]$Path) {
    $access = [ordered]@{ Allow = ''; Bind = ''; Password = $null; Upnp = $true; Invite = $true; Stun = ''; Relay = $script:KitDefaultRelay; RelayKey = ''; RelayRoom = ''; ApproveJoins = $false; Pvp = $false; SameNetworkOpen = $true }
    if (-not (Test-Path -LiteralPath $Path)) { return $access }
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match '^\s*server_allow\s*=\s*(.*?)\s*$') { $access.Allow = $Matches[1] }
        elseif ($line -match '^\s*server_bind\s*=\s*(.*?)\s*$') { $access.Bind = $Matches[1] }
        elseif ($line -match '^\s*server_password\s*=\s*(.*?)\s*$') { if ($Matches[1]) { $access.Password = $Matches[1] } }
        elseif ($line -match '^\s*upnp\s*=\s*(.*?)\s*$') { $access.Upnp = $Matches[1] -notmatch '^(no|off|false|0)$' }
        elseif ($line -match '^\s*invite\s*=\s*(.*?)\s*$') { $access.Invite = $Matches[1] -notmatch '^(no|off|false|0)$' }
        elseif ($line -match '^\s*stun_servers\s*=\s*(.*?)\s*$') { $access.Stun = $Matches[1] }
        elseif ($line -match '^\s*relay\s*=\s*(.*?)\s*$') { $access.Relay = $(if ($Matches[1]) { $Matches[1] } else { $script:KitDefaultRelay }) }
        elseif ($line -match '^\s*same_network_password\s*=\s*(.*?)\s*$') { $access.SameNetworkOpen = $Matches[1] -notmatch '^(on|yes|1|true)$' }
        elseif ($line -match '^\s*relay_key\s*=\s*(.*?)\s*$') { $access.RelayKey = $Matches[1] }
        elseif ($line -match '^\s*relay_room\s*=\s*(.*?)\s*$') { $access.RelayRoom = $Matches[1] }
        elseif ($line -match '^\s*approve_joins\s*=\s*(.*?)\s*$') { $access.ApproveJoins = $Matches[1] -match '^(on|yes|1|true)$' }
        elseif ($line -match '^\s*pvp\s*=\s*(.*?)\s*$') { $access.Pvp = $Matches[1] -match '^(on|yes|1|true)$' }
    }
    return $access
}

# Saves the join password in coop-options.ini as 'server_password = ...' ('' to go back to a
# new random one each session): over
# the first server_password line (with or without a '#'), else at the end of the file.
function Set-KitServerPassword([string]$Path, [string]$Password) {
    $lines = @()
    if (Test-Path -LiteralPath $Path) { $lines = @([System.IO.File]::ReadAllLines($Path)) }
    $new = "server_password = $Password".TrimEnd()
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*#?\s*server_password\s*=') { $lines[$i] = $new; $done = $true; break }
    }
    if (-not $done) { $lines += $new }
    [System.IO.File]::WriteAllText($Path, (($lines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
}

# Fritchii v5's graphics pipeline libraries ("shader libraries") crash AMD's Windows driver
# (amdvlk64.dll, issue 118); Fritchii v5.4 and later switch them off on AMD by themselves.
# Which video cards this PC has: 'amd' (an AMD card and no NVIDIA one), 'amd+nvidia', or ''.
# With an NVIDIA card the emulator uses it (it lists NVIDIA first and takes the first card),
# so AMD's driver is not used. $Query returns the cards (Win32_VideoController by default);
# a query that fails counts as no AMD card.
function Get-AmdGpuKind([scriptblock]$Query = { Get-CimInstance Win32_VideoController -OperationTimeoutSec 5 -ErrorAction Stop }) {
    try { $cards = @(& $Query) } catch { return '' }
    $amd = $false; $nvidia = $false
    foreach ($card in $cards) {
        $text = "$($card.Name) | $($card.AdapterCompatibility)"
        if ($text -match '\bAMD\b|Radeon|Advanced Micro Devices') { $amd = $true }
        if ($text -match 'NVIDIA') { $nvidia = $true }
    }
    if (-not $amd) { return '' }
    if ($nvidia) { return 'amd+nvidia' }
    return 'amd'
}

# Sets EDEN_NX2_GPL=0 (shader libraries off) on a PC whose card is AMD, unless the player
# already set EDEN_NX2_GPL (in Windows or in coop-options.ini, which play.ps1 reads first).
# Returns the line to show, or nothing.
function Set-AmdGplDefault([scriptblock]$Query = { Get-CimInstance Win32_VideoController -OperationTimeoutSec 5 -ErrorAction Stop }) {
    if ($env:EDEN_NX2_GPL) { return }
    switch (Get-AmdGpuKind $Query) {
        'amd' {
            $env:EDEN_NX2_GPL = '0'
            return 'AMD GPU found: shader libraries off (EDEN_NX2_GPL=0)'
        }
        'amd+nvidia' {
            return 'AMD and NVIDIA GPUs found: the game uses NVIDIA, shader libraries stay on. Crashes in amdvlk64.dll: add EDEN_NX2_GPL=0 to coop-options.ini.'
        }
    }
}

# Low frame rates (docs/setup.md, "Low frame rate"): a PC with only built-in graphics (Intel
# UHD/Iris/Arc Graphics, AMD Radeon ... Graphics in a Ryzen) and no graphics card often runs the
# game under 30 fps at 60 FPS. Returns a line telling the player about the 30 FPS button, or
# nothing (a graphics card found, no cards, or a query that fails). Virtual and remote-desktop
# adapters do not count.
function Get-GpuSpeedHint([scriptblock]$Query = { Get-CimInstance Win32_VideoController -OperationTimeoutSec 5 -ErrorAction Stop }) {
    try { $cards = @(& $Query) } catch { return }
    $builtIn = @()
    foreach ($card in $cards) {
        $name = "$($card.Name)"
        $text = "$name | $($card.AdapterCompatibility)"
        if ($text -match 'Microsoft|Remote|Virtual|Parsec|Citrix|VMware|Hyper-V|DisplayLink|spacedesk|Idd') { continue }
        if ($text -match 'NVIDIA') { return }
        if ($name -match 'Radeon.*\b(RX\s*\d{3,4}|RX\s*Vega\s*(56|64)\b|Pro|VII|R[79]\s)') { return }
        if ($name -match 'Arc.*\b(Pro|[AB]\d{3})\b') { return }
        if ($text -match 'Intel|\bAMD\b|Radeon|Advanced Micro Devices') { $builtIn += $name }
    }
    if (-not $builtIn) { return }
    return "Only built-in graphics found ($($builtIn -join ', ')). If the game runs slowly, click the 60 FPS button at the bottom of the emulator window to switch to 30 FPS (SETUP.md, `"Low frame rate`")."
}

# The pack tool's own hash, as tools\puppet_pack\build.py (tool_hash) writes it into
# coop_puppet.json: SHA-256 over "<name>:<sha256 of the file>`n" for each *.py in the folder
# (not tests\), names in ordinal order. Nothing when the folder is not there.
function Get-PuppetToolHash([string]$ToolDir) {
    if (-not $ToolDir -or -not (Test-Path -LiteralPath $ToolDir -PathType Container)) { return }
    $files = @(Get-ChildItem -LiteralPath $ToolDir -File -ErrorAction Stop | Where-Object { $_.Extension -eq '.py' })
    $names = [string[]]@($files | ForEach-Object { $_.Name })
    [Array]::Sort($names, [StringComparer]::Ordinal)
    $text = New-Object System.Text.StringBuilder
    foreach ($n in $names) {
        $h = (Get-FileHash -LiteralPath (Join-Path $ToolDir $n) -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$text.Append("${n}:$h`n")
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text.ToString()))
    } finally { $sha.Dispose() }
    return (-join ($bytes | ForEach-Object { $_.ToString('x2') }))
}

# The puppet pack (coop_puppet.json, written by make-puppet-pack.cmd) against the kit's pack
# tool (tools\puppet_pack): a line asking the player to make the pack again, or nothing. A pack
# made before issue 131 has no "sound": true, so the other players' Links make no sound; a pack
# whose tool_sha256 is not the kit's tool hash was made by another version of the tool (a hash,
# not file times: a kit zip's times are the builder's local time). Never throws: the start goes on.
# Your own Link in your colour (#202). The tunic preset nearest a colour (a name or #RRGGBB), as
# the emulator picks the other players' puppets (lichii/hook/coop_colors.cpp TunicOf; keep the
# colours the same): luma and chroma apart, chroma counted double. '' when it is not a colour.
$KitTunicColors = [ordered]@{ red = 0xFF3030; orange = 0xFF8020; yellow = 0xFFC020; green = 0x30D040; cyan = 0x20D0E0
    blue = 0x3070FF; purple = 0xA040FF; pink = 0xFF60C0; white = 0xFFFFFF; black = 0x202020 }
function Get-KitTunic([string]$Color) {
    if (-not $Color) { return '' }
    $c = $Color.Trim().Trim('"', "'").Trim().ToLowerInvariant()
    if ((Get-Command ConvertTo-CoopColor -ErrorAction SilentlyContinue) -and (ConvertTo-CoopColor $c)) {
        $c = (ConvertTo-CoopColor $c).ToLowerInvariant()   # (0x, #RGB, #RRGGBBAA, rgb(): coop-ini.ps1)
    }
    $named = @{ brown = 0x8B5A2B; gray = 0xA0A0A0; grey = 0xA0A0A0 }
    if ($KitTunicColors.Contains($c)) { $rgb = $KitTunicColors[$c] }
    elseif ($named.ContainsKey($c)) { $rgb = $named[$c] }
    elseif ($c -match '^#?([0-9a-f]{6})$') { $rgb = [Convert]::ToInt32($Matches[1], 16) }
    else { return '' }
    $ycc = {
        param($v)
        $r = ($v -shr 16) -band 0xFF; $g = ($v -shr 8) -band 0xFF; $b = $v -band 0xFF
        $y = 0.299 * $r + 0.587 * $g + 0.114 * $b
        , @($y, (0.564 * ($b - $y)), (0.713 * ($r - $y)))
    }
    $want = & $ycc $rgb
    $best = ''; $bestD = -1.0
    foreach ($name in $KitTunicColors.Keys) {
        $have = & $ycc $KitTunicColors[$name]
        $dy = $want[0] - $have[0]; $db = $want[1] - $have[1]; $dr = $want[2] - $have[2]
        $d = $dy * $dy + 2.0 * ($db * $db + $dr * $dr)
        if ($bestD -lt 0 -or $d -lt $bestD) { $bestD = $d; $best = $name }
    }
    return $best
}

# Puts your colour's tunic files in the pack's romfs, so your own Link wears your colour: the pack
# tool's coop_self/ files, listed in coop_puppet.json as self_files ({colour: {romfs path: file}}).
# Since colour-agree they are your own textures under their own names (Tex/Angin_Tile_S_Alb, ...)
# and a copy of each player model naming them over Model/Player_<Age>: your own Link never draws
# the puppets' copies. Older packs: the game-name tiles and the adult's puppet model copy (#202);
# a pack from before that: self_tiles / self_dark_tiles. The Linux kit and the autotest run
# tools/puppet_pack/self_tunic.py, the same steps. Green, no colour or a
# pack without them: takes every copy away, the game's own green. Only the Kokiri tunic: the
# Goron and Zora tunics have their own textures. Returns the colour put in, or ''.
# -Painted (Longshot): the co-op module paints your exact colour on green's files while the game
# runs (hookshot/src/coop_look_live.cpp, from the pack's coop_look/green.bin), so a pack that has
# green's own files and coop_look/green.bin gets green's whatever the colour: what the game's memory
# holds is then always the texels the module paints from. Without -Painted (the emulator, which has
# no such module) or with an older pack: your colour's files, the nearest preset.
function Set-KitSelfTunic([string]$PackDir, [string]$Color, [switch]$Painted) {
    $romfs = Join-Path $PackDir 'romfs'
    $tile = 'Tex/Angin_Tile_A_Alb.bntx.zs'
    $dark = 'Tex/Angin_Tile_A_Dark_Alb.bntx.zs'
    # No colour: green's own files (npc-colour). Your own Link never wears the game's tunic textures: Mido's and
    # Saria's models (every Kokiri) share them, and the module repaints your own Link's by their texels.
    $tunic = Get-KitTunic $Color
    if (-not $tunic) { $tunic = 'green' }
    $json = $null
    try {
        $json = [System.IO.File]::ReadAllText((Join-Path $PackDir 'coop_puppet.json')) | ConvertFrom-Json -ErrorAction Stop
    } catch { }
    try {
        if ($Painted -and $json -and $json.self_files -and $json.self_files.green -and $json.look_files -and
            $json.look_files.green -and (Test-Path -LiteralPath (Join-Path $PackDir $json.look_files.green) -PathType Leaf)) {
            $tunic = 'green'
        }
    } catch { }
    $ok = { param($rel) $rel -is [string] -and ($rel -match '^Tex/[A-Za-z0-9_]+\.bntx\.zs$' -or
            $rel -match '^Model/Player_(Adult|Child)\.Player_(Adult|Child)\.bfres\.zs$') }
    # Every file a colour may put in, taken away first: no copy stays after a change of colour.
    # The fixed list also covers a pack without self_files (made again older): no stale copy.
    $targets = @($tile, $dark, 'Tex/Mt_Vest_Player_Adult_Alb.bntx.zs', 'Tex/Mt_Angin_Acs_Player_Adult_Alb.bntx.zs',
        'Tex/Mt_Cloth_Parts_Player_Child_Alb.bntx.zs', 'Tex/Mt_Vest_Player_Child_Alb.bntx.zs',
        'Model/Player_Adult.Player_Adult.bfres.zs', 'Model/Player_Child.Player_Child.bfres.zs')
    $put = [ordered]@{}
    try {
        if ($json -and $json.self_files) {
            foreach ($c in $json.self_files.PSObject.Properties) {
                foreach ($f in $c.Value.PSObject.Properties) { if (& $ok $f.Name) { $targets += $f.Name } }
            }
        }
        if ($json -and $json.tunics -and $json.tunics.green) {
            if ($json.self_files -and $json.self_files.$tunic) {
                foreach ($f in $json.self_files.$tunic.PSObject.Properties) {
                    if ((& $ok $f.Name) -and $f.Value) { $put[$f.Name] = Join-Path $PackDir $f.Value }
                }
            } else {
                $rel = if ($json.self_tiles) { $json.self_tiles.$tunic } else { $null }
                $relDark = if ($json.self_dark_tiles) { $json.self_dark_tiles.$tunic } else { $null }
                if ($rel) { $put[$tile] = Join-Path $PackDir $rel }
                if ($rel -and $relDark) { $put[$dark] = Join-Path $PackDir $relDark }
            }
        }
    } catch { $put = [ordered]@{} }
    # A main tile must be there (your own S tile since colour-agree, or an older pack's game-name tile); a
    # missing other file is left to the game's.
    $main = @($put.Keys | Where-Object { $_ -match '^Tex/Angin_Tile_[A-Z]_Alb\.bntx\.zs$' -and (Test-Path -LiteralPath $put[$_] -PathType Leaf) })
    if (-not $main.Count) { $put = [ordered]@{} }
    try {
        foreach ($t in ($targets | Select-Object -Unique)) {
            $dst = Join-Path $romfs $t
            if (-not $put.Contains($t) -and (Test-Path -LiteralPath $dst)) { Remove-Item -LiteralPath $dst -Force }
        }
        foreach ($t in @($put.Keys)) {
            $dst = Join-Path $romfs $t
            if (Test-Path -LiteralPath $put[$t] -PathType Leaf) {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
                Copy-Item -LiteralPath $put[$t] -Destination $dst -Force
            } elseif (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Force }
        }
        if ($put.Count) { return $tunic }
    } catch { }
    return ''
}

# With no colour chosen (coop-options.ini's color behind its '#'), the others see you in your player
# number's colour (1 blue, 2 red), which is known only once the game has joined. Your own Link wears
# the tunic put in before the game starts, so it stayed green (the hand test of a768c36). The colour of
# the number you will most likely get is taken now, for both: the one who hosts (Host.bat,
# Host-Hookshot.bat) joins first, player 1, blue; the others red (a relay join also goes to 127.0.0.1:
# that is no host). It is passed as LICHII_COOP_COLOR, so the others see the same colour your Link wears
# even when the number turns out another. Nothing with a colour chosen, with LICHII_COOP_TUNICS=0, or
# with no server (alone). Returns the colour taken, or ''.
function Set-KitDefaultColor([string]$Mode, [string]$Server) {
    if ($env:LICHII_COOP_COLOR -or $env:LICHII_COOP_TUNICS -match '^(0|off)$') { return '' }
    if ($Mode -ne 'host' -and -not $Server) { return '' }
    $local = $Mode -eq 'host'
    $color = if ($local) { 'blue' } else { 'red' }
    $env:LICHII_COOP_COLOR = $color
    $who = if ($local) { 'you host, so you join first: player 1' } else { 'player 2, the first to join a host' }
    Write-Host "Your colour: $color (none chosen in coop-options.ini; $who). Pick one with color = ... there."
    return $color
}

# The puppet pack before the game starts, the same for the emulator and Hookshot (play.ps1): the pack
# hints, your own Link in your colour (#202: LICHII_COOP_SELF_TUNIC and the tile over the game's), and with
# no pack the puppets off. $Manifest: the pack's coop_puppet.json (the emulator's load\<title>\coop-puppet,
# Hookshot's mods\contents\<title>\coop-puppet). $Missing: the lines to say when there is no pack.
# Hookshot indexes its mod files once at start, so this runs before Start-Process there too.
# Returns $true when the pack is there. -Painted: Longshot's co-op module paints your colour on green's
# tunic files (Set-KitSelfTunic -Painted).
function Use-KitPuppetPack([string]$Manifest, [string]$ToolDir, [string[]]$Missing, [switch]$Painted) {
    if (Test-Path -LiteralPath $Manifest) {
        Write-Host 'Puppet pack: found.'
        # Made before sound or before the kit's pack tool changed: say so, start anyway.
        $hint = Get-PuppetPackHint $Manifest $ToolDir
        if ($hint) { Write-Host $hint }
        # Your own Link in your colour (#202); LICHII_COOP_TUNICS=0 keeps everyone green.
        $want = if ($env:LICHII_COOP_TUNICS -match '^(0|off)$') { '' } else { $env:LICHII_COOP_COLOR }
        $self = Set-KitSelfTunic (Split-Path -Parent $Manifest) $want -Painted:$Painted
        if ($self) {
            $env:LICHII_COOP_SELF_TUNIC = $self
            if ($self -eq 'green' -and $want -and (Get-KitTunic $want) -and (Get-KitTunic $want) -ne 'green') {
                Write-Host "Your Link: your tunic colour ($want) is painted on in the game (coop-options.ini color; the Goron and Zora tunics stay as they are)."
            } else {
                Write-Host "Your Link: the $self tunic (coop-options.ini color; the Goron and Zora tunics stay as they are)."
            }
        }
        elseif ($env:LICHII_COOP_SELF_TUNIC) { Remove-Item env:LICHII_COOP_SELF_TUNIC }
        if ($want -and -not $self -and (Get-KitTunic $want) -ne 'green') {
            Write-Host "Your Link stays green: the puppet pack has no tunic of your colour. $script:KitPackRemake"
        }
        return $true
    }
    foreach ($line in $Missing) { Write-Host $line }
    if ($env:LICHII_COOP_PUPPET) { Remove-Item env:LICHII_COOP_PUPPET; Write-Host 'Option: LICHII_COOP_PUPPET off (no pack)' }
    if ($env:LICHII_COOP_SELF_TUNIC) { Remove-Item env:LICHII_COOP_SELF_TUNIC }
    return $false
}

# Why the puppet pack cannot be made now, in plain words (player report: a player saw the others' names but no
# Links, and only longshot_log.txt said why). $Remembered: the game file hookshot\game-files.txt names ('' when
# none). The pack is made from that file (the .nup), so a file moved, renamed or on a drive not plugged in is the
# usual cause. Lines for the window; the last one says what happens without the pack.
function Get-KitPackSourceText([string]$Remembered) {
    $out = @()
    if (-not $Remembered) {
        $out += 'Puppet pack: not made. It is made from your game file (the .nup), and this kit does not know where it is'
        $out += '  (hookshot\game-files.txt is missing).'
    } else {
        $out += "Puppet pack: not made. Your game file is not at $Remembered any more"
        $out += '  (hookshot\game-files.txt names it): it was moved, renamed or deleted, or its drive is not plugged in.'
        $out += '  Put it back there, or give its new place below.'
    }
    $out += '  Without the pack the other players show as names only (no Link). Every start tries again.'
    return $out
}

# The drag prompt for the game file is asked once, not at every start (a player away from the window waited 2 minutes
# each time): the key is the game file named ('' for none) and the pack tool's hash, so a new game file or a new tool
# asks again. Test-KitPackAsked: the note at $Note holds that key.
function Get-KitPackAskKey([string]$Remembered, [string]$ToolDir) {
    $tool = ''
    try { $tool = "$(Get-PuppetToolHash $ToolDir)" } catch { }
    return "$Remembered|$tool"
}
function Test-KitPackAsked([string]$Note, [string]$Key) {
    try {
        if (-not (Test-Path -LiteralPath $Note -PathType Leaf)) { return $false }
        return ([System.IO.File]::ReadAllText($Note).Trim() -eq $Key.Trim())
    } catch { return $false }
}

# The game file (.nup) when hookshot\game-files.txt names none or one that is gone (pack-retry: players whose
# pack was never made because the .nup moved): the usual places, the folder the remembered file was in first,
# then the kit, the kit's parent, Downloads and the Desktop (each with one level of subfolders). $Check says
# whether a file is the game (dumpcheck.py: the right title and version, from its header). The one with the
# remembered file's name wins; else the only good one. '' when none or several (the caller asks then).
function Find-KitGameNup([string]$Remembered, [string]$Kit, [scriptblock]$Check, [string[]]$Places = $null) {
    if ($null -eq $Places) {
        $Places = @()
        if ($Remembered) { try { $Places += Split-Path -Parent $Remembered } catch { } }
        $Places += $Kit
        try { $Places += Split-Path -Parent $Kit } catch { }
        $Places += Join-Path $env:USERPROFILE 'Downloads'
        try { $Places += [Environment]::GetFolderPath('Desktop') } catch { }
    }
    $seen = @{}
    $good = @()
    foreach ($place in $Places) {
        if (-not $place -or -not (Test-Path -LiteralPath $place -PathType Container)) { continue }
        $files = @(Get-ChildItem -LiteralPath $place -Filter '*.nup' -File -Depth 1 -ErrorAction SilentlyContinue)
        foreach ($f in $files) {
            $key = $f.FullName.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            $ok = $false
            try { $ok = [bool](& $Check $f.FullName) } catch { }
            if ($ok) { $good += $f }
        }
    }
    if (-not $good) { return '' }
    if ($Remembered) {
        $leaf = Split-Path -Leaf $Remembered
        $same = @($good | Where-Object { $_.Name -ieq $leaf })
        if ($same.Count -eq 1) { return $same[0].FullName }
    }
    if ($good.Count -eq 1) { return $good[0].FullName }
    return ''
}

# --- Hookshot: what the kit's game needs before it starts (updates without player steps) ---

# The kit's Hookshot game folder: hookshot\game-folder.txt (hookshot-build.ps1 writes it), else hookshot\game.
# The file holds a path relative to the kit (hookshot\game) or, for -Out, a full one. Kits up to 0.10.1 wrote
# their own hookshot\game as a full path: a copy of such a kit (a player's "oot-coop-v0.10.1 - Copy" for two
# players on one PC) or a moved one then named the other kit's game, and both started one exe. A full path that
# is another kit's default game (<folder>\hookshot\game) is never followed: this kit uses its own.
function Get-KitHookshotGame([string]$Kit) {
    $own = Join-Path $Kit 'hookshot\game'
    $where = Join-Path $Kit 'hookshot\game-folder.txt'
    if (Test-Path -LiteralPath $where) {
        $g = "$(Get-Content -LiteralPath $where -TotalCount 1)".Trim()
        if ($g -and -not [System.IO.Path]::IsPathRooted($g)) { return (Join-Path $Kit $g) }
        if ($g -and -not (Test-KitOtherKitGame $Kit $g)) { return $g }
    }
    return $own
}

# A folder removed or moved while something may still hold a file in it for a moment: antivirus scanning a fresh
# unzip, Explorer or the search indexer, a python.exe that just ended (a 0.10.1 player: "Access to the path
# 'D:\coop\hookshot\tools.new' is denied", as admin too). Tried again for $Seconds; a move then falls back to
# robocopy /MOVE (it copies what is held and leaves only that behind). Returns '' when done, else why not.
# ($OnWait: tests; it runs before each wait.)
function Remove-KitFolderRetry([string]$Path, [int]$Seconds = 30, [int]$WaitMs = 500, [scriptblock]$OnWait = $null) {
    $until = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ($true) {
        if (-not (Test-Path -LiteralPath $Path)) { return '' }
        try { Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop; return '' } catch { $why = $_.Exception.Message }
        if ([DateTime]::UtcNow -ge $until) { return $why }
        if ($OnWait) { & $OnWait }
        Start-Sleep -Milliseconds $WaitMs
    }
}
function Move-KitFolderRetry([string]$From, [string]$To, [int]$Seconds = 30, [int]$WaitMs = 500, [scriptblock]$OnWait = $null) {
    $until = [DateTime]::UtcNow.AddSeconds($Seconds)
    $why = ''
    while ($true) {
        # (Directory.Move: one rename or nothing; a Move-Item that fails half way leaves a part of $To, and the next
        # try then moves $From INTO it)
        try { [System.IO.Directory]::Move($From, $To); return '' } catch { $why = $_.Exception.InnerException.Message; if (-not $why) { $why = $_.Exception.Message } }
        if ([DateTime]::UtcNow -ge $until) { break }
        if ($OnWait) { & $OnWait }
        Start-Sleep -Milliseconds $WaitMs
    }
    if (-not (Test-Path -LiteralPath $From)) { return $why }
    & robocopy $From $To /MOVE /E /R:3 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
    $code = $LASTEXITCODE
    $global:LASTEXITCODE = 0
    if ($code -ge 8) { return "$why (robocopy $code)" }
    # (what robocopy copied but could not delete stays in $From: gone later, or with the next build)
    Remove-KitFolderRetry $From 2 $WaitMs | Out-Null
    return ''
}
# The cloud-sync client whose synced folder holds $Folder (OneDrive, Google Drive, Dropbox, iCloud...: the sync roots
# Windows knows, HKLM ...\Explorer\SyncRootManager), or ''. A sync client opens each new file to upload it.
function Get-KitSyncRoot([string]$Folder) {
    try { $full = [System.IO.Path]::GetFullPath($Folder).TrimEnd('\') + '\' } catch { return '' }
    $base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager'
    foreach ($k in @(Get-ChildItem -LiteralPath $base -ErrorAction SilentlyContinue)) {
        $roots = Get-ItemProperty -LiteralPath (Join-Path $k.PSPath 'UserSyncRoots') -ErrorAction SilentlyContinue
        if (-not $roots) { continue }
        foreach ($p in $roots.PSObject.Properties) {
            if ($p.Name -like 'PS*' -or -not ($p.Value -is [string]) -or -not $p.Value) { continue }
            $r = $p.Value.TrimEnd('\') + '\'
            if ($full.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)) { return ($k.PSChildName -split '!')[0] }
        }
    }
    return ''
}

# Who holds files in $Folder: programs running from it (a python.exe of the build still ending), and, for the files in it
# no one else may open now (at most $MaxFiles looked at, the first 16 held asked about), the programs Windows' Restart
# Manager names (an antivirus, Explorer, the search indexer, a sync client). Names only, '' parts left out.
function Get-KitFolderHolders([string]$Folder, [int]$MaxFiles = 40000) {
    $out = @()
    try { $full = [System.IO.Path]::GetFullPath($Folder).TrimEnd('\') + '\' } catch { return @() }
    if (-not (Test-Path -LiteralPath $Folder)) { return @() }
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($full, [StringComparison]::OrdinalIgnoreCase) })) {
        $out += "$($p.ProcessName).exe (process $($p.Id), started from this folder)"
    }
    $held = @()
    foreach ($f in @(Get-ChildItem -LiteralPath $Folder -Recurse -File -Force -ErrorAction SilentlyContinue | Select-Object -First $MaxFiles)) {
        try { ([System.IO.File]::Open($f.FullName, 'Open', 'Read', 'None')).Dispose() } catch { $held += $f.FullName; if ($held.Count -ge 16) { break } }
    }
    if ($held) {
        try {
            if (-not ('KitRestartManager' -as [type])) {
                Add-Type -ErrorAction Stop -TypeDefinition @'
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class KitRestartManager {
    [StructLayout(LayoutKind.Sequential)] struct RM_UNIQUE_PROCESS { public int dwProcessId; public System.Runtime.InteropServices.ComTypes.FILETIME ProcessStartTime; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct RM_PROCESS_INFO {
        public RM_UNIQUE_PROCESS Process;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string strAppName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string strServiceShortName;
        public int ApplicationType; public uint AppStatus; public uint TSSessionId; [MarshalAs(UnmanagedType.Bool)] public bool bRestartable; }
    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)] static extern int RmStartSession(out uint h, int flags, StringBuilder key);
    [DllImport("rstrtmgr.dll")] static extern int RmEndSession(uint h);
    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)] static extern int RmRegisterResources(uint h, uint nFiles, string[] files, uint nApps, RM_UNIQUE_PROCESS[] apps, uint nSvc, string[] svcs);
    [DllImport("rstrtmgr.dll")] static extern int RmGetList(uint h, out uint needed, ref uint n, [In, Out] RM_PROCESS_INFO[] info, ref uint reasons);
    public static string[] Holders(string[] files) {
        var names = new List<string>();
        uint h; var key = new StringBuilder(64);
        if (RmStartSession(out h, 0, key) != 0) return names.ToArray();
        try {
            if (RmRegisterResources(h, (uint)files.Length, files, 0, null, 0, null) != 0) return names.ToArray();
            uint needed, n = 0, reasons = 0;
            int r = RmGetList(h, out needed, ref n, null, ref reasons);
            if (r == 234 && needed > 0) {
                var info = new RM_PROCESS_INFO[needed]; n = needed;
                if (RmGetList(h, out needed, ref n, info, ref reasons) == 0)
                    for (int i = 0; i < n; i++) names.Add(info[i].strAppName + " (process " + info[i].Process.dwProcessId + ")");
            }
        } finally { RmEndSession(h); }
        return names.ToArray();
    }
}
'@
            }
            $out += @([KitRestartManager]::Holders([string[]]$held))
        } catch { }
        if (-not $out) { $out += "a program Windows would not name (it holds $(Split-Path -Leaf $held[0]) and $($held.Count - 1) more)" }
    }
    @($out | Where-Object { $_ } | Select-Object -Unique)
}

# What to say when a folder of the build stays held ($Why: Remove- or Move-KitFolderRetry's answer). $Holders: who
# holds it (Get-KitFolderHolders, worked out here when not given); $Sync: the sync client of its folder (Get-KitSyncRoot).
function Get-KitHeldFolderText([string]$Folder, [string]$Why, [string[]]$Holders = $null, [string]$Sync = $null) {
    if ($null -eq $Holders) { try { $Holders = @(Get-KitFolderHolders $Folder) } catch { $Holders = @() } }
    if ($null -eq $Sync) { $Sync = Get-KitSyncRoot $Folder }
    $text = "Windows would not let the build move or delete $Folder ($Why). Something still has a file in it open"
    $text += $(if ($Holders) { ': ' + ($Holders -join ', ') + '.' } else { '.' })
    if ($Sync) {
        $text += " This folder is in a cloud-sync folder ($Sync), and the sync client opens every new file to upload it: move the kit out of it, to a short folder such as C:\Squadala, and start it there."
        return $text
    }
    $text += (' Most often it is the antivirus scanning the files just unpacked, or an Explorer window or the search indexer. ' +
        'Close Explorer windows on this kit''s folder; in Windows Security > Virus & threat protection > Manage settings > ' +
        'Exclusions > Add an exclusion > Folder, add this kit''s folder (another antivirus: its own exclusions); then start it again (what was downloaded is kept).')
    return $text
}
# Is $Game the default game folder of a kit other than $Kit (<that kit>\hookshot\game)?
function Test-KitOtherKitGame([string]$Kit, [string]$Game) {
    try {
        $full = [System.IO.Path]::GetFullPath($Game).TrimEnd('\')
        $mine = [System.IO.Path]::GetFullPath((Join-Path $Kit 'hookshot\game')).TrimEnd('\')
    } catch { return $false }
    if ($full -ieq $mine) { return $false }
    return ((Split-Path -Leaf $full) -ieq 'game') -and ((Split-Path -Leaf (Split-Path -Parent $full)) -ieq 'hookshot')
}

# A built game another kit on this PC has, made from the same release, pin and patches as $Kit wants
# ($Stamp: Get-KitHookshotStamp), so a second kit (unzipped twice or copied) copies it instead of a 10 to 45
# minute build. $Others: the other kit folders (kits.txt and the kit game-folder.txt names). $Dlss: this kit's NVIDIA
# DLSS choice (hookshot\dlss.txt); only a game with DLSS as chosen is taken (Test-KitHookshotDlss), so a kit never
# gets NVIDIA's files without its own opt-in, nor a game without them that it would build again at once. Returns the
# game folder, or ''.
function Find-KitHookshotGameToCopy([string]$Kit, [string]$Stamp, [string[]]$Others, [bool]$Dlss = $false) {
    if (-not $Stamp) { return '' }
    $mine = [System.IO.Path]::GetFullPath($Kit).TrimEnd('\')
    foreach ($o in @($Others | Where-Object { $_ } | Sort-Object -Unique)) {
        try { $of = [System.IO.Path]::GetFullPath($o).TrimEnd('\') } catch { continue }
        if ($of -ieq $mine) { continue }
        $g = Join-Path $of 'hookshot\game'
        if (-not (Get-KitHookshotExe $g)) { continue }
        $s = Join-Path $g 'coop-built-from.txt'
        if ((Test-KitHookshotDlss $g) -ne $Dlss) { continue }
        if ((Test-Path -LiteralPath $s) -and [System.IO.File]::ReadAllText($s) -eq $Stamp) { return $g }
    }
    return ''
}

# A built game in the co-op game cache on this PC made from $Stamp (Get-KitHookshotStamp): <cache>\<key>\game, where
# <cache> is OOT_COOP_HS_CACHE or %LOCALAPPDATA%\oot-coop\hs-cache (tools/hookshot/hs.py fills it; it keeps the last 3
# builds). Its game folders hold nothing of a player's. $Dlss: as Find-KitHookshotGameToCopy's. Returns the game folder, or ''.
function Find-KitHookshotCachedGame([string]$Stamp, [string]$Root = '', [bool]$Dlss = $false) {
    if (-not $Stamp) { return '' }
    if (-not $Root) {
        $Root = if ($env:OOT_COOP_HS_CACHE) { $env:OOT_COOP_HS_CACHE } elseif ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'oot-coop\hs-cache' } else { '' }
    }
    if (-not $Root -or -not (Test-Path -LiteralPath $Root -PathType Container)) { return '' }
    # (as hs.py's key_of: line endings and trailing space do not count)
    $norm = { param($t) (@(("$t" -replace "`r", '').Trim() -split "`n") | ForEach-Object { $_.TrimEnd() }) -join "`n" }
    $want = & $norm $Stamp
    foreach ($d in @(Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^[0-9a-f]{20}$' })) {
        $s = Join-Path $d.FullName 'stamp.txt'
        $g = Join-Path $d.FullName 'game'
        if (-not (Test-Path -LiteralPath $s -PathType Leaf) -or -not (Get-KitHookshotExe $g)) { continue }
        if ((Test-KitHookshotDlss $g) -ne $Dlss) { continue }
        if ((& $norm ([System.IO.File]::ReadAllText($s))) -eq $want) { return $g }
    }
    return ''
}

# A game folder built with NVIDIA DLSS (#234): make_game.py's place_ngx_runtime puts DLSS's runtime in its ngx folder
# when the build had NVIDIA's SDK, and removes it when a build had none.
function Test-KitHookshotDlss([string]$Game) {
    return [bool]($Game -and (Test-Path -LiteralPath (Join-Path $Game 'ngx\nvngx_dlss.dll')))
}

# NVIDIA's DLSS license, as the pinned Longshot's own tools\scripts\ngx_sdk.py names it (LICENSE_URL at its COMMIT), so a
# pin that moves NVIDIA's commit moves the link too. Without those tools: the license in NVIDIA's repository.
function Get-KitNgxLicenseUrl([string]$Tools) {
    $py = if ($Tools) { Join-Path $Tools 'scripts\ngx_sdk.py' } else { '' }
    if ($py -and (Test-Path -LiteralPath $py)) {
        $m = Select-String -LiteralPath $py -Pattern '^COMMIT\s*=\s*"([0-9a-f]{40})"' | Select-Object -First 1
        if ($m) { return "https://github.com/NVIDIA/DLSS/blob/$($m.Matches[0].Groups[1].Value)/LICENSE.txt" }
    }
    return 'https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt'
}

# longshot.exe of a game folder (a builder's game folder has it in bin\), or $null.
function Get-KitHookshotExe([string]$Game) {
    foreach ($p in (Join-Path $Game 'bin\longshot.exe'), (Join-Path $Game 'longshot.exe')) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}

# The game folders collect-logs looks in for longshot_log.txt, as the kit's start finds its game, then wider (a player
# could not find the logs, and the zip had none: 0.11.0): the start's own game (Get-KitHookshotGame, which follows
# hookshot\game-folder.txt), the kit's default hookshot\game, the folder game-folder.txt names even when the start
# would not follow it (another kit's game), and the folder of each longshot.exe running now. $Running: tests.
# Returns each longshot.exe once, in that order.
function Get-KitLogGameExes([string]$Kit, [string[]]$Running = $null) {
    $games = @((Get-KitHookshotGame $Kit), (Join-Path $Kit 'hookshot\game'))
    $where = Join-Path $Kit 'hookshot\game-folder.txt'
    if (Test-Path -LiteralPath $where) {
        $g = "$(Get-Content -LiteralPath $where -TotalCount 1)".Trim()
        if ($g) { $games += $(if ([System.IO.Path]::IsPathRooted($g)) { $g } else { Join-Path $Kit $g }) }
    }
    $exes = @($games | ForEach-Object { Get-KitHookshotExe $_ })
    if ($null -eq $Running) {
        $Running = @(Get-Process longshot, hookshot -ErrorAction SilentlyContinue | ForEach-Object { $_.Path } | Where-Object { $_ })
    }
    $exes += $Running
    $seen = @{}
    foreach ($e in @($exes | Where-Object { $_ -and (Test-Path -LiteralPath $_) })) {
        $k = ([System.IO.Path]::GetFullPath($e)).ToLowerInvariant()
        if (-not $seen.ContainsKey($k)) { $seen[$k] = $true; $e }
    }
}

# Where a game's coop.ini and mods\ are (and its longshot_log*.txt up to Longshot rc.3): beside longshot.exe, and for
# a builder's bin\ layout (bin\longshot.exe beside image.bin) the game folder above it, where Longshot writes them
# (hos_paths.h: hos_game_path_a gives the folder's path then). Each folder once, the game folder first.
# Longshot rc.4 writes its log into logs\ in that folder: Get-KitHookshotLogFiles looks in both.
function Get-KitHookshotLogDirs([string]$Exe) {
    if (-not $Exe) { return @() }
    $dir = Split-Path -Parent $Exe
    $up = Split-Path -Parent $dir
    if ((Split-Path -Leaf $dir) -ieq 'bin' -and $up -and (Test-Path -LiteralPath (Join-Path $up 'image.bin'))) {
        return @($up, $dir)
    }
    return @($dir)
}

# The game's log files (longshot_log*.txt, hookshot_log*.txt) for one game folder of Get-KitHookshotLogDirs: in it
# (Longshot up to rc.3, 0.11.1's games) and in its logs\ (rc.4). Each as @{ File; Sub } with Sub '' or 'logs'.
function Get-KitHookshotLogFiles([string]$Dir) {
    if (-not $Dir) { return @() }
    foreach ($sub in '', 'logs') {
        $d = if ($sub) { Join-Path $Dir $sub } else { $Dir }
        if (-not (Test-Path -LiteralPath $d -PathType Container)) { continue }
        Get-ChildItem (Join-Path $d '*') -Include 'longshot_log*.txt', 'hookshot_log*.txt' -File -ErrorAction SilentlyContinue |
            ForEach-Object { [pscustomobject]@{ File = $_; Sub = $sub } }
    }
}

# The stamp the kit's hookshot-build.ps1 writes to the game's coop-built-from.txt: release, pin, patches.
function Get-KitHookshotStamp([string]$Kit) {
    $hs = Join-Path $Kit 'hookshot'
    $keys = @{}
    foreach ($f in 'PIN', 'TOOLS.txt') {
        foreach ($line in @(Get-Content -LiteralPath (Join-Path $hs $f) -ErrorAction SilentlyContinue)) {
            if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $keys[$Matches[1]] = $Matches[2] }
        }
    }
    if (-not $keys.release -or -not $keys.commit) { return '' }
    $patches = @(Get-ChildItem -LiteralPath (Join-Path $hs 'patches') -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
    $hash = (($patches | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }) -join ',')
    return "release=$($keys.release)`ncommit=$($keys.commit)`npatches=$hash"
}

# What the kit's Hookshot game needs before it starts:
#   'quarantine' the co-op module is in neither the kit nor the game (antivirus took it, as a false alarm):
#             a build cannot help, the player restores it (Get-KitHookshotQuarantineText)
#   'game'    no game built yet (the first build, 10 to 45 minutes)
#   'moved'   game-folder.txt names a game that is not there any more (moved or deleted): built again,
#             or found with Build-Hookshot-Coop.bat -Out <its new folder>
#   'pin'     the kit moved to another Hookshot release or pin (a full game build, about 15 minutes)
#   'patches' the co-op patches changed (the game built again, only what changed: a few minutes)
#   'module'  the co-op module changed, or the puppet pack is missing or stale (co-op and the pack put in
#             again: seconds to a minute; without a pack the other players cannot be shown), or the kit's
#             randomizer (hookshot\OOTRR.txt) is not in the game yet
#   'offline' only the kit's randomizer is missing and this PC has no internet: the game starts as it is
#             (play.ps1 says one line), the next start with internet puts it in
#   ''        nothing: start the game
function Get-KitHookshotNeed([string]$Kit) {
    $game = Get-KitHookshotGame $Kit
    $exe = Get-KitHookshotExe $game
    # (a kit always carries hookshot\hookshot_coop.dll; with the game's copy gone too, nothing can put co-op in)
    $gameHas = $exe -and (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $exe) 'hookshot_coop.dll'))
    # (only a kit with the Hookshot part: one made without it has no module to lose and no game to start)
    if (-not $gameHas -and (Test-Path -LiteralPath (Join-Path $Kit 'hookshot\PIN')) -and
        -not (Test-Path -LiteralPath (Join-Path $Kit 'hookshot\hookshot_coop.dll'))) { return 'quarantine' }
    if (-not $exe) {
        if ($game -ine (Join-Path $Kit 'hookshot\game')) { return 'moved' }
        return 'game'
    }
    $stampFile = Join-Path $game 'coop-built-from.txt'
    $have = if (Test-Path -LiteralPath $stampFile) { [System.IO.File]::ReadAllText($stampFile) } else { '' }
    $want = Get-KitHookshotStamp $Kit
    if (-not $want) { return '' }   # (a kit without hookshot\PIN or TOOLS.txt cannot build: nothing to do here)
    if ($have -ne $want) {
        $base = { param($s) (@($s -split "`n") | Where-Object { $_ -match '^(release|commit)=' }) -join "`n" }
        if ((& $base $have) -ne (& $base $want)) { return 'pin' }
        return 'patches'
    }
    $exeDir = Split-Path -Parent $exe
    $kitModule = Join-Path $Kit 'hookshot\hookshot_coop.dll'
    $gameModule = Join-Path $exeDir 'hookshot_coop.dll'
    if (Test-Path -LiteralPath $kitModule) {
        if (-not (Test-Path -LiteralPath $gameModule)) { return 'module' }
        if ((Get-FileHash -LiteralPath $kitModule -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $gameModule -Algorithm SHA256).Hash) {
            return 'module'
        }
    }
    $manifest = Join-Path $exeDir 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json'
    if (Test-KitPuppetPackStale $manifest (Join-Path $Kit 'tools\puppet_pack')) { return 'module' }
    if (Get-KitStaleRandoSeeds $exeDir) { return 'module' }   # (a seed built before the puppet pack: it would abort)
    if (Test-KitOotrrStale $Kit $exeDir) {
        # (an offline player: the module step would only try the OOTRR download again, on every start. Everything
        # else is current and checked above, so the game starts as it is; a zip already downloaded and matching its
        # pinned SHA-256 needs no network, so that one still goes in)
        if (-not (Test-KitOotrrZipReady $Kit) -and (Test-KitOffline)) { return 'offline' }
        return 'module'
    }
    return ''
}

# The kit's OOTRR zip (hookshot\OOTRR.txt) is in hookshot\downloads already and matches its pinned SHA-256.
function Test-KitOotrrZipReady([string]$Kit) {
    $pin = Join-Path $Kit 'hookshot\OOTRR.txt'
    if (-not (Test-Path -LiteralPath $pin)) { return $false }
    $keys = @{}
    foreach ($line in @(Get-Content -LiteralPath $pin -ErrorAction SilentlyContinue)) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $keys[$Matches[1]] = $Matches[2] }
    }
    if ($keys.windows -notmatch '^[A-Za-z0-9._-]+$' -or -not $keys.windows_sha256) { return $false }
    $zip = Join-Path $Kit "hookshot\downloads\$($keys.windows)"
    if (-not (Test-Path -LiteralPath $zip)) { return $false }
    return (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -eq $keys.windows_sha256.ToUpperInvariant()
}

# No internet on this PC: no network adapter up, or github.com (where the kit's downloads are) cannot be reached
# in a few seconds. OOT_COOP_OFFLINE=1 says offline and =0 online, with no probe (tests). Behind a proxy it is never
# offline: a direct connect is often blocked there while the kit's downloads (the system proxy) work.
function Test-KitOffline {
    if ($env:OOT_COOP_OFFLINE -eq '1') { return $true }
    if ($env:OOT_COOP_OFFLINE -eq '0') { return $false }
    if (Test-KitProxied) { return $false }
    try { if (-not [System.Net.NetworkInformation.NetworkInterface]::GetIsNetworkAvailable()) { return $true } } catch { }
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $try = $client.BeginConnect('github.com', 443, $null, $null)
        if (-not $try.AsyncWaitHandle.WaitOne(4000)) { return $true }
        $client.EndConnect($try)
        return $false
    } catch { return $true } finally { $client.Close() }
}

# github.com goes through a proxy here ($Proxy: the system's, as Invoke-WebRequest uses it; tests pass their own).
# Can't tell: yes (online is the safe answer, the download is then tried as before).
function Test-KitProxied($Proxy = $null) {
    try {
        if (-not $Proxy) { $Proxy = [System.Net.WebRequest]::GetSystemWebProxy() }
        if (-not $Proxy) { return $false }
        $u = [uri]'https://github.com'
        return (-not $Proxy.IsBypassed($u)) -and ($Proxy.GetProxy($u) -ne $u)
    } catch { return $true }
}

# The randomizer the kit brings (hookshot\OOTRR.txt) is not in the game's tools\ootrr yet, or another release is:
# the module step (hookshot-build.ps1) puts it in. A kit without OOTRR.txt brings none.
function Test-KitOotrrStale([string]$Kit, [string]$ExeDir) {
    $pin = Join-Path $Kit 'hookshot\OOTRR.txt'
    if (-not (Test-Path -LiteralPath $pin)) { return $false }
    $keys = @{}
    foreach ($line in @(Get-Content -LiteralPath $pin -ErrorAction SilentlyContinue)) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $keys[$Matches[1]] = $Matches[2] }
    }
    if (-not $keys.release -or -not $keys.windows_sha256) { return $false }
    $dir = Join-Path $ExeDir 'tools\ootrr'
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'ootrr.exe'))) { return $true }
    $stamp = Join-Path $dir 'coop-ootrr.txt'
    $have = if (Test-Path -LiteralPath $stamp) { (Get-Content -LiteralPath $stamp -TotalCount 1).Trim() } else { '' }
    return $have -ne "$($keys.release) $($keys.windows_sha256)"
}

# Where the last OOTRR setup said how it went, beside the game's tools\ootrr: 'ok <release>' or 'failed <release>: <why>'
# (one line). The co-op module shows the failure in the game's chat when it finds no ootrr.exe (coop_rando.cpp
# OotrrMissingText), and collect-logs.cmd sends it, so a player who never saw the start window's line still learns why.
$KitOotrrStatusFile = 'tools\ootrr-setup.txt'

function Write-KitOotrrStatus([string]$ExeDir, [string]$Text) {
    try {
        New-Item -ItemType Directory -Force -Path (Join-Path $ExeDir 'tools') | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $ExeDir $KitOotrrStatusFile), "$Text`r`n")
    } catch { }
}

# '' when ootrr.exe starts and ends by itself (its 'shuffle 1 1' prints one number in well under a second and touches
# no file); else why not. An antivirus that blocks or quarantines the program makes the start fail (or the file
# vanish a moment after it was written), which an existence check alone never sees: Velvet's 0.11.5 kit (2026-11-19)
# had a tools\ootrr without ootrr.exe on every start. -Wait: how long to look again for an antivirus's late removal.
function Test-KitOotrrRuns([string]$Exe, [int]$WaitMs = 1500) {
    if (-not (Test-Path -LiteralPath $Exe)) { return 'ootrr.exe is not there after unpacking' }
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList 'shuffle', '1', '1' -WindowStyle Hidden -PassThru -ErrorAction Stop
        $null = $p.Handle   # (Windows PowerShell 5.1: without the handle taken now, ExitCode reads empty after the wait)
        if (-not $p.WaitForExit(30000)) { try { $p.Kill() } catch { }; return 'ootrr.exe did not finish a 1-second test in 30 s' }
        # (review-0116-j N1: a program that starts but fails, e.g. a damaged or half-written exe, is not swapped in either)
        if ($p.ExitCode -ne 0) { return "ootrr.exe failed a 1-second test (exit $($p.ExitCode))" }
    } catch {
        return "Windows did not let ootrr.exe run ($($_.Exception.Message.Trim()))"
    }
    if ($WaitMs -gt 0) { Start-Sleep -Milliseconds $WaitMs }
    if (-not (Test-Path -LiteralPath $Exe)) { return 'ootrr.exe was removed right after it was unpacked (an antivirus took it)' }
    return ''
}

# The randomizer the kit pins (hookshot\OOTRR.txt) into <ExeDir>\tools\ootrr, all or nothing: the zip is downloaded
# (or taken from $Downloads when its SHA-256 is the pinned one), unpacked into tools\ootrr.new, tried
# (Test-KitOotrrRuns), and only then swapped for the old folder. A failure at any step leaves the OOTRR that was
# there as it was (the old way deleted tools\ootrr first, so any later failure left the game with none, every start)
# and writes why into tools\ootrr-setup.txt. A folder a stopped swap left (tools\ootrr.old, no tools\ootrr) is put
# back first. Never touches saves, seeds or other mods. Returns @{ Ok; Why; Release; Had; Lines }:
#   Ok  $true when the pinned OOTRR is in place (already, or now); Why '' or why not ('offline': no internet, no try)
#   Had the release that was there before ('' for none); Lines what to show.
function Install-KitOotrr([string]$Kit, [string]$ExeDir, [string]$Downloads = '') {
    $r = @{ Ok = $false; Why = ''; Release = ''; Had = ''; Lines = @() }
    $pin = Join-Path $Kit 'hookshot\OOTRR.txt'
    if (-not (Test-Path -LiteralPath $pin)) { $r.Ok = $true; return $r }   # (a kit without OOTRR brings none)
    $op = @{}
    foreach ($line in @(Get-Content -LiteralPath $pin -ErrorAction SilentlyContinue)) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $op[$Matches[1]] = $Matches[2] }
    }
    $r.Release = "$($op.release)"
    if (-not ($op.release -and $op.url -and $op.windows -match '^[A-Za-z0-9._-]+$' -and $op.windows_sha256 -match '^[0-9a-fA-F]{64}$')) {
        $r.Why = 'hookshot\OOTRR.txt is incomplete'
        $r.Lines += 'hookshot\OOTRR.txt is incomplete: the randomizer is left out.'
        return $r
    }
    $sha = $op.windows_sha256.ToLowerInvariant()
    $want = "$($op.release) $sha"
    $tools = Join-Path $ExeDir 'tools'
    $dir = Join-Path $tools 'ootrr'
    $new = Join-Path $tools 'ootrr.new'
    $old = Join-Path $tools 'ootrr.old'
    $stamp = Join-Path $dir 'coop-ootrr.txt'
    # (a swap stopped between its two moves: the old folder goes back first)
    if (-not (Test-Path -LiteralPath $dir) -and (Test-Path -LiteralPath (Join-Path $old 'ootrr.exe'))) {
        try { [System.IO.Directory]::Move($old, $dir) } catch { }
    }
    $have = if (Test-Path -LiteralPath $stamp) { "$(Get-Content -LiteralPath $stamp -TotalCount 1)".Trim() } else { '' }
    $r.Had = if ($have) { ($have -split ' ')[0] } else { '' }
    if ($have -eq $want -and (Test-Path -LiteralPath (Join-Path $dir 'ootrr.exe'))) {
        $r.Ok = $true
        $r.Lines += "randomizer: OOTRR $($op.release) -> $dir (there already)"
        Write-KitOotrrStatus $ExeDir "ok $($op.release)"
        return $r
    }
    if (-not $Downloads) { $Downloads = Join-Path $Kit 'hookshot\downloads' }
    $zip = Join-Path $Downloads $op.windows
    $why = ''
    try {
        New-Item -ItemType Directory -Force -Path $Downloads | Out-Null
        if (-not ((Test-Path -LiteralPath $zip) -and (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() -eq $sha)) {
            if (($env:OOT_COOP_OFFLINE -eq '1' -or $op.url -match '^https?://') -and (Test-KitOffline)) {
                $why = 'offline'
            } else {
                # (kit-mirror: retries and curl.exe too; optional, so two tries, not four)
                $why = Get-KitDownload @("$($op.url)/$($op.windows)") $zip $sha @() 2
                if ($why) { $why = "the download failed ($why)" }
            }
        }
        if (-not $why) {
            if (Test-Path -LiteralPath $new) { Remove-Item -LiteralPath $new -Recurse -Force -ErrorAction Stop }
            New-Item -ItemType Directory -Force -Path $new | Out-Null
            # (the zip holds one folder, OOTRR-<version>-windows-x64\: ootrr.exe, LICENSE, NOTICE, README.md)
            & "$env:SystemRoot\System32\tar.exe" -x -f $zip -C $new --strip-components=1 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { $why = "unpacking $($op.windows) failed (tar $LASTEXITCODE)" }
            else { $why = Test-KitOotrrRuns (Join-Path $new 'ootrr.exe') }
        }
        if (-not $why) {
            [System.IO.File]::WriteAllText((Join-Path $new 'coop-ootrr.txt'), "$want`n")
            if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction Stop }
            # (Directory.Move: one rename or nothing. Move-Item falls back to moving file by file when one file is held,
            # and a failure half way splits the folder in two: seen in the kit test with the old LICENSE held open)
            if (Test-Path -LiteralPath $dir) { [System.IO.Directory]::Move($dir, $old) }
            try { [System.IO.Directory]::Move($new, $dir) }
            catch {
                if ((Test-Path -LiteralPath $old) -and -not (Test-Path -LiteralPath $dir)) { try { [System.IO.Directory]::Move($old, $dir) } catch { } }
                throw
            }
            Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
        }
    } catch {
        # (a file held open: the game or its ootrr.exe still running from this folder)
        $msg = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
        $why = "it could not be put in place ($("$msg".Trim())): close the game and start again"
    }
    if (Test-Path -LiteralPath $new) { Remove-Item -LiteralPath $new -Recurse -Force -ErrorAction SilentlyContinue }
    $r.Why = $why
    $kept = if (Test-Path -LiteralPath (Join-Path $dir 'ootrr.exe')) { " The OOTRR that was there ($(if ($r.Had) { $r.Had } else { 'unknown release' })) is kept." } else { '' }
    if ($why -eq 'offline') {
        $r.Lines += "offline: the randomizer (OOTRR $($op.release)) is set up at the next start with internet.$kept"
        Write-KitOotrrStatus $ExeDir "failed $($op.release): this PC had no internet at the last start"
    } elseif ($why) {
        $r.Lines += "The randomizer (OOTRR $($op.release)) was not set up: $why.$kept Co-op and the game work without it; start again (or run Update.bat) to retry."
        if ($why -match 'antivirus|did not let') {
            $r.Lines += "  Windows Security: Virus & threat protection > Protection history: restore/allow ootrr.exe, or add $tools as an exclusion; then start again."
        }
        Write-KitOotrrStatus $ExeDir "failed $($op.release): $why"
    } else {
        $r.Ok = $true
        $r.Lines += "randomizer: OOTRR $($op.release) -> $dir"
        Write-KitOotrrStatus $ExeDir "ok $($op.release)"
    }
    return $r
}

# The resource size tables (romfs\System\Resource\*.rsizetable.zs) of the mods the game loads besides the randomizer's
# seeds: every folder under mods\ (any depth: the puppet pack is in mods\contents\<title>\coop-puppet) but seeds
# (randomizer-*), the randomizer's control folder and folders whose name starts with '-' or '_' (Longshot skips them).
# A mod switched off in the Mods tab is still read: its larger entries do no harm.
# The game uses one table, the seed's (it sorts last), so a seed must be built with all of these (coop_rando's
# StageSizeTables does the same in the game).
function Get-KitOtherSizeTables([string]$ModsDir) {
    if (-not (Test-Path -LiteralPath $ModsDir)) { return @() }
    $out = New-Object System.Collections.Generic.List[System.IO.FileInfo]
    $walk = {
        param($dir, $depth)
        foreach ($d in @(Get-ChildItem -LiteralPath $dir -Directory -ErrorAction SilentlyContinue)) {
            if ($d.Name -match '^[-_]') { continue }
            if ($depth -eq 0 -and ($d.Name -like 'randomizer-*' -or $d.Name -eq 'oot-remake-randomizer')) { continue }
            if ($d.Name -eq 'romfs') {
                foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $d.FullName 'System\Resource') -File -Filter '*.rsizetable.zs' -ErrorAction SilentlyContinue)) { $out.Add($f) }
                continue
            }
            if ($depth -lt 3) { & $walk $d.FullName ($depth + 1) }
        }
    }
    & $walk $ModsDir 0
    return $out.ToArray()
}

# Another OOTRR release replaced the game's (an update of hookshot\OOTRR.txt): its seeds are other worlds than the
# old one's for the same seed word, and the old release's seed files do not build with the new one (OOTRR 0.6.0:
# "Seeds made with older versions will not build"). So every built seed (mods\randomizer-*\coop-seed.txt) and its
# seed file go: the game makes each one again with the new OOTRR at its next start (about a minute), so every player
# of the same kit plays the same world. Run while the game is closed (the module step). The lines to show.
function Remove-KitRandoSeeds([string]$ExeDir, [string]$Release) {
    $lines = @()
    $mods = Join-Path $ExeDir 'mods'
    $control = Join-Path $mods 'oot-remake-randomizer'
    foreach ($d in @(Get-ChildItem -LiteralPath $mods -Directory -Filter 'randomizer-*' -ErrorAction SilentlyContinue)) {
        if (-not (Test-Path -LiteralPath (Join-Path $d.FullName 'coop-seed.txt'))) { continue }
        Remove-Item -LiteralPath $d.FullName -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $control "$($d.Name).ootrr") -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $control "$($d.Name).spoiler.txt") -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $d.FullName) { $lines += "randomizer: seed $($d.Name) could not be removed for OOTRR $Release" }
        else { $lines += "randomizer: seed $($d.Name) removed for OOTRR $Release`: the game makes it again at its next start (another world: start a new save)" }
    }
    return $lines
}

# Built randomizer seeds (mods\randomizer-*\coop-seed.txt) whose size table is older than another mod's (a puppet
# pack made again): the game aborts while it loads with such a seed on, so they are built again (Update-KitRandoSeeds).
function Get-KitStaleRandoSeeds([string]$ExeDir) {
    $mods = Join-Path $ExeDir 'mods'
    $others = @(Get-KitOtherSizeTables $mods)
    if (-not $others) { return @() }
    $newest = ($others | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
    $stale = @()
    foreach ($d in @(Get-ChildItem -LiteralPath $mods -Directory -Filter 'randomizer-*' -ErrorAction SilentlyContinue)) {
        if (-not (Test-Path -LiteralPath (Join-Path $d.FullName 'coop-seed.txt'))) { continue }
        $own = @(Get-ChildItem -LiteralPath (Join-Path $d.FullName 'romfs\System\Resource') -File -Filter '*.rsizetable.zs' -ErrorAction SilentlyContinue)
        # (no table of its own: the seed's build time, coop-seed.txt, so it is not built again at every start)
        $built = if ($own) { ($own | Measure-Object -Property LastWriteTimeUtc -Minimum).Minimum } else { (Get-Item -LiteralPath (Join-Path $d.FullName 'coop-seed.txt')).LastWriteTimeUtc }
        if ($built -lt $newest) { $stale += $d.Name }
    }
    return $stale
}

# Builds the stale seeds again with the kit's OOTRR (a few seconds each), beside every other mod's size table, from
# the seed file the game kept (mods\oot-remake-randomizer\<seed folder>.ootrr): host and guest seeds alike. A seed
# that cannot be built again is removed (the game builds it at its next start and asks for a restart), never left to
# abort the game. The lines to show.
function Update-KitRandoSeeds([string]$ExeDir, [string]$Ootrr = '') {
    $lines = @()
    $mods = Join-Path $ExeDir 'mods'
    $control = Join-Path $mods 'oot-remake-randomizer'
    # (-Ootrr: another OOTRR program, for the kit tests; the game's own by default)
    $ootrr = if ($Ootrr) { $Ootrr } else { Join-Path $ExeDir 'tools\ootrr\ootrr.exe' }
    $romfs = @((Join-Path $ExeDir 'romfs'), (Join-Path (Split-Path -Parent $ExeDir) 'romfs')) | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'System') } | Select-Object -First 1
    foreach ($name in @(Get-KitStaleRandoSeeds $ExeDir)) {
        $seedDir = Join-Path $mods $name
        $seedFile = Join-Path $control "$name.ootrr"
        $stage = Join-Path $control '_seed-build'
        $ok = $false
        if ((Test-Path -LiteralPath $ootrr) -and $romfs -and (Test-Path -LiteralPath $seedFile)) {
          try {
            if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
            $n = 0
            foreach ($t in @(Get-KitOtherSizeTables $mods)) {
                $to = Join-Path $stage "$n\romfs\System\Resource"
                New-Item -ItemType Directory -Force -Path $to | Out-Null
                Copy-Item -LiteralPath $t.FullName -Destination (Join-Path $to $t.Name)
                $n++
            }
            $ErrorActionPreference = 'Continue'
            & $ootrr build $seedFile $romfs $stage $name --host hookshot *> (Join-Path $control 'last-build.txt')
            $code = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            $built = Join-Path $stage $name
            if ($code -eq 0 -and (Test-Path -LiteralPath (Join-Path $built 'coop-seed.txt'))) {
                # The game's own files in the seed, which OOTRR's build does not write: the preset, and the spoiler
                # log and settings string the players share (the same seed file, so the same seed).
                foreach ($keep in @('coop-rando.txt', 'spoiler.txt', 'settings.txt')) {
                    $from = Join-Path $seedDir $keep
                    if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination (Join-Path $built $keep) }
                }
                Remove-Item -LiteralPath $seedDir -Recurse -Force
                Move-Item -LiteralPath $built -Destination $seedDir
                $ok = $true
            }
          } catch {
            $ok = $false   # (a copy or move that failed: removed below, as when it cannot be built)
          } finally {
            $ErrorActionPreference = 'Stop'
            if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
          }
        }
        if ($ok) { $lines += "randomizer: seed $name built again for the new puppet pack" }
        else {
            if (Test-Path -LiteralPath $seedDir) { Remove-Item -LiteralPath $seedDir -Recurse -Force }
            $lines += "randomizer: seed $name removed (could not build it again for the new puppet pack): the game builds it at its next start"
        }
    }
    return $lines
}

# A puppet pack to make (again) before the game starts: none there, or one Get-PuppetPackHint calls old (no sound,
# no tunics, another pack tool). Without it the other players cannot be shown (a player report: "in game but we
# can't see each other"), so the launchers make it themselves, for the emulator and Hookshot alike.
function Test-KitPuppetPackStale([string]$Manifest, [string]$ToolDir) {
    if (-not (Test-Path -LiteralPath $Manifest)) { return $true }
    return [bool](Get-PuppetPackHint $Manifest $ToolDir)
}

# The line said before Build-Hookshot-Coop runs for that need ('game', 'moved' and 'pin' ask first:
# Get-KitHookshotNeedQuestion). A tester read the old "No co-op Hookshot game" as "couldn't find Hookshot"
# and stopped: these say what is missing and that the kit sets it up.
# $HsOnly: a Hookshot-only kit ("Start Squadala.bat"), which has no emulator and no Build-Hookshot-Coop.bat.
function Get-KitHookshotNeedText([string]$Need, [string]$Game = '', [bool]$HsOnly = $false) {
    # (kit-friction: players who had built Longshot asked whether it could be used, or moved it into the kit; one asked
    # where 60 GB goes. Sizes measured on a built kit, 2026-11-14)
    if ($HsOnly -and $Need -eq 'game') {
        return ('The game isn''t set up yet. The kit builds the co-op game on this PC from your own game file, once (about 20 to 40 minutes; the PC stays usable).' + "`n" +
            'It is Longshot with co-op built into its source, so a Longshot you built yourself is never used as it is, and moving one into this folder does nothing (the setup can copy your saves from it).' + "`n" +
            'Disk: about 60 GB free while it builds; about 37 GB stay (the game 35 GB, 24 GB of it your game''s unpacked files; 2 GB of build files that keep updates quick).')
    }
    if ($HsOnly -and $Need -eq 'moved') { return "The co-op game this kit built is not in $Game any more (moved or deleted). Moved it? Answer n, move it back there, then start again. Otherwise it is built again (about 20 to 40 minutes)." }
    switch ($Need) {
        # (the tester behind this was an emulator player joining a Hookshot host with Join-Hookshot.bat)
        'game' { 'Playing on the emulator? Use Join.bat (or Host.bat): emulator and Hookshot players can be in the same game.' + "`n" + 'Hookshot isn''t set up yet. The kit builds the co-op Hookshot game on this PC from your own game, once (about 20 to 40 minutes; the PC stays usable).' }
        'moved' { "The co-op Hookshot game this kit built is not in $Game any more (moved or deleted). Moved it? Answer n, then run Build-Hookshot-Coop.bat -Out ""<its new folder>"" (seconds). Otherwise it is built again (about 20 to 40 minutes)." }
        'pin' { 'Your co-op game was built for another game version than this kit uses: it is built again (about 15 minutes; the PC stays usable).' }
        'patches' { 'This update changes the co-op game a little: updating it (a few minutes).' }
        'module' { 'Putting the updated co-op and the puppet pack into the co-op game (seconds to a minute).' }
        'quarantine' { Get-KitHookshotQuarantineText }
        default { '' }
    }
}

# The question before a long build ('' = none: short steps just run, and 'quarantine' cannot be built away).
function Get-KitHookshotNeedQuestion([string]$Need) {
    switch ($Need) {
        'game' { 'Set it up now?' }
        'moved' { 'Build it again now?' }
        'pin' { 'Update it now?' }
        default { '' }
    }
}

# The co-op module gone with the game there (or the kit's copy gone too): almost always antivirus, which
# takes a new unsigned DLL away as a false alarm. Rebuilding cannot fix it; restoring it does.
function Get-KitHookshotQuarantineText {
    @(
        'The co-op module hookshot_coop.dll is missing, so co-op cannot start.',
        'Antivirus most likely took it away as a false alarm. To get it back (SETUP.md, "Antivirus took hookshot_coop.dll"):',
        '  1. Windows Security > Virus & threat protection > Protection history: the hookshot_coop.dll entry > Actions > Restore.',
        '  2. Add an exclusion so it stays: Virus & threat protection settings > Exclusions > Add or remove exclusions >',
        '     Add an exclusion > Folder: this kit''s folder (and the co-op game''s folder if it is elsewhere).',
        '  Another antivirus: restore it from its quarantine and allow the kit folder the same way, or unzip the kit again.',
        'Then start this again.'
    ) -join "`n"
}

# The UDP port check-hosting.cmd checks: the one this kit's server listens on when it is running (Host.bat or
# Host-Hookshot.bat), else the port of the coop.ini written last (the emulator's or the Hookshot game's: Host-Hookshot.bat
# writes only the latter), else $Default.
function Get-KitHostPort([string]$Kit, [int[]]$Listening, [int]$Default) {
    $inis = @((Join-Path $Kit 'emulator\user\config\coop.ini'))
    $exe = Get-KitHookshotExe (Get-KitHookshotGame $Kit)
    if ($exe) { $inis += (Join-Path (Split-Path -Parent $exe) 'coop.ini') }
    $known = @()
    foreach ($i in $inis) {
        if (-not (Test-Path -LiteralPath $i)) { continue }
        $p = (Read-CoopIni $i).port
        if ($p -match '^\d{1,5}$') { $known += [pscustomobject]@{ Port = [int]$p; Time = (Get-Item -LiteralPath $i).LastWriteTimeUtc } }
    }
    foreach ($k in $known) { if ($Listening -contains $k.Port) { return $k.Port } }
    if (@($Listening).Count -eq 1) { return [int]@($Listening)[0] }
    $newest = $known | Sort-Object Time -Descending | Select-Object -First 1
    if ($newest) { return $newest.Port }
    return $Default
}

# How to make the puppet pack again. A Hookshot-only kit has no make-puppet-pack.cmd: its launcher's build step
# makes the pack (play.ps1 sets this there).
$script:KitPackRemake = 'Run make-puppet-pack.cmd again.'

# The files a puppet pack's manifest ($Json, coop_puppet.json read) names that are not there: look_files (beside the
# manifest: coop_look\<colour>.bin, coop_look\gear.bin) and each tunic's files (in its romfs\). Relative paths, the
# manifest's own spelling; @() when all are there. Never throws.
function Get-KitPuppetPackGaps([string]$Manifest, $Json) {
    $out = @()
    try {
        $dir = Split-Path -Parent $Manifest
        $romfs = Join-Path $dir 'romfs'
        if ($Json.look_files) {
            foreach ($p in $Json.look_files.PSObject.Properties) {
                $rel = "$($p.Value)"
                if ($rel -and -not (Test-Path -LiteralPath (Join-Path $dir $rel) -PathType Leaf)) { $out += $rel }
            }
        }
        if ($Json.tunics) {
            foreach ($p in $Json.tunics.PSObject.Properties) {
                foreach ($rel in @($p.Value)) {
                    if ($rel -and -not (Test-Path -LiteralPath (Join-Path $romfs "$rel") -PathType Leaf)) { $out += "romfs/$rel" }
                }
            }
        }
    } catch { }
    return $out
}

function Get-PuppetPackHint([string]$Manifest, [string]$ToolDir) {
    try {
        $json = [System.IO.File]::ReadAllText($Manifest) | ConvertFrom-Json -ErrorAction Stop
        if ($json.sound -ne $true) {
            return "Puppet pack: made without the other players' sounds. $script:KitPackRemake to hear their footsteps, sword and voice."
        }
        # No tunic models (a pack from before the colours, or a game without the
        # tunic textures): every other player shows in the green tunic (#202).
        $tunics = $json.tunics
        if ($json.tunics_off -ne $true -and (-not $tunics -or @($tunics.PSObject.Properties).Count -eq 0)) {
            return "Puppet pack: made without the tunic colours, so the other players show in green. $script:KitPackRemake"
        }
        # Files the manifest names but are gone (a pack tool stopped half way, antivirus, a hand-copied pack): the
        # look files (coop_look\gear.bin, a colour's .bin) or a tunic's textures. The puppets need them (pack-retry).
        $gaps = @(Get-KitPuppetPackGaps $Manifest $json)
        if ($gaps) {
            return "Puppet pack: incomplete ($($gaps[0]) is missing$(if ($gaps.Count -gt 1) { " and $($gaps.Count - 1) more" })). $script:KitPackRemake"
        }
        $tool = Get-PuppetToolHash $ToolDir
        if ($tool -and $json.tool_sha256 -and $json.tool_sha256 -ne $tool) {
            return "Puppet pack: made by another version of the pack tool. $script:KitPackRemake (for the latest puppet fixes)"
        }
        # (a pack from before the tool hash: made by an older tool, so without the newer fixes, e.g. 0.11.1's colours)
        if ($tool -and -not $json.tool_sha256) {
            return "Puppet pack: made by an older version of the pack tool. $script:KitPackRemake (for the latest puppet fixes)"
        }
    } catch {
        return "Puppet pack: coop_puppet.json could not be read. $script:KitPackRemake"
    }
}

# --- A Longshot the player built themselves (kit-friction) ---------------------------------------
# The kit never plays a Longshot the player built: co-op needs its own build of the Longshot release in hookshot\PIN with
# co-op's patches in its source. But such a game folder gives two things: its saves (copied into the co-op game), and,
# through its builder's home, Longshot's own downloads (the same tool zips, by SHA-256) and, when it is the same release,
# its compiled objects (Longshot keys each by its source, flags and headers, never by folder: a copy can only hit or miss).
# Nothing is ever written into the player's folders: everything is copied out.

# The folder the game's image.bin is in (its saves are in user\ beside it): <game>\bin (a builder's game folder, its
# launcher longshot.exe on top) or <game>. '' when $Folder is no Longshot game.
function Get-KitLongshotImageDir([string]$Folder) {
    if (-not $Folder) { return '' }
    foreach ($d in (Join-Path $Folder 'bin'), $Folder) {
        if (Test-Path -LiteralPath (Join-Path $d 'image.bin')) { return $d }
    }
    return ''
}

# What longshot_version.txt in a game folder says: @{ version = 'v3.0.0-rc.3' (or a nightly's name); home = the builder's
# home ('' when not named) }. $null when there is none.
function Get-KitLongshotGameInfo([string]$Folder) {
    foreach ($f in (Join-Path $Folder 'longshot_version.txt'), (Join-Path $Folder 'bin\longshot_version.txt')) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $info = @{ version = ''; home = '' }
        foreach ($line in Get-Content -LiteralPath $f) {
            if ($line -match '^\s*version\s*=\s*(.+?)\s*$') { $info.version = $Matches[1] }
            # (builder=<home>\versions\<tag>\LongshotBuilder\LongshotBuilder.exe: the home is above versions\)
            elseif ($line -match '^\s*builder\s*=\s*(.+?)\\versions\\[^\\]+\\') { $info.home = $Matches[1] }
        }
        return $info
    }
    return $null
}

# Longshot Builder homes on this PC: the one the game folder names, LONGSHOT_SETUP_BASE, %LOCALAPPDATA%\LongshotBuilder.
# Each @{ path; version (versions\current.txt, '' when unknown) }, only the ones that have downloads\ or objcache\.
function Get-KitLongshotHomes([string]$Named = '', [string]$LocalAppData = $env:LOCALAPPDATA) {
    $seen = @()
    $homes = @()
    foreach ($h in @($Named, $env:LONGSHOT_SETUP_BASE, $(if ($LocalAppData) { Join-Path $LocalAppData 'LongshotBuilder' }))) {
        if (-not $h) { continue }
        $full = [System.IO.Path]::GetFullPath($h).TrimEnd('\')
        if ($seen -contains $full.ToLowerInvariant()) { continue }
        $seen += $full.ToLowerInvariant()
        if (-not ((Test-Path -LiteralPath (Join-Path $full 'downloads')) -or (Test-Path -LiteralPath (Join-Path $full 'objcache')))) { continue }
        $cur = Join-Path $full 'versions\current.txt'
        $v = if (Test-Path -LiteralPath $cur) { "$(Get-Content -LiteralPath $cur -TotalCount 1)".Trim() } else { '' }
        $homes += @{ path = $full; version = $v }
    }
    $homes
}

# A tool zip Longshot's setup already downloaded (downloads\<sha256>.zip in a home), copied to $To when $To is not there:
# its SHA-256 is its name, and Get-Download checks it again. Returns the home it came from, or ''.
function Copy-KitLongshotDownload([object[]]$Homes, [string]$Sha256, [string]$To) {
    if (Test-Path -LiteralPath $To) { return '' }
    foreach ($h in $Homes) {
        $from = Join-Path $h.path "downloads\$($Sha256.ToLowerInvariant()).zip"
        if (-not (Test-Path -LiteralPath $from)) { continue }
        try {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
            Copy-Item -LiteralPath $from -Destination "$To.part" -Force
            Move-Item -LiteralPath "$To.part" -Destination $To -Force
            return $h.path
        } catch { Remove-Item -LiteralPath "$To.part" -Force -ErrorAction SilentlyContinue }
    }
    return ''
}

# The saves of a Longshot game (user\save\<account>\slot_NN, account.sav, option.sav beside its image.bin) copied into
# the co-op game's. A slot or file the co-op game has already is never overwritten (said, and left). The co-op guest
# folders (<account>-coop) are not copied. Returns the lines to say.
# (The account folder's name is the same in every Longshot: its one user's id is a constant, host/nn_applet.c user_uid,
# 5a3c9e1f0b7d2468 0000000000000001, which no co-op patch touches; the test kits' games use that folder. Still on the
# in-game check list: the copied slots show on the title.)
function Copy-KitLongshotSaves([string]$FromImageDir, [string]$ToImageDir) {
    $from = Join-Path $FromImageDir 'user\save'
    if (-not (Test-Path -LiteralPath $from)) { return @("No saves in $FromImageDir\user\save: nothing to copy.") }
    $copied = @(); $kept = @()
    foreach ($acct in @(Get-ChildItem -LiteralPath $from -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*-coop' })) {
        $to = Join-Path $ToImageDir "user\save\$($acct.Name)"
        foreach ($e in @(Get-ChildItem -LiteralPath $acct.FullName -Force -ErrorAction SilentlyContinue)) {
            if (-not ($e.Name -like 'slot_*' -or $e.Name -in @('account.sav', 'option.sav'))) { continue }
            $dest = Join-Path $to $e.Name
            if (Test-Path -LiteralPath $dest) { $kept += $e.Name; continue }
            New-Item -ItemType Directory -Force -Path $to | Out-Null
            Copy-Item -LiteralPath $e.FullName -Destination $dest -Recurse -Force
            if ($e.Name -like 'slot_*') { $copied += $e.Name }
        }
    }
    $say = @()
    if ($copied) { $say += "Copied your Longshot save slot(s) $($copied -join ', ') into the co-op game (your Longshot's own saves are not changed)." }
    else { $say += 'No save slots copied.' }
    if ($kept) { $say += "  Already in the co-op game, so left as they are: $($kept -join ', ')." }
    $say
}

# --- Downloads with retries, curl.exe and the co-op mirror (kit-mirror, 0.11.1) ------------------------------------
# Players saw "The underlying connection was closed: An unexpected error occurred on a send." from git.magicbean.dev
# (intermittent, some regions; most often an antivirus that scans HTTPS, a VPN or a proxy). Each download now gets
# several tries, then Windows' own curl.exe, then the co-op mirror (a GitHub release with the same pinned files), and
# every file is checked against the SHA-256 the kit pins: a file that differs is deleted, never used.

# The TLS versions this .NET knows besides what Windows enables (Tls13 only where the enum has it: .NET 4.8 on new Windows).
function Set-KitTls {
    $p = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    if ([Enum]::GetNames([Net.SecurityProtocolType]) -contains 'Tls13') { $p = $p -bor [Net.SecurityProtocolType]'Tls13' }
    [Net.ServicePointManager]::SecurityProtocol = $p
}

# One try of one URL into $Out: 'web' = Invoke-WebRequest, 'curl' = Windows' curl.exe (Windows 10 1803 and newer; its
# own connection code, and --ssl-no-revoke gets past the revocation check an HTTPS-scanning antivirus breaks; the
# SHA-256 check still decides). Throws with the reason. OOT_COOP_TEST_BLOCK_HOST (tests only): hosts matching that
# regex fail as if blocked. (curl -g: a URL's [ ] { } are never globbed. Invoke-WebRequest -OutFile reads [ ] in a
# path as wildcards in PowerShell 5.1, so a kit folder with them gets the file under a plain name in TEMP first.)
function Invoke-KitFetch([string]$Url, [string]$Out, [string]$Method) {
    $h = Get-KitUrlHost $Url
    if ($env:OOT_COOP_TEST_BLOCK_HOST -and $h -match $env:OOT_COOP_TEST_BLOCK_HOST) { throw "$h is blocked (test)" }
    if ($Method -eq 'curl') {
        $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
        if (-not (Test-Path -LiteralPath $curl)) { throw 'this Windows has no curl.exe' }
        $ErrorActionPreference = 'Continue'   # (curl's error line on stderr is the reason, not a stop)
        $err = & $curl -fsSLg --ssl-no-revoke --retry 2 --connect-timeout 30 -o $Out $Url 2>&1
        if ($LASTEXITCODE -ne 0) { throw "curl.exe: $(("$err" -replace '\s+', ' ').Trim()) (exit $LASTEXITCODE)" }
    } else {
        if ($Out -notmatch '[\[\]]') { Invoke-WebRequest -Uri $Url -OutFile $Out -UseBasicParsing; return }
        $plain = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-dl-' + [Guid]::NewGuid().ToString('N') + '.part')
        try {
            Invoke-WebRequest -Uri $Url -OutFile $plain -UseBasicParsing
            Move-Item -LiteralPath $plain -Destination $Out -Force
        } finally { Remove-Item -LiteralPath $plain -Force -ErrorAction SilentlyContinue }
    }
}

function Get-KitUrlHost([string]$Url) { if ($Url -match '^[a-z]+://([^/:]*)') { if ($Matches[1]) { $Matches[1] } else { 'this PC' } } else { $Url } }

# An answer that will not change on a retry: HTTP 404 or 410 (Invoke-WebRequest), or curl.exe's "error: 404"/"410".
function Test-KitGoneError([Exception]$E) {
    $r = $E.Response
    if ($r -and $r.PSObject.Properties['StatusCode']) { return [int]$r.StatusCode -in 404, 410 }
    return $E.Message -match '^curl\.exe: .*error: (404|410)\b'
}

# Hosts that failed every try once in this run: later downloads give them one try (no wait), not the whole round.
# (Not for the mirror: it is the last place to try, and shares github.com with tools.lock's upstream URLs.)
$script:KitBadHosts = @{}

# Get-KitDownload URLS TO SHA256 [MIRRORS]: $To, checked against $Sha256 (required: nothing unverified is kept). Each
# URL, then each mirror URL, gets $Tries tries with Invoke-WebRequest ($Wait, then twice as long, between them) and
# then one with curl.exe. A file with another SHA-256 is deleted and the next URL tried. Returns '' when $To is there
# and right, else what failed, host by host ("git.magicbean.dev: <reason>; github.com: <reason>").
function Get-KitDownload([string[]]$Urls, [string]$To, [string]$Sha256, [string[]]$Mirrors = @(), [int]$Tries = 4, [double]$Wait = 2) {
    if ($Sha256 -notmatch '^[0-9a-fA-F]{64}$') { return "no SHA-256 to check $(Split-Path -Leaf $To) against" }
    $Sha256 = $Sha256.ToLowerInvariant()
    if ((Test-Path -LiteralPath $To) -and (Get-FileHash -LiteralPath $To -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Sha256) { return '' }
    Write-Host "  downloading $(Split-Path -Leaf $To) ..."
    $part = "$To.part"
    $failed = @()
    $mirrorSet = @($Mirrors | Where-Object { $_ })
    foreach ($url in @(@($Urls) + $mirrorSet | Where-Object { $_ })) {
        $h = Get-KitUrlHost $url
        if ($mirrorSet -contains $url) { Write-Host "  trying the co-op mirror ($h) ..." }
        $bad = $script:KitBadHosts[$h] -and $mirrorSet -notcontains $url
        $n = if ($bad) { 1 } else { [Math]::Max(1, $Tries) }
        $methods = @(@('web') * $n)
        if (-not $bad) { $methods += 'curl' }
        $got = $false; $gone = $false; $why = ''; $delay = $Wait
        for ($i = 0; $i -lt $methods.Count -and -not $got; $i++) {
            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
            try { Invoke-KitFetch $url $part $methods[$i]; $got = $true }
            catch {
                $why = $_.Exception.Message
                # (the server answered "not there": no use asking it again, and the host itself works)
                if (Test-KitGoneError $_.Exception) { $gone = $true; break }
                if ($i + 1 -lt $methods.Count) {
                    $next = if ($methods[$i + 1] -eq 'curl') { "trying with curl.exe" } else { "try $($i + 2) of $n in $delay s" }
                    Write-Host "  $h failed: $why ($next)" -ForegroundColor Yellow
                    if ($methods[$i + 1] -eq 'web' -and $delay -gt 0) { Start-Sleep -Milliseconds ([int]($delay * 1000)); $delay *= 2 }
                }
            }
        }
        if (-not $got) {
            if (-not $gone) { $script:KitBadHosts[$h] = $true }
            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
            $failed += "${h}: $why"; continue
        }
        if ((Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Sha256) {
            Remove-Item -LiteralPath $part -Force
            $failed += "${h}: the file it gave is not the one this kit pins (SHA-256 differs)"; continue
        }
        Move-Item -LiteralPath $part -Destination $To -Force
        if ($failed) { Write-Host "  got it from $h" -ForegroundColor Green }
        return ''
    }
    return ($failed -join '; ')
}

# What a player reads when a download failed everywhere: which hosts failed and why, that the mirror was tried, the
# usual causes on the PC's side, and a URL to try in a browser.
function Get-KitDownloadFailText([string]$File, [string]$Why, [string]$TestUrl, [bool]$HadMirror) {
    $tried = if ($HadMirror) { "Longshot's servers and the co-op mirror on GitHub" } else { "Longshot's servers (this kit has no mirror for it)" }
    @(
        "The download of $File failed. The kit tried $tried, each several times and with curl.exe:"
        "  $Why"
        'This is nearly always something on this PC or network: an antivirus that scans HTTPS/web traffic (Avast, AVG,'
        'Kaspersky, ESET, Bitdefender, Norton: turn its web shield or HTTPS scanning off for the build), a VPN or proxy,'
        'or a firewall. Test: open this in your browser:'
        "  $TestUrl"
        'If the browser downloads it but this script cannot, the antivirus or a proxy blocks PowerShell. Start again: what was'
        'downloaded is kept. Still stuck: scripts\collect-logs.cmd, and attach its zip to /bugreport on the Discord.'
    ) -join "`n"
}

# The kit's puppet pack command, one place for every caller (hookshot-build.ps1's pack step, make-puppet-pack.cmd, the
# test kits' jobs): python's arguments for tools\puppet_pack\build.py from the game file $From into the pack folder $Mod.
# --title-rows / --pause-tab: the title's Host, Join and Co-op Settings rows and the pause menu's Multiplayer tab. They
# make the pack replace three files the game reads at boot (each language's Mals archive, the Title and Common layout
# archives), larger than the game's, with their sizes in the pack's size table; merge_tables.py (every start) takes
# them out of play when the size table the game reads cannot hold them (another mod's table wins), as that crashed the
# game at boot in 0.11.5's test (the old pack left in mods\contents as coop-puppet.old-b26; its table won).
function Get-KitPackArgs([string]$Tool, [string]$From, [string]$Mod) {
    return @('-I', '-B', $Tool, '--romfs', $From, '--mod-dir', $Mod, '--title-rows', '--pause-tab')
}

# #48: Longshot serves ONE resource size table, the winning mod's. A seed made on OOTRR's website (its folder put into
# mods by hand) has its own, made without the puppet pack's entries, so the others could not be shown. Before the game
# starts, tools\puppet_pack\merge_tables.py writes ONE table where the game reads it: that mod's entries and every
# other mod's that is on (the pack's too), keeping its own in <mod>\_coop-size-table (undo: merge_tables.py --undo).
# Last it writes <mod>\coop-merged.txt, which the module checks (MergedByKit) to keep the puppets on. Never stops the start. The lines it says, for the window.
function Invoke-KitSizeTableMerge([string]$Kit, [string]$ExeDir) {
    $python = Join-Path $Kit 'hookshot\tools\python\python.exe'
    $tool = Join-Path $Kit 'tools\puppet_pack\merge_tables.py'
    $mods = Join-Path $ExeDir 'mods'
    if (-not (Test-Path -LiteralPath $tool) -or -not (Test-Path -LiteralPath $mods)) { return @() }
    if (-not (Test-Path -LiteralPath $python)) { return @("Size tables: not merged (no Python in $(Split-Path -Parent $python)): with a mod that has its own size table, the other players can't be shown.") }
    $romfs = @((Join-Path $ExeDir 'romfs'), (Join-Path (Split-Path -Parent $ExeDir) 'romfs')) | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Pack') } | Select-Object -First 1
    $pyArgs = @('-I', '-B', $tool, '--mods', $mods)
    if ($romfs) { $pyArgs += @('--romfs', $romfs) }
    $before = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $out = @(& $python @pyArgs 2>&1 | ForEach-Object { "$_" }) } catch { $out = @("Size tables: not merged ($($_.Exception.Message)).") }
    $ErrorActionPreference = $before
    return $out
}

# The Discord status (docs/setup.md, "Discord"): Squadala!'s Discord application and this kit's release in the game's
# longshot_options.ini (discord_app_id=, discord_details=, discord_state=), scripts\discord_status.py. A line the player
# changed stays theirs; the Insert menu's "Discord status" row turns it off. Never stops the start. The lines it says.
function Invoke-KitDiscordStatus([string]$Kit, [string]$ExeDir) {
    $python = Join-Path $Kit 'hookshot\tools\python\python.exe'
    $tool = Join-Path $Kit 'scripts\discord_status.py'
    if (-not (Test-Path -LiteralPath $tool) -or -not (Test-Path -LiteralPath $python)) { return @() }
    $before = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = @(& $python -I -B $tool --options (Join-Path $ExeDir 'longshot_options.ini') --version-file (Join-Path $Kit 'VERSION.txt') 2>&1 | ForEach-Object { "$_" })
    } catch { $out = @("Discord status: not set ($($_.Exception.Message)).") }
    $ErrorActionPreference = $before
    return $out
}
