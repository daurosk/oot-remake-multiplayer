# Writes emulator\user\config\coop.ini (asking for what is missing) and starts the emulator.
# Usage: play.cmd                  asks the first time, then reuses the answers
#        play.cmd change           asks again
#        play.cmd -Server 192.168.1.20 -Name Saria [-Port 27510]   no questions
#        Host.bat (play.ps1 host)  starts this kit's server too and plays on it (127.0.0.1)
#        Join.bat (play.ps1 join)  asks for the host's address each time
#        Host-Hookshot.bat, Join-Hookshot.bat (play.ps1 host|join -Hookshot)  the same with the
#                                  Hookshot game Build-Hookshot-Coop.bat built (coop.ini beside its hookshot.exe)
# On every start it looks for a newer co-op release and, as auto_update in coop-options.ini
# says, updates the kit (emulator included) and starts the new kit's play.ps1 with -NoUpdate.
# Every later play.ps1 must accept -NoUpdate.
param(
    [string]$Mode = '',
    [string]$Server = '',
    [string]$Name = '',
    [string]$Port = '',
    [switch]$NoUpdate,
    [switch]$Hookshot
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'coop-ini.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
. (Join-Path $PSScriptRoot 'hosting.ps1')
. (Join-Path $PSScriptRoot 'relay-join.ps1')
. (Join-Path $PSScriptRoot 'invite.ps1')
$kit = Split-Path -Parent $PSScriptRoot
$emu = Join-Path $kit 'emulator'
$exe = Join-Path $emu 'lichii.exe'
$ini = Join-Path $emu 'user\config\coop.ini'
$template = Join-Path $PSScriptRoot 'coop.ini.template'
# Play-OoT-Coop.bat (play.ps1 choose -Hookshot): the game starts with no session (mode 'title' below);
# Host and Join are on its title screen. A session started here (a server in coop.ini) would make the
# title's Host and Join answer "Co-op is on already in this game".
if ($Mode -eq 'choose' -and $Hookshot) { $Mode = 'title' }
# play.ps1 choose without -Hookshot: asks host or join, the last answer first.
if ($Mode -eq 'choose') {
    $lastMode = Join-Path $kit 'hookshot\last-mode.txt'
    $was = if (Test-Path -LiteralPath $lastMode) { "$(Get-Content -LiteralPath $lastMode -TotalCount 1)".Trim() } else { '' }
    $default = if ($was -eq 'join') { 'J' } else { 'H' }
    Write-Host 'H = host a game (your friends join you)    J = join a friend''s game'
    $answer = "$(Read-Host "H or J [$default]")".Trim().ToUpperInvariant()
    if (-not $answer) { $answer = $default }
    $Mode = if ($answer.StartsWith('J')) { 'join' } else { 'host' }
    try { New-Item -ItemType Directory -Force -Path (Split-Path -Parent $lastMode) | Out-Null; [System.IO.File]::WriteAllText($lastMode, "$Mode`n") } catch { }
}
if ($Hookshot) {
    # The game Build-Hookshot-Coop.bat built (hookshot-build.ps1 notes where, for -Out); its module
    # reads coop.ini beside hookshot.exe. The emulator's own steps below are skipped for it. A game not
    # built yet, or one an update left behind, is brought up to date after the update check below.
    $hsGame = Get-KitHookshotGame $kit
    $exe = Get-KitHookshotExe $hsGame
    if ($exe) { $emu = Split-Path -Parent $exe; $ini = Join-Path $emu 'coop.ini' }
}
if (-not $Hookshot -and -not (Test-Path $exe)) { Write-Host "Not found: $exe"; exit 1 }

# Yes or no, yes when no key comes in $Seconds (an unattended start still updates).
function Read-YesNo([string]$Question, [int]$Seconds) {
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected } catch { }
    if ($redirected) {
        Write-Host "$Question [Y/n]"
        $line = [Console]::In.ReadLine()
        return -not ($line -and $line.Trim() -match '^(?i)(n|no)$')
    }
    Write-Host -NoNewline "$Question [Y/n] (yes in $Seconds s) "
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq 'N') { Write-Host 'n'; return $false }
            if ($key.Key -eq 'Y' -or $key.Key -eq 'Enter') { Write-Host 'y'; return $true }
        }
        Start-Sleep -Milliseconds 100
    }
    Write-Host 'y'
    return $true
}

