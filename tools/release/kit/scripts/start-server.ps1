# Runs the co-op server from this kit and writes its output to logs\server-<date>.log.
# Usage: start-server.cmd [port] [-Allow 25.1.2.3,26.0.0.0/8] [-Bind 25.1.2.3] [more coop_server options]
# -Allow: only these IPv4 addresses (or ranges) may join; -Bind: listen on this one address of
# this PC only (its Hamachi or Radmin address). play.ps1 passes coop-options.ini's server_allow
# and server_bind here; the join password (server_password) is read from coop-options.ini here.
param(
    [Parameter(Position = 0)][int]$Port = @PORT@,
    [string]$Allow = '',
    [string]$Bind = '',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)
$ErrorActionPreference = 'Continue'
$kit = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $kit 'server\coop_server.exe'
if (-not (Test-Path $exe)) { Write-Host "Not found: $exe"; exit 1 }
if ($Port -lt 1 -or $Port -gt 65535) { Write-Host "The port must be 1 to 65535."; exit 1 }
$access = @()
$range = '^\d{1,3}(\.\d{1,3}){3}(/\d{1,2})?$'
foreach ($a in @($Allow -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    if ($a -notmatch $range) { Write-Host "server_allow: '$a' is not an IPv4 address (25.1.2.3) or range (26.0.0.0/8)."; exit 1 }
    $access += @('--allow', $a)
}
if ($Bind.Trim()) {
    if ($Bind.Trim() -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { Write-Host "server_bind: '$Bind' is not an IPv4 address (25.1.2.3)."; exit 1 }
    $access += @('--bind', $Bind.Trim())
}

# The join password (coop-options.ini server_password, Host.bat asks for it once). It goes to
# the server in an environment variable of this window only, never on its command line,
# and is never printed or logged.
. (Join-Path $PSScriptRoot 'coop-ini.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
. (Join-Path $PSScriptRoot 'invite.ps1')
$kitAccess = Read-KitServerAccess (Join-Path $kit 'coop-options.ini')
$password = $kitAccess.Password
# No server_password (no line, or an empty one): a new random one for this session. Every
# session has a password, which also encrypts the traffic (docs/play-online.md). Host.bat
# makes it and hands it over in OOT_COOP_SESSION_PASSWORD, so the host's own game has it too.
$madePassword = $false
if ($null -eq $password) {
    $password = if ($env:OOT_COOP_SESSION_PASSWORD) { $env:OOT_COOP_SESSION_PASSWORD } else { New-JoinPassword }
    $madePassword = $true
}
if (-not (Test-CoopPassword $password)) { Write-Host 'server_password: no spaces, at most 128 characters. The server was not started.'; exit 1 }
$env:OOT_COOP_SERVER_PASSWORD = $password
$access += @('--password-env', 'OOT_COOP_SERVER_PASSWORD')
# The router forward (upnp = yes) and the relay (relay = address): docs/play-online.md.
. (Join-Path $PSScriptRoot 'relay-join.ps1')
$hosting = Get-ServerHostingArgs $kitAccess $password $Allow.Trim()
$access += $hosting.Args
if ($kitAccess.RelayKey) { $env:OOT_COOP_RELAY_KEY = $kitAccess.RelayKey }
# PvP (coop-options.ini pvp = on): players can hurt each other. The host's value is the
# whole session's; the host can change it in the game with /pvp.
if ($kitAccess.Pvp) { $access += @('--pvp', 'on') }

$logs = Join-Path $kit 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$log = Join-Path $logs ('server-{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))

Write-Host "Co-op server on UDP port $Port. Log: $log"
Write-Host 'Keep this window open while you play; closing it (or Ctrl+C) stops the server.'
if ($madePassword) {
    Write-Host 'Join password: a new random one for this session; it is inside the invite code (server_password in coop-options.ini sets your own).'
    # Shown here only (never in the log), for friends who join by a VPN or home-network address.
    Write-Host "Join password for this session: $password   (only needed when a friend joins by address instead of the invite code)"
}
else { Write-Host "Join password: $password   (server_password in coop-options.ini). Send it to your friends with the address." }
Write-Host 'Every session has a join password, and the traffic is encrypted with it.'
if ($kitAccess.Pvp) { Write-Host 'PvP: on (pvp in coop-options.ini): players can hurt each other. /pvp off in the game chat turns it off.' }
else { Write-Host 'PvP: off (players cannot hurt each other; pvp = on in coop-options.ini, or /pvp on in the game chat).' }
# Which address the others type (hosting.ps1): VPN, internet (one HTTPS question to a fixed
# service, unless coop-options.ini says public_address_lookup = no), home network. The best
# one goes to the clipboard.
. (Join-Path $PSScriptRoot 'hosting.ps1')
$lookup = $true
foreach ($line in @(Get-Content -LiteralPath (Join-Path $kit 'coop-options.ini') -ErrorAction SilentlyContinue)) {
    if ($line -match '^\s*public_address_lookup\s*=\s*(no|off|false|0)\s*$') { $lookup = $false }
}
$public = ''
if ($lookup -and -not $Bind.Trim()) { $public = Get-PublicAddress 4000 }
$addresses = Get-JoinAddresses (Get-LocalAddresses) $public $Bind.Trim()
$best = Get-BestJoinAddress $addresses $Port
$copied = ''
if ($best) { try { Set-Clipboard -Value $best -ErrorAction Stop; $copied = $best } catch { } }
foreach ($line in (Format-JoinBlock $addresses $Port ($lookup -and -not $Bind.Trim() -and -not $public) $Allow.Trim() $copied $Bind.Trim())) { Write-Host $line }
foreach ($line in $hosting.Notes) { Write-Host $line }

# This kit's co-op release (VERSION.txt), named when the server refuses a game of another
# version, so the players know which one to update to.
$release = ''
$versionFile = Join-Path $kit 'VERSION.txt'
if (Test-Path -LiteralPath $versionFile) {
    $first = [System.IO.File]::ReadAllLines($versionFile) | Select-Object -First 1
    if ($first -match '\(release ([^)]+)\)') { $release = $Matches[1] }
}

$args2 = @('--port', "$Port") + $access + @($Rest | Where-Object { $_ })
# This session's port and password, for a host who closed the game: Host.bat then joins this
# server again instead of making a new password (kit-paths.ps1). Removed when the server ends.
$sessionText = ''
try { $sessionText = Write-KitHostSession $kit $Port $password } catch { }
& $exe @args2 2>&1 | ForEach-Object {
    $text = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
    if ($release -and $text -match 'refused a connection, reason 1\b') {
        $text += ". This server is co-op ${release}: every player needs $release (Update.bat updates to the newest release)."
    }
    # The relay's join code (relay-join.ps1): copied, so the host can paste it to the others.
    if ($text -match 'relay: join code \S+\. Give your friends (\S+) ') {
        try { Set-Clipboard -Value $Matches[1] -ErrorAction Stop; $text += ' Copied to the clipboard.' } catch { }
    }
    # The invite code (coop_server --invite): copied for the host, and never written to the log
    # (it holds the join password).
    $logged = $text
    if ($text -match 'invite code: ([A-Z2-9-]+)') {
        try { Set-Clipboard -Value $Matches[1] -ErrorAction Stop; $text += '   (copied to the clipboard)' } catch { }
        $logged = 'coop_server: invite code: (not in the log: it holds the join password)'
    }
    $line = '{0:HH:mm:ss.fff} {1}' -f (Get-Date), $text
    Write-Host $line
    Add-Content -Path $log -Value ('{0:HH:mm:ss.fff} {1}' -f (Get-Date), $logged) -Encoding UTF8
}
$code = $LASTEXITCODE
try {
    $sessionFile = Get-KitHostSessionFile $kit
    if ($sessionText -and (Test-Path -LiteralPath $sessionFile) -and [System.IO.File]::ReadAllText($sessionFile) -eq $sessionText) { Remove-Item -LiteralPath $sessionFile -Force }
} catch { }
Add-Content -Path $log -Value ('{0:HH:mm:ss.fff} (server exited with code {1})' -f (Get-Date), $code) -Encoding UTF8
exit $code
