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
        if (-not $t -or $t.StartsWith('#') -or $t.StartsWith(';') -or $t -match '^(auto_update|verify_signature|server_allow|server_bind|server_password|public_address_lookup|upnp|invite|stun_servers|relay|relay_key|pvp)\s*=') { continue }
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
# no), Stun (stun_servers), Relay (relay = address[:port]) and RelayKey (relay_key) are the
# router forward, the invite code and the relay (docs/play-online.md). Pvp: $true
# only for 'pvp = on' (or yes, 1, true); anything else, or no line, is off.
function Read-KitServerAccess([string]$Path) {
    $access = [ordered]@{ Allow = ''; Bind = ''; Password = $null; Upnp = $true; Invite = $true; Stun = ''; Relay = ''; RelayKey = ''; Pvp = $false }
    if (-not (Test-Path -LiteralPath $Path)) { return $access }
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match '^\s*server_allow\s*=\s*(.*?)\s*$') { $access.Allow = $Matches[1] }
        elseif ($line -match '^\s*server_bind\s*=\s*(.*?)\s*$') { $access.Bind = $Matches[1] }
        elseif ($line -match '^\s*server_password\s*=\s*(.*?)\s*$') { if ($Matches[1]) { $access.Password = $Matches[1] } }
        elseif ($line -match '^\s*upnp\s*=\s*(.*?)\s*$') { $access.Upnp = $Matches[1] -notmatch '^(no|off|false|0)$' }
        elseif ($line -match '^\s*invite\s*=\s*(.*?)\s*$') { $access.Invite = $Matches[1] -notmatch '^(no|off|false|0)$' }
        elseif ($line -match '^\s*stun_servers\s*=\s*(.*?)\s*$') { $access.Stun = $Matches[1] }
        elseif ($line -match '^\s*relay\s*=\s*(.*?)\s*$') { $access.Relay = $Matches[1] }
        elseif ($line -match '^\s*relay_key\s*=\s*(.*?)\s*$') { $access.RelayKey = $Matches[1] }
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

