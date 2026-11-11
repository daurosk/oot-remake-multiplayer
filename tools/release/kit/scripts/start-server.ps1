# Runs the co-op server from this kit and writes its output to logs\server-<date>.log.
# Usage: start-server.cmd [port] [-Allow 25.1.2.3,26.0.0.0/8] [-Bind 25.1.2.3] [more coop_server options]
# -Allow: only these IPv4 addresses (or ranges) may join; -Bind: listen on this one address of
# this PC only (its Hamachi or Radmin address). play.ps1 passes coop-options.ini's server_allow
# and server_bind here.
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

$logs = Join-Path $kit 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$log = Join-Path $logs ('server-{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))

Write-Host "Co-op server on UDP port $Port. Players on other PCs use one of these addresses:"
try {
    Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
        Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
        ForEach-Object { Write-Host ('  {0,-16} ({1})' -f $_.IPAddress, $_.InterfaceAlias) }
} catch { Write-Host '  (run ipconfig to see them)' }
Write-Host "Players on this PC use 127.0.0.1. Log: $log"
Write-Host 'Ctrl+C stops the server.'
Write-Host ''

# This kit's co-op release (VERSION.txt), named when the server refuses a game of another
# version, so the players know which one to update to.
$release = ''
$versionFile = Join-Path $kit 'VERSION.txt'
if (Test-Path -LiteralPath $versionFile) {
    $first = [System.IO.File]::ReadAllLines($versionFile) | Select-Object -First 1
    if ($first -match '\(release ([^)]+)\)') { $release = $Matches[1] }
}

if ($Allow.Trim()) { Write-Host "Only these addresses may join (server_allow): $Allow" }
$args2 = @('--port', "$Port") + $access + @($Rest | Where-Object { $_ })
& $exe @args2 2>&1 | ForEach-Object {
    $text = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
    if ($release -and $text -match 'refused a connection, reason 1\b') {
        $text += ". This server is co-op ${release}: every player needs $release (Update.bat updates to the newest release)."
    }
    $line = '{0:HH:mm:ss.fff} {1}' -f (Get-Date), $text
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}
$code = $LASTEXITCODE
Add-Content -Path $log -Value ('{0:HH:mm:ss.fff} (server exited with code {1})' -f (Get-Date), $code) -Encoding UTF8
exit $code
