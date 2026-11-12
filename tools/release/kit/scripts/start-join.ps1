# Join.bat's helper for an invite code (coop_server --join, server/join_helper.hpp), in its own
# window. Join.bat (play.ps1) starts it with the host's address from the invite code and puts the
# join password in OOT_COOP_JOIN_PASSWORD for this window only; it is never on a command line or
# in a log. The game connects to this helper on 127.0.0.1:GamePort. Output also goes to
# logs\join-<date>.log. An ANSWER CODE it shows is copied to the clipboard.
param(
    [Parameter(Mandatory = $true)][string]$HostAddress,
    [int]$GamePort = 27512
)
$ErrorActionPreference = 'Continue'
$kit = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $kit 'server\coop_server.exe'
if (-not (Test-Path $exe)) { Write-Host "Not found: $exe"; exit 1 }
if ($HostAddress -notmatch '^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$') { Write-Host "Not a host address: $HostAddress"; exit 1 }
if (-not $env:OOT_COOP_JOIN_PASSWORD) { Write-Host 'Start Join.bat and paste the invite code there; this window is started by it.'; exit 1 }
$host.UI.RawUI.WindowTitle = 'Co-op join helper (keep open while you play)'

$logs = Join-Path $kit 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$log = Join-Path $logs ('join-{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))
Write-Host 'Co-op join helper: the way from your game to the host. Keep this window open while you play;'
Write-Host "closing it ends the connection. Log: $log"
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$stun = (Read-KitServerAccess (Join-Path $kit 'coop-options.ini')).Stun
$args2 = @('--join', $HostAddress, '--game-port', "$GamePort", '--password-env', 'OOT_COOP_JOIN_PASSWORD')
foreach ($s in @($stun -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[A-Za-z0-9._-]{1,253}(:\d{1,5})?$' })) { $args2 += @('--stun', $s) }
& $exe @args2 2>&1 | ForEach-Object {
    $text = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
    if ($text -match 'answer code: ([A-Z2-9-]+)') {
        try { Set-Clipboard -Value $Matches[1] -ErrorAction Stop; $text += '   (copied to the clipboard)' } catch { }
    }
    $line = '{0:HH:mm:ss.fff} {1}' -f (Get-Date), $text
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}
$code = $LASTEXITCODE
Add-Content -Path $log -Value ('{0:HH:mm:ss.fff} (helper exited with code {1})' -f (Get-Date), $code) -Encoding UTF8
Write-Host 'The join helper stopped. Press a key to close this window.'
try { [Console]::ReadKey($true) | Out-Null } catch { }
exit $code