# Waits for a key, $Seconds at most, so a message is read before the game starts.
function Wait-AnyKey([int]$Seconds) {
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected } catch { }
    if ($redirected) { return }
    Write-Host -NoNewline "Press a key to go on (goes on in $Seconds s) "
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        if ([Console]::KeyAvailable) { [Console]::ReadKey($true) | Out-Null; break }
        Start-Sleep -Milliseconds 100
    }
    Write-Host ''
}

# A newer co-op release? One short question to GitHub (update.ps1 -Status), given 15 s at
# most: a slow name lookup or proxy search is not bounded by the request's own timeout, so it
# is stopped when it takes longer. No answer, or an update that fails, is said in one line
# with what to do, and this kit starts as it is; a failed update waits for a key first, so
# its message is read before the game window covers it.
$updater = Join-Path $PSScriptRoot 'update.ps1'
if (-not $NoUpdate -and (Test-Path $updater)) {
    $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $status = ''
    $out = [System.IO.Path]::GetTempFileName()
    try {
        $check = Start-Process -FilePath $ps -NoNewWindow -PassThru -RedirectStandardOutput $out -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$updater`"", '-Status', '-Kit', "`"$kit`"")
        if ($check.WaitForExit(15000)) { $status = ([System.IO.File]::ReadAllText($out)).Trim() } else {
            $check.Kill()
            $status = 'failed GitHub did not answer in 15 s'
        }
    } catch { $status = "failed $($_.Exception.Message)" }
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    if ($status -match '^failed (.*)') {
        Write-Host "Could not check for a new co-op version: $($Matches[1])."
        Write-Host 'Every player needs the same version. To check again, run Update.bat; releases: https://github.com/daurosk/oot-remake-multiplayer/releases'
    }
    if ($status -match '^(newer (\S+)|unfinished)$') {
        $what = if ($Matches[2]) { "Co-op $($Matches[2]) is out." } else { 'An update of this kit did not finish.' }
        $when = 'ask'
        foreach ($line in @(Get-Content -LiteralPath (Join-Path $kit 'coop-options.ini') -ErrorAction SilentlyContinue)) {
            if ($line -match '^\s*auto_update\s*=\s*(ask|yes|no)\s*$') { $when = $Matches[1].ToLowerInvariant() }
        }
        $go = switch ($when) {
            'yes' { Write-Host "$what Updating (auto_update = yes in coop-options.ini)."; $true }
            'no' { Write-Host "$what Close the games and run Update.bat to update (auto_update = no)."; $false }
            default { Read-YesNo "$what Update now? Your saves, keys and settings are kept." 10 }
        }
        if ($go) {
            # The update replaces this script, so it runs from a copy, and the new kit's own
            # play.ps1 starts the game afterwards.
            $copy = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-update-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.ps1')
            Copy-Item -LiteralPath $updater -Destination $copy
            & $ps -NoProfile -ExecutionPolicy Bypass -File $copy -Kit $kit -Launch
            $code = $LASTEXITCODE
            Remove-Item -LiteralPath $copy -Force -ErrorAction SilentlyContinue
            if ($code -eq 3) {
                $next = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'play.ps1'))
                foreach ($pair in @(@('-Mode', $Mode), @('-Server', $Server), @('-Name', $Name), @('-Port', $Port))) {
                    if ($pair[1]) { $next += $pair }
                }
                if ($Hookshot) { $next += '-Hookshot' }
                & $ps @next -NoUpdate
                exit $LASTEXITCODE
            }
            if ($code -ne 0) {
                Write-Host 'The update did not happen (above: why, and how to update by hand); this version starts as it is.'
                Wait-AnyKey 60
            }
        }
    }
}

