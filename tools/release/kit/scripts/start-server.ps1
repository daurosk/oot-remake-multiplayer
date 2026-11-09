# Runs the co-op server from this kit and writes its output to logs\server-<date>.log.
# Usage: start-server.cmd [port] [more coop_server options]
param(
    [int]$Port = @PORT@,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)
$ErrorActionPreference = 'Continue'
$kit = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $kit 'server\coop_server.exe'
if (-not (Test-Path $exe)) { Write-Host "Not found: $exe"; exit 1 }
if ($Port -lt 1 -or $Port -gt 65535) { Write-Host "The port must be 1 to 65535."; exit 1 }

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

$args2 = @('--port', "$Port") + @($Rest | Where-Object { $_ })
& $exe @args2 2>&1 | ForEach-Object {
    $text = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
    $line = '{0:HH:mm:ss.fff} {1}' -f (Get-Date), $text
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}
$code = $LASTEXITCODE
Add-Content -Path $log -Value ('{0:HH:mm:ss.fff} (server exited with code {1})' -f (Get-Date), $code) -Encoding UTF8
exit $code
