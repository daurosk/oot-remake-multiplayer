# The host's self-check (check-hosting.cmd): is this kit's server running and on which
# networks, does a firewall rule let it in (only coop_server.exe, only UDP), is server_allow
# set. It reads only; for each problem it prints the exact command for the player to run.
# Usage: check-hosting.cmd [port]   (default: the port this kit's server listens on, else the port of the
#                                   coop.ini written last: the emulator's or the Hookshot game's)
param([int]$Port = 0)
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'coop-ini.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
. (Join-Path $PSScriptRoot 'hosting.ps1')
$kit = Split-Path -Parent $PSScriptRoot
$serverExe = [System.IO.Path]::GetFullPath((Join-Path $kit 'server\coop_server.exe'))
$serverPids = @()
$serverPorts = @()
try {
    $serverPids = @(Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $serverExe } | ForEach-Object { $_.Id })
    if ($serverPids.Count) {
        $serverPorts = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -in $serverPids } |
            ForEach-Object { [int]$_.LocalPort } | Sort-Object -Unique)
    }
} catch { }
# (Host-Hookshot.bat writes the Hookshot game's coop.ini, not the emulator's: its port was not checked before)
$defaultPort = @PORT@
if (-not $Port) { $Port = Get-KitHostPort $kit $serverPorts $defaultPort }
Write-Host "Checking this PC for hosting (kit: $kit, UDP port $Port)..."
Write-Host ''

$listen = @()
try {
    $pids = @(Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $serverExe } | ForEach-Object { $_.Id })
    if ($pids.Count) {
        $listen = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -in $pids })
    }
} catch { }
$profiles = @()
try { $profiles = @(Get-NetConnectionProfile -ErrorAction Stop | ForEach-Object { [pscustomobject]@{ Alias = $_.InterfaceAlias; Category = "$($_.NetworkCategory)" } }) } catch { }
$findings = Get-HostingFindings $serverExe $Port (Get-ServerFirewallRules $serverExe) $listen (Get-LocalAddresses) $profiles (Read-KitServerAccess (Join-Path $kit 'coop-options.ini'))

$problems = 0
foreach ($f in $findings) {
    $tag = switch ($f.Level) { 'ok' { 'OK     ' } 'problem' { 'PROBLEM' } default { 'NOTE   ' } }
    if ($f.Level -eq 'problem') { $problems++ }
    $first = $true
    foreach ($line in $f.Lines) {
        if ($first) { Write-Host "$tag $line"; $first = $false } else { Write-Host "        $line" }
    }
}
Write-Host ''
if ($problems) { Write-Host 'Fix the PROBLEM lines (the commands go in Command Prompt as administrator), then run this again.' }
else { Write-Host 'Nothing wrong found on this PC. Still no luck: check the address your friends typed, and the VPN or router (docs\play-online.md).' }
Write-Host 'This check changed nothing on your PC.'