# Hookshot: the game as this kit wants it, with no step of the player's. Build-Hookshot-Coop runs by
# itself: co-op only (seconds) when the Hookshot pin is the same, the game built again when the update moved it,
# and the first build when there is no game yet (the game files are asked for once and remembered). The long
# builds (no game, a moved game, another pin) ask Y/n first; a module antivirus took is said, not built.
if ($Hookshot) {
    $need = Get-KitHookshotNeed $kit
    if ($need -eq 'quarantine') {
        Write-Host (Get-KitHookshotNeedText $need) -ForegroundColor Yellow
        exit 1
    }
    if ($need) {
        Write-Host (Get-KitHookshotNeedText $need $hsGame)
        # The long builds ask first (yes by itself after a minute, so an unattended start still builds).
        $question = Get-KitHookshotNeedQuestion $need
        if ($question -and -not (Read-YesNo "$question (then the game starts; closing this window cancels)" 60)) {
            Write-Host 'Not now. Build-Hookshot-Coop.bat sets it up (SETUP.md, "Play on Hookshot"); then start this again.'
            exit 1
        }
        $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hookshot-build.ps1')
        if ($LASTEXITCODE -ne 0) {
            Write-Host 'The Hookshot game could not be brought up to date (above: why). Run Build-Hookshot-Coop.bat to try again.'
            Wait-AnyKey 60
            exit 1
        }
        $hsGame = Get-KitHookshotGame $kit
        $exe = Get-KitHookshotExe $hsGame
    }
    if (-not $exe) {
        Write-Host "Hookshot isn't set up yet: no Hookshot game in $hsGame. Run Build-Hookshot-Coop.bat and let it finish (SETUP.md, ""Play on Hookshot"")."
        exit 1
    }
    if (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $exe) 'hookshot_coop.dll'))) {
        # The build put it there a moment ago (or the game was ready): gone again means antivirus.
        Write-Host (Get-KitHookshotQuarantineText) -ForegroundColor Yellow
        exit 1
    }
    $emu = Split-Path -Parent $exe
    $ini = Join-Path $emu 'coop.ini'
}
if ($Mode -eq 'title') {
    if (-not $Hookshot) { Write-Host 'play.ps1 title is for Hookshot (Play-OoT-Coop.bat).'; exit 1 }
    $running = Get-Process hookshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
    if ($running) { Write-Host 'This Hookshot game is already running: Host and Join are on its title screen.'; exit 1 }
    # No session from this start: no server and no join password in coop.ini (the name, the colour and the
    # last invite code stay), and none in the variables an earlier start in this window may have left.
    Clear-CoopIniSession $template $ini
    foreach ($v in 'LICHII_COOP_SERVER', 'LICHII_COOP_PASSWORD', 'OOT_COOP_SESSION_PASSWORD', 'OOT_COOP_JOIN_PASSWORD', 'LICHII_COOP_HOST_SAVE') {
        Remove-Item -Path "env:$v" -ErrorAction SilentlyContinue
    }
    [void](Use-KitPuppetPack (Join-Path $emu 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json') (Join-Path $kit 'tools\puppet_pack') @(
        'Puppet pack: not in the Hookshot game, so the other players show only as markers.',
        '  Run Build-Hookshot-Coop.bat -Game <your .nup> again (SETUP.md, "Play on Hookshot").'))
    [void](Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru)
    Write-Host 'Hookshot started. Host or Join on the title screen (Host, Join and Co-op Settings under the game''s own rows;'
    Write-Host 'if they are not there, Minus opens them). Same house or a VPN: Host, then "Same network or VPN".'
    Write-Host 'Host-Hookshot.bat and Join-Hookshot.bat still host or join from a window like this one instead.'
    exit 0
}

