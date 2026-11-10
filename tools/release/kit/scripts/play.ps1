# Writes emulator\user\config\coop.ini (asking for what is missing) and starts the emulator.
# Usage: play.cmd                  asks the first time, then reuses the answers
#        play.cmd change           asks again
#        play.cmd -Server 192.168.1.20 -Name Saria [-Port 27510]   no questions
#        Host.bat (play.ps1 host)  starts this kit's server too and plays on it (127.0.0.1)
#        Join.bat (play.ps1 join)  asks for the host's address each time
# On every start it looks for a newer co-op release and, as auto_update in coop-options.ini
# says, updates the kit (emulator included) and starts the new kit's play.ps1 with -NoUpdate.
# Every later play.ps1 must accept -NoUpdate.
param(
    [string]$Mode = '',
    [string]$Server = '',
    [string]$Name = '',
    [string]$Port = '',
    [switch]$NoUpdate
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'coop-ini.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$kit = Split-Path -Parent $PSScriptRoot
$emu = Join-Path $kit 'emulator'
$exe = Join-Path $emu 'lichii.exe'
$ini = Join-Path $emu 'user\config\coop.ini'
$template = Join-Path $PSScriptRoot 'coop.ini.template'
if (-not (Test-Path $exe)) { Write-Host "Not found: $exe"; exit 1 }

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

# A newer co-op release? One short question to GitHub (update.ps1 -Status), given 6 s at
# most: a slow name lookup or proxy search is not bounded by the request's own timeout, so it
# is stopped when it takes longer. No network, or GitHub down: nothing is said and this kit
# starts as it is.
$updater = Join-Path $PSScriptRoot 'update.ps1'
if (-not $NoUpdate -and (Test-Path $updater)) {
    $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $status = ''
    $out = [System.IO.Path]::GetTempFileName()
    try {
        $check = Start-Process -FilePath $ps -NoNewWindow -PassThru -RedirectStandardOutput $out -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$updater`"", '-Status', '-Kit', "`"$kit`"")
        if ($check.WaitForExit(6000)) { $status = ([System.IO.File]::ReadAllText($out)).Trim() } else { $check.Kill() }
    } catch { }
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
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
                & $ps @next -NoUpdate
                exit $LASTEXITCODE
            }
            if ($code -ne 0) { Write-Host 'The update did not happen; this version starts as it is.' }
        }
    }
}

$current = Read-CoopIni $ini
$ask = $Mode -eq 'change'
$serverGiven = [bool]$Server
$askServer = $ask -or ($Mode -eq 'join' -and -not $serverGiven)
if ($Mode -eq 'host') { $Server = '127.0.0.1' }
if (-not $Server) { $Server = $current.server }
if ($Mode -eq 'join' -and -not $serverGiven -and $Server -match '^(127\.|localhost)') { $Server = '' }
if (-not $Name) { $Name = $current.name }
if (-not $Port) { $Port = if ($current.port) { $current.port } else { '@PORT@' } }

if ($askServer -or -not $Server) {
    Write-Host 'Server address: the IP of the PC running start-server.cmd (start-server shows it).'
    Write-Host 'Use 127.0.0.1 if the server runs on this PC. host:port also works.'
    do {
        $answer = Read-Host "Server address$(if ($Server) { " [$Server]" })"
        if ($answer) { $Server = $answer.Trim() }
        $ok = Test-CoopServer $Server
        if (-not $ok) { Write-Host 'Letters, digits, dots and dashes only, optionally :port.' }
    } until ($ok)
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

Write-CoopIni $template $ini $Server $Port $Name
Write-Host "Co-op: server $Server, port $Port, name $Name   (play.cmd change to change the name)"

$user = Join-Path $emu 'user'
if (-not (Get-ChildItem (Join-Path $user 'keys') -Filter '*.keys' -ErrorAction SilentlyContinue)) {
    Write-Host "No keys in $user\keys yet. SETUP.md says what goes where."
}
# No game mods with co-op (issue 136): stop before anything starts, and say where they are.
$mods = @(Get-KitMods $user)
if ($mods.Count -gt 0) {
    Write-Host ''
    Write-Host 'Co-op does not work with game mods, and these are installed for Ocarina of Time:'
    foreach ($m in $mods) { Write-Host "  $m" }
    Write-Host 'Remove those folders, or turn the mods off: in the emulator, right-click the game,'
    Write-Host 'Properties, Add-Ons, untick them. Then start again. (co-op''s own coop-puppet stays.)'
    exit 1
}
if ($Mode -eq 'host') {
    $serverExe = Join-Path $kit 'server\coop_server.exe'
    $serverUp = Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $serverExe }
    if ($serverUp) { Write-Host 'The server from this kit is already running.' }
    else {
        Write-Host "Starting the server in its own window (UDP port $Port). Keep that window open while you play."
        Start-Process -FilePath (Join-Path $kit 'start-server.cmd') -ArgumentList $Port -WorkingDirectory $kit | Out-Null
        Start-Sleep -Seconds 2
    }
}
$running = Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
if ($running) { Write-Host 'This emulator is already running; close it first to use new settings.'; exit 1 }

# A copied kit: point the emulator's saved folders at this copy (kit-paths.ps1).
$fixed = Repair-KitPaths $user
if ($fixed) { Write-Host "This kit was copied: pointed $($fixed -join ', ') at $user" }

# Switches from coop-options.ini (the lines without a '#' in front). They win over the AMD rule below.
$options = Read-KitOptions (Join-Path $kit 'coop-options.ini')
foreach ($key in $options.Keys) {
    $value = $options[$key]
    if ($value -and -not [System.IO.Path]::IsPathRooted($value) -and (Test-Path -LiteralPath (Join-Path $kit $value))) {
        $value = [System.IO.Path]::GetFullPath((Join-Path $kit $value))
    }
    Set-Item -Path "env:$key" -Value $value
    Write-Host "Option: $key=$value"
}
# AMD cards: shader libraries off unless the player chose (kit-paths.ps1, issue 118).
$gpl = Set-AmdGplDefault
if ($gpl) { Write-Host $gpl }
$pack = Join-Path (Get-KitLoadDir $user) '0400486024DEC000\coop-puppet\coop_puppet.json'
if (Test-Path -LiteralPath $pack) {
    Write-Host 'Puppet pack: found.'
    # Made before sound or before the kit's pack tool changed (kit-paths.ps1): say so, start anyway.
    $hint = Get-PuppetPackHint $pack (Join-Path $kit 'tools\puppet_pack')
    if ($hint) { Write-Host $hint }
}
else {
    Write-Host 'Puppet pack: not made yet, so the other players show only as markers.'
    Write-Host '  Make it once with make-puppet-pack.cmd (SETUP.md, "The puppet pack").'
    if ($env:LICHII_COOP_PUPPET) { Remove-Item env:LICHII_COOP_PUPPET; Write-Host 'Option: LICHII_COOP_PUPPET off (no pack)' }
}

Start-Process -FilePath $exe -WorkingDirectory $emu | Out-Null
Write-Host 'Emulator started. Load the game; the co-op panel is in the top-left corner of the game.'