# Puts your colour's tunic textures over the game's in the pack's romfs, so your own Link wears
# your colour: the pack tool's coop_self/ files, listed in coop_puppet.json as self_files
# ({colour: {romfs path: file}}: the tile, the child's dark tile, the adult's other tunic
# textures and the adult's model copy that names the colour's textures, #202); a pack from before that: self_tiles / self_dark_tiles. Green, no colour or a
# pack without them: takes every copy away, the game's own green. Only the Kokiri tunic: the
# Goron and Zora tunics have their own textures. Returns the colour put in, or ''.
function Set-KitSelfTunic([string]$PackDir, [string]$Color) {
    $romfs = Join-Path $PackDir 'romfs'
    $tile = 'Tex/Angin_Tile_A_Alb.bntx.zs'
    $dark = 'Tex/Angin_Tile_A_Dark_Alb.bntx.zs'
    $tunic = Get-KitTunic $Color
    $json = $null
    try {
        $json = [System.IO.File]::ReadAllText((Join-Path $PackDir 'coop_puppet.json')) | ConvertFrom-Json -ErrorAction Stop
    } catch { }
    $ok = { param($rel) $rel -is [string] -and ($rel -match '^Tex/[A-Za-z0-9_]+\.bntx\.zs$' -or
            $rel -match '^Model/Player_(Adult|Child)\.Player_(Adult|Child)\.bfres\.zs$') }
    # Every file a colour may put in, taken away first: no copy stays after a change of colour.
    # The fixed list also covers a pack without self_files (made again older): no stale copy.
    $targets = @($tile, $dark, 'Tex/Mt_Vest_Player_Adult_Alb.bntx.zs', 'Tex/Mt_Angin_Acs_Player_Adult_Alb.bntx.zs',
        'Tex/Mt_Cloth_Parts_Player_Child_Alb.bntx.zs', 'Tex/Mt_Vest_Player_Child_Alb.bntx.zs',
        'Model/Player_Adult.Player_Adult.bfres.zs')
    $put = [ordered]@{}
    try {
        if ($json -and $json.self_files) {
            foreach ($c in $json.self_files.PSObject.Properties) {
                foreach ($f in $c.Value.PSObject.Properties) { if (& $ok $f.Name) { $targets += $f.Name } }
            }
        }
        if ($json -and $tunic -and $tunic -ne 'green' -and $json.tunics -and $json.tunics.green) {
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
    # The tile is the one that must be there; a missing other file is left to the game's.
    if (-not $put.Contains($tile) -or -not (Test-Path -LiteralPath $put[$tile] -PathType Leaf)) { $put = [ordered]@{} }
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
# Returns $true when the pack is there.
function Use-KitPuppetPack([string]$Manifest, [string]$ToolDir, [string[]]$Missing) {
    if (Test-Path -LiteralPath $Manifest) {
        Write-Host 'Puppet pack: found.'
        # Made before sound or before the kit's pack tool changed: say so, start anyway.
        $hint = Get-PuppetPackHint $Manifest $ToolDir
        if ($hint) { Write-Host $hint }
        # Your own Link in your colour (#202); LICHII_COOP_TUNICS=0 keeps everyone green.
        $want = if ($env:LICHII_COOP_TUNICS -match '^(0|off)$') { '' } else { $env:LICHII_COOP_COLOR }
        $self = Set-KitSelfTunic (Split-Path -Parent $Manifest) $want
        if ($self) {
            $env:LICHII_COOP_SELF_TUNIC = $self
            Write-Host "Your Link: the $self tunic (coop-options.ini color; the Goron and Zora tunics stay as they are)."
        }
        elseif ($env:LICHII_COOP_SELF_TUNIC) { Remove-Item env:LICHII_COOP_SELF_TUNIC }
        if ($want -and -not $self -and (Get-KitTunic $want) -ne 'green') {
            Write-Host 'Your Link stays green: the puppet pack has no tunic of your colour. Run make-puppet-pack.cmd again.'
        }
        return $true
    }
    foreach ($line in $Missing) { Write-Host $line }
    if ($env:LICHII_COOP_PUPPET) { Remove-Item env:LICHII_COOP_PUPPET; Write-Host 'Option: LICHII_COOP_PUPPET off (no pack)' }
    if ($env:LICHII_COOP_SELF_TUNIC) { Remove-Item env:LICHII_COOP_SELF_TUNIC }
    return $false
}

# --- Hookshot: what the kit's game needs before it starts (updates without player steps) ---

# The kit's Hookshot game folder: hookshot\game-folder.txt (hookshot-build.ps1 writes it), else hookshot\game.
function Get-KitHookshotGame([string]$Kit) {
    $where = Join-Path $Kit 'hookshot\game-folder.txt'
    if (Test-Path -LiteralPath $where) {
        $g = "$(Get-Content -LiteralPath $where -TotalCount 1)".Trim()
        if ($g) { return $g }
    }
    return (Join-Path $Kit 'hookshot\game')
}

# longshot.exe of a game folder (a builder's game folder has it in bin\), or $null.
function Get-KitHookshotExe([string]$Game) {
    foreach ($p in (Join-Path $Game 'bin\longshot.exe'), (Join-Path $Game 'longshot.exe')) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
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
#   'game'    no game built yet (the first build, 10 to 45 minutes)
#   'pin'     the kit moved to another Hookshot release or pin (a full game build, about 15 minutes)
#   'patches' the co-op patches changed (the game built again, only what changed: a few minutes)
#   'module'  the co-op module changed, or the puppet pack is missing or stale (co-op and the pack put in
#             again: seconds to a minute; without a pack the other players cannot be shown)
#   ''        nothing: start the game
function Get-KitHookshotNeed([string]$Kit) {
    $game = Get-KitHookshotGame $Kit
    $exe = Get-KitHookshotExe $game
    if (-not $exe) { return 'game' }
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
    return ''
}

# A puppet pack to make (again) before the game starts: none there, or one Get-PuppetPackHint calls old (no sound,
# no tunics, another pack tool). Without it the other players cannot be shown (a player report: "in game but we
# can't see each other"), so the launchers make it themselves, for the emulator and Hookshot alike.
function Test-KitPuppetPackStale([string]$Manifest, [string]$ToolDir) {
    if (-not (Test-Path -LiteralPath $Manifest)) { return $true }
    return [bool](Get-PuppetPackHint $Manifest $ToolDir)
}

# The line said before Build-Hookshot-Coop runs by itself for that need.
function Get-KitHookshotNeedText([string]$Need) {
    switch ($Need) {
        'game' { 'The Hookshot game is not built yet: building it now from your game (10 to 45 minutes, once; the PC stays usable).' }
        'pin' { 'This update moves to a new Hookshot: updating the game (about 15 minutes; the PC stays usable).' }
        'patches' { 'This update changes the Hookshot game a little: updating it (a few minutes).' }
        'module' { 'Putting the updated co-op and the puppet pack into the Hookshot game (seconds to a minute).' }
        default { '' }
    }
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

function Get-PuppetPackHint([string]$Manifest, [string]$ToolDir) {
    try {
        $json = [System.IO.File]::ReadAllText($Manifest) | ConvertFrom-Json -ErrorAction Stop
        if ($json.sound -ne $true) {
            return 'Puppet pack: made without the other players'' sounds. Run make-puppet-pack.cmd again to hear their footsteps, sword and voice.'
        }
        # No tunic models (a pack from before the colours, or a game without the
        # tunic textures): every other player shows in the green tunic (#202).
        $tunics = $json.tunics
        if ($json.tunics_off -ne $true -and (-not $tunics -or @($tunics.PSObject.Properties).Count -eq 0)) {
            return 'Puppet pack: made without the tunic colours, so the other players show in green. Run make-puppet-pack.cmd again.'
        }
        $tool = Get-PuppetToolHash $ToolDir
        if ($tool -and $json.tool_sha256 -and $json.tool_sha256 -ne $tool) {
            return 'Puppet pack: made by another version of the pack tool. Run make-puppet-pack.cmd again for the latest puppet fixes.'
        }
    } catch {
        return 'Puppet pack: coop_puppet.json could not be read. Run make-puppet-pack.cmd again.'
    }
}