$current = Read-CoopIni $ini
$inviteJoin = $null
$ask = $Mode -eq 'change'
$serverGiven = [bool]$Server
$askServer = $ask -or ($Mode -eq 'join' -and -not $serverGiven)
if ($Mode -eq 'host') { $Server = '127.0.0.1' }
if (-not $Server) { $Server = $current.server }
if ($Mode -eq 'join' -and -not $serverGiven -and $Server -match '^(127\.|localhost)') { $Server = '' }
if (-not $Name) { $Name = $current.name }
if (-not $Port) { $Port = if ($current.port) { $current.port } else { '@PORT@' } }

if ($askServer -or -not $Server) {
    # The last join through a relay is offered as its join code (relay-join.ps1).
    $savedRelay = Read-RelayJoin $kit
    if ($savedRelay -and $Server -eq $savedRelay.Address) { $Server = $savedRelay.Text }
    Write-Host 'Paste the INVITE CODE the host sent you (right-click or Ctrl+V pastes here), or the host''s'
    Write-Host 'address. The host''s server window shows both and copies the code. Enter keeps the one in [ ].'
    do {
        $answer = Read-Host "Invite code or host's address$(if ($Server) { " [$Server]" })"
        $typed = if ($answer -and $answer.Trim()) { $answer } else { $Server }
        # An invite code (invite.ps1): the host's internet address and the join password. A
        # helper window (start-join.cmd) carries the game's traffic; the game talks to it here.
        if (Test-InviteCodeText $typed) {
            $invite = ConvertFrom-InviteCode $typed
            $ok = $invite.Ok
            if ($ok) { $inviteJoin = $invite; $Server = '127.0.0.1'; $Port = '27512' }
            else { Write-Host "Not used: $($invite.Error)." }
            continue
        }
        # A join code with its relay (ABCD-EFGH@relay.example.net:27530): the code goes to the
        # relay first, then the game connects to the relay.
        $relayJoin = ConvertTo-RelayJoin $typed
        if ($relayJoin) {
            $relayAddress = Connect-RelayJoin $relayJoin
            $ok = [bool]$relayAddress
            if ($ok) { $Server = $relayAddress; $Port = "$($relayJoin.Port)"; Save-RelayJoin $kit $relayJoin $relayAddress }
            continue
        }
        # Pasted with spaces, a trailing dot or :port: made plain (hosting.ps1).
        $parsed = ConvertTo-JoinAddress $typed
        $ok = $parsed -and (Test-CoopServer $parsed.Server)
        if ($ok) {
            $Server = $parsed.Server
            if ($parsed.Port) { $Port = $parsed.Port }
            elseif ($answer -and $answer.Trim()) { $Port = '@PORT@' }  # a new address without :port: the usual port
        }
        else {
            Write-Host "'$typed' is not an address. It looks like 26.12.34.56 or 192.168.1.20 (four numbers"
            Write-Host 'with dots), sometimes with :port at the end, or a join code with its relay (ABCD-EFGH@relay.example.net).'
            Write-Host 'Ask the host to copy it from their server window.'
        }
    } until ($ok)
}
elseif ($serverGiven -and (Test-InviteCodeText $Server)) {
    $inviteJoin = ConvertFrom-InviteCode $Server
    if (-not $inviteJoin.Ok) { Write-Host "Not used: $($inviteJoin.Error)."; exit 1 }
    $Server = '127.0.0.1'; $Port = '27512'
}
elseif ($serverGiven -and (ConvertTo-RelayJoin $Server)) {
    $relayJoin = ConvertTo-RelayJoin $Server
    $relayAddress = Connect-RelayJoin $relayJoin
    if (-not $relayAddress) { exit 1 }
    $Server = $relayAddress; $Port = "$($relayJoin.Port)"; Save-RelayJoin $kit $relayJoin $relayAddress
}
elseif ($serverGiven) {
    $parsed = ConvertTo-JoinAddress $Server
    if ($parsed) { $Server = $parsed.Server; if ($parsed.Port -and -not $PSBoundParameters.ContainsKey('Port')) { $Port = $parsed.Port } }
}
if ($ask -or -not $Name) {
    do {
        $answer = Read-Host "Your player name, 1 to 16 characters$(if ($Name) { " [$Name]" })"
        if ($answer) { $Name = $answer.Trim() }
        $ok = Test-CoopName $Name
        if (-not $ok) { Write-Host '1 to 16 characters, and different from the other player.' }
    } until ($ok)
}
if (-not (Test-CoopServer $Server)) { Write-Host "Bad server address: $Server"; exit 1 }
if (-not (Test-CoopName $Name)) { Write-Host "Bad name: $Name"; exit 1 }
if ($Port -notmatch '^\d{1,5}$' -or [int]$Port -lt 1 -or [int]$Port -gt 65535) { Write-Host "Bad port: $Port"; exit 1 }

