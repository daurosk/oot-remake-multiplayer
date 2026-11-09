# Writes emulator\user\config\coop.ini (asking for what is missing) and starts the emulator.
# Usage: play.cmd                  asks the first time, then reuses the answers
#        play.cmd change           asks again
#        play.cmd -Server 192.168.1.20 -Name Saria [-Port 27510]   no questions
#        Host.bat (play.ps1 host)  starts this kit's server too and plays on it (127.0.0.1)
#        Join.bat (play.ps1 join)  asks for the host's address each time
param(
    [string]$Mode = '',
    [string]$Server = '',
    [string]$Name = '',
    [string]$Port = ''
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

# A newer release? One short question to GitHub; nothing is shown when there is none or no
# network. Update.bat does the update.
$updater = Join-Path $PSScriptRoot 'update.ps1'
if (Test-Path $updater) {
    try { & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $updater -Check -Kit $kit } catch { }
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

# Switches from coop-options.ini (the lines without a '#' in front).
$options = Read-KitOptions (Join-Path $kit 'coop-options.ini')
foreach ($key in $options.Keys) {
    $value = $options[$key]
    if ($value -and -not [System.IO.Path]::IsPathRooted($value) -and (Test-Path -LiteralPath (Join-Path $kit $value))) {
        $value = [System.IO.Path]::GetFullPath((Join-Path $kit $value))
    }
    Set-Item -Path "env:$key" -Value $value
    Write-Host "Option: $key=$value"
}
$pack = Join-Path (Get-KitLoadDir $user) '0400486024DEC000\coop-puppet\coop_puppet.json'
if (Test-Path -LiteralPath $pack) { Write-Host 'Puppet pack: found.' }
else {
    Write-Host 'Puppet pack: not made yet, so the other players show only as markers.'
    Write-Host '  Make it once with make-puppet-pack.cmd (SETUP.md, "The puppet pack").'
    if ($env:LICHII_COOP_PUPPET) { Remove-Item env:LICHII_COOP_PUPPET; Write-Host 'Option: LICHII_COOP_PUPPET off (no pack)' }
}

Start-Process -FilePath $exe -WorkingDirectory $emu | Out-Null
Write-Host 'Emulator started. Load the game; the co-op panel is in the top-left corner of the game.'