# The join password (protocol 9; every session has one, and it encrypts the traffic).
# Host.bat: server_password in coop-options.ini, or with none (no line or an empty one) a new
# random one for this session; this game uses the same one. Join.bat and play.cmd change:
# asked each time (the invite code holds it), Enter keeps the saved one.
$optionsFile = Join-Path $kit 'coop-options.ini'
$Password = $current.password
$rejoin = $false
if ($inviteJoin) {
    # The invite code holds the join password: nothing to type.
    $Password = $inviteJoin.Password
}
elseif ($Mode -eq 'host') {
    $Password = (Read-KitServerAccess $optionsFile).Password
    $serverExe = Join-Path $kit 'server\coop_server.exe'
    $kitServers = @()
    try {
        $kitServers = @(Get-CimInstance Win32_Process -Filter "Name = 'coop_server.exe'" -OperationTimeoutSec 5 -ErrorAction Stop |
            Where-Object { $_.ExecutablePath -eq $serverExe } |
            ForEach-Object { [pscustomobject]@{ StartTime = $_.CreationDate; CommandLine = $_.CommandLine } })
    } catch {
        $kitServers = @(Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $serverExe } |
            ForEach-Object { [pscustomobject]@{ StartTime = $_.StartTime; CommandLine = '' } })
    }
    if ($null -eq $Password -and $kitServers.Count -gt 0) {
        # The host closed the game and the server of the earlier Host start still runs, with
        # that session's password: the game joins it again with the password start-server.ps1
        # saved (kit-paths.ps1). The server is left alone, so the friends stay connected.
        $session = Read-KitHostSession $kit $kitServers
        if (-not $session) {
            Write-Host 'A co-op window of this kit (a server or a join helper) is still open, and its join password is not known here.'
            Write-Host "Close that window, then start $(if ($Hookshot) { 'Host-Hookshot.bat' } else { 'Host.bat' }) again (closing a server disconnects the friends on it)."
            exit 1
        }
        $Password = $session.Password
        $Port = "$($session.Port)"
        $rejoin = $true
        Write-Host "Your server is still running: joining that session again as the host (same join password and invite code, UDP port $Port). Your friends stay connected."
    }
    elseif ($null -eq $Password) {
        # No server_password line: a new random password for this session only, put in the
        # invite code by the server (start-server.ps1 gets it in OOT_COOP_SESSION_PASSWORD).
        $Password = New-JoinPassword
        $env:OOT_COOP_SESSION_PASSWORD = $Password
        Write-Host 'Join password: a new random one for this session (it is inside the invite code; nobody types it).'
    }
    elseif (-not (Test-CoopPassword $Password)) {
        Write-Host 'coop-options.ini: server_password has a space or is longer than 128 characters; the server was not started.'
        exit 1
    }
}
elseif ($Mode -eq 'join' -or $ask) {
    Write-Host 'Join password: every session has one; the host sees it in the server window. Enter keeps the one in [ ].'
    $tries = 0
    do {
        $answer = Read-Host "Join password$(if ($Password) { ' [saved]' })"
        if ($answer -and $answer.Trim() -eq '-') { $Password = '' }  # once "none": every session has one now
        elseif ($answer -and $answer.Trim()) { $Password = $answer.Trim() }
        $ok = $Password -and (Test-CoopPassword $Password)
        if (-not $ok) {
            Write-Host 'Type the join password the host sent you (every session has one; no spaces, at most 128 characters).'
            $Password = ''
            # Nothing more to read (input from a file or pipe): stop instead of asking forever.
            if (++$tries -ge 5) { exit 1 }
        }
    } until ($ok)
}
if ($Password -and -not (Test-CoopPassword $Password)) { Write-Host 'coop.ini: the join password has a space or is too long.'; exit 1 }

Write-CoopIni $template $ini $Server $Port $Name $Password
Write-Host "Co-op: server $Server, port $Port, name $Name, join password $(if ($Password) { 'set' } else { 'not set: the server will refuse' })   (play.cmd change to change the name)"

$user = Join-Path $emu 'user'
if ($Hookshot) {
    $running = Get-Process hookshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
    if ($running) { Write-Host 'This Hookshot game is already running; close it first to use new settings.'; exit 1 }
}
elseif (-not (Get-ChildItem (Join-Path $user 'keys') -Filter '*.keys' -ErrorAction SilentlyContinue)) {
    Write-Host "No keys in $user\keys yet. SETUP.md says what goes where."
}
if (-not $Hookshot) {
$running = Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
if ($running) { Write-Host 'This emulator is already running; close it first to use new settings.'; exit 1 }

# A copied kit: point the emulator's saved folders at this copy (kit-paths.ps1).
$fixed = Repair-KitPaths $user
if ($fixed) { Write-Host "This kit was copied: pointed $($fixed -join ', ') at $user" }
# No code or cheat mods with co-op (issue 136): stop before anything starts, and say where they are. After the
# copied-kit fix above, so this kit's own load and SD folders are read.
$mods = @(Get-KitMods $user)
if ($mods.Count -gt 0) {
    Write-Host ''
    Write-Host 'These mods for Ocarina of Time change the game''s code or are cheats:'
    foreach ($m in $mods) { Write-Host "  $m" }
    Write-Host 'Code and cheat mods are not supported in co-op; texture and model mods are fine.'
    Write-Host 'Remove those folders, or turn them off: in the emulator, right-click the game,'
    Write-Host 'Properties, Add-Ons, untick them. Then start again.'
    exit 1
}
}
# Invite code: the helper window that carries the game's traffic to the host (start-join.ps1).
if ($inviteJoin) {
    $helperExe = Join-Path $kit 'server\coop_server.exe'
    $helperUp = Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $helperExe }
    if ($helperUp) {
        Write-Host 'A co-op window of this kit (a join helper or a server) is still open. Close it, then start Join.bat again.'
        exit 1
    }
    $env:OOT_COOP_JOIN_PASSWORD = $inviteJoin.Password
    Write-Host "Invite code accepted. A second window, the join helper, opens now: keep it open while you play."
    Write-Host 'If the host''s router does not let the game in directly, that window shows an ANSWER CODE (copied'
    Write-Host 'to the clipboard): send it to the host, who pastes it into the server window and presses Enter.'
    Start-Process -FilePath (Join-Path $kit 'start-join.cmd') -ArgumentList @('-HostAddress', "$($inviteJoin.Address):$($inviteJoin.Port)", '-GamePort', '27512') -WorkingDirectory $kit | Out-Null
    Start-Sleep -Seconds 1
}
if ($Mode -eq 'host') {
    $serverExe = Join-Path $kit 'server\coop_server.exe'
    $serverUp = Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $serverExe }
    if ($serverUp -and $env:OOT_COOP_SESSION_PASSWORD) {
        # It runs with another session's password: this game could not join it.
        Write-Host 'The server from this kit is still running from an earlier Host.bat. Close its window, then start Host.bat again.'
        exit 1
    }
    if ($rejoin) { }  # said above: the running session is joined again
    elseif ($serverUp) { Write-Host 'The server from this kit is already running.' }
    else {
        Write-Host "Starting the server in its own window (UDP port $Port). Keep that window open while you play."
        $serverArgs = @("$Port")
        $access = Read-KitServerAccess (Join-Path $kit 'coop-options.ini')
        if ($access.Allow -match '^[0-9./, ]+$') { $serverArgs += @('-Allow', ($access.Allow -replace '\s', '')) }
        elseif ($access.Allow) { Write-Host "coop-options.ini: server_allow '$($access.Allow)' is not a list of IPv4 addresses (25.1.2.3,26.0.0.0/8); the server was not started."; exit 1 }
        if ($access.Bind -match '^[0-9.]+$') { $serverArgs += @('-Bind', $access.Bind) }
        elseif ($access.Bind) { Write-Host "coop-options.ini: server_bind '$($access.Bind)' is not an IPv4 address (25.1.2.3); the server was not started."; exit 1 }
        Start-Process -FilePath (Join-Path $kit 'start-server.cmd') -ArgumentList $serverArgs -WorkingDirectory $kit | Out-Null
        Start-Sleep -Seconds 2
    }
}

# Switches from coop-options.ini (the lines without a '#' in front). They win over the AMD rule below.
$options = Read-KitOptions (Join-Path $kit 'coop-options.ini')
foreach ($key in $options.Keys) {
    $value = $options[$key]
    if ($value -and -not [System.IO.Path]::IsPathRooted($value) -and (Test-Path -LiteralPath (Join-Path $kit $value))) {
        $value = [System.IO.Path]::GetFullPath((Join-Path $kit $value))
    }
    Set-Item -Path "env:$key" -Value $value
    $tunicNote = ''
    if ($key -eq 'LICHII_COOP_COLOR' -and (Get-Command Get-KitTunic -ErrorAction SilentlyContinue)) {
        $t = Get-KitTunic $value
        if ($t) { $tunicNote = "   (your tunic: $t, the nearest of the ten tunic colours)" }
    }
    Write-Host "Option: $key=$value$tunicNote"
}
# The host's save (issue 167): a join (an address or an invite code, whose server is 127.0.0.1) plays from the
# co-op save folder the host's save comes into; a host from its own saves. Not from coop.ini's address: an old
# friend's address there says nothing about this start. coop-options.ini may set it itself.
# (not on Hookshot: there the session's role decides, as the title menu may Host or Join after this start)
if (-not $Hookshot -and -not $env:LICHII_COOP_HOST_SAVE) {
    if ($Mode -eq 'join' -or $inviteJoin) { $env:LICHII_COOP_HOST_SAVE = '1' } elseif ($Mode -eq 'host') { $env:LICHII_COOP_HOST_SAVE = '0' }
}
# No colour chosen: the colour of the player number you will most likely get, for your own Link too
# (kit-paths.ps1). Before the pack step, which puts that tunic over the game's.
[void](Set-KitDefaultColor $Mode $Server)
if ($Hookshot) {
    # As the emulator below (kit-paths.ps1 Use-KitPuppetPack): the pack hints, your own tunic (#202), the
    # puppets off without a pack. The pack is in mods\contents (Build-Hookshot-Coop.bat installs it), and
    # Hookshot indexes its mod files once at start: this runs before Start-Process.
    [void](Use-KitPuppetPack (Join-Path $emu 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json') (Join-Path $kit 'tools\puppet_pack') @(
        'Puppet pack: not in the Hookshot game, so the other players show only as markers.',
        '  Run Build-Hookshot-Coop.bat -Game <your .nup> again (SETUP.md, "Play on Hookshot").'))
    $started = Get-Date
    $game = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru
    Write-Host "Hookshot started. Its hookshot_log.txt (beside the game) says 'coop: module loaded', then 'coop: connected'."
    exit 0
}
# AMD cards: shader libraries off unless the player chose (kit-paths.ps1, issue 118).
$cardList = $null
try { $cardList = @(Get-CimInstance Win32_VideoController -OperationTimeoutSec 5 -ErrorAction Stop) } catch { }
$cardQuery = if ($null -ne $cardList) { { $cardList }.GetNewClosure() } else { { throw 'no card list' } }
$gpl = Set-AmdGplDefault $cardQuery
if ($gpl) { Write-Host $gpl }
# Built-in graphics only: point to the 30 FPS button (docs/setup.md, "Low frame rate").
$speed = Get-GpuSpeedHint $cardQuery
if ($speed) { Write-Host $speed }
# No puppet pack, or an old one: made now, before the start, so the other players can be seen (puppet-pack.ps1,
# what make-puppet-pack.cmd runs: it finds the game files in the emulator's game list, else asks once). A pack
# that cannot be made is said, and the game starts anyway (the others show as markers).
$packJson = Join-Path (Get-KitLoadDir $user) '0400486024DEC000\coop-puppet\coop_puppet.json'
if (Test-KitPuppetPackStale $packJson (Join-Path $kit 'tools\puppet_pack')) {
    Write-Host 'The puppet pack (how you see the other players) is missing or old: making it now (about a minute) ...'
    $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'puppet-pack.ps1')
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'The puppet pack could not be made (above: why). The game starts; the other players show only as markers.'
        Wait-AnyKey 30
    }
}
# The pack hints, your own tunic (#202), the puppets off without a pack (kit-paths.ps1, the same for Hookshot above).
[void](Use-KitPuppetPack (Join-Path (Get-KitLoadDir $user) '0400486024DEC000\coop-puppet\coop_puppet.json') (Join-Path $kit 'tools\puppet_pack') @(
    'Puppet pack: not made yet, so the other players show only as markers.',
    '  Make it once with make-puppet-pack.cmd (SETUP.md, "The puppet pack").'))

$started = Get-Date
$game = Start-Process -FilePath $exe -WorkingDirectory $emu -PassThru
Write-Host 'Emulator started. Load the game; the co-op panel is in the top-left corner of the game.'

# Join.bat: watch the emulator's log until the join worked or clearly did not, and then say
# so in plain words (hosting.ps1 has the three likely causes). Ends when the emulator closes,
# after 15 minutes, or once connected; closing this window does not touch the game.
if ($Mode -eq 'join' -and $game) {
    $logFile = Join-Path $user 'log\eden_log.txt'
    $shown = if ($inviteJoin) { 'the host (through the join helper window)' } else { Format-JoinAddress $Server ([int]$Port) }
    Write-Host "This window says when you are connected to $shown (you may also close it)."
    $trying = $null; $helped = $false; $state = ''
    while (-not $game.HasExited -and ((Get-Date) - $started).TotalMinutes -lt 15) {
        Start-Sleep -Seconds 2
        $text = ''
        try {
            if ((Test-Path -LiteralPath $logFile) -and (Get-Item -LiteralPath $logFile).LastWriteTime -ge $started.AddSeconds(-2)) {
                $stream = [System.IO.File]::Open($logFile, 'Open', 'Read', 'ReadWrite, Delete')
                try { $text = [System.IO.StreamReader]::new($stream).ReadToEnd() } finally { $stream.Dispose() }
            }
        } catch { }
        $state = Get-JoinState $text
        if ($state -eq 'welcomed') { Write-Host "Connected to $shown. Have fun!"; break }
        if ($state -like 'refused:*') {
            Write-Host "The host's server said no: $($state.Substring(9))."
            Write-Host 'name taken: pick another name with play.cmd change. wrong protocol version: everyone runs Update.bat.'
            Write-Host 'wrong join password: start Join.bat again and type the password the host sent you.'
            exit 1
        }
        if ($state -eq 'trying') {
            if (-not $trying) { $trying = Get-Date }
            elseif (-not $helped -and ((Get-Date) - $trying).TotalSeconds -ge 30) {
                Write-Host ''
                if ($inviteJoin) { Write-Host 'Not connected yet. The join helper window says what is going on (and shows an ANSWER CODE for the host if needed).' }
                else { foreach ($line in (Get-JoinFailureHelp $shown)) { Write-Host $line } }
                Write-Host 'The game keeps trying by itself; this window says when it connects.'
                $helped = $true
            }
        }
    }
}
