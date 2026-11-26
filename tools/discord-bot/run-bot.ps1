# Starts the Squadala! bug-report bot (tools/discord-bot/bot.py) and restarts it if it stops.
# Config: coord-multiplayer\secrets\discord-bot.json (never committed). See README.md.
# Python: ..\..\..\tools-venv first, the old oot-remake-recomp\.venv link only as a fallback.
# Every start, stop and restart reason goes into coord-multiplayer\player-logs\discord-bot.log (never the token).
param(
    [string]$Config = (Join-Path $PSScriptRoot '..\..\..\coord-multiplayer\secrets\discord-bot.json'),
    [string]$Python = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Python) {
    foreach ($c in @('..\..\..\tools-venv\Scripts\python.exe', '..\..\..\oot-remake-recomp\.venv\Scripts\python.exe')) {
        $p = Join-Path $PSScriptRoot $c
        if (Test-Path $p) { $Python = $p; break }
    }
}
$logDir = Join-Path (Split-Path -Parent (Split-Path -Parent $Config)) 'player-logs'
$runLog = Join-Path $logDir 'discord-bot.log'
function Write-RunLog([string]$text) {
    $line = '{0} INFO run-bot: {1}' -f (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss'), $text
    try {
        if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }
        [IO.File]::AppendAllText($runLog, $line + "`r`n")
    } catch { }
    Write-Host $line
}
if (-not $Python -or -not (Test-Path $Python)) { Write-RunLog "stopped: Python not found (tools-venv or the old .venv)"; exit 1 }
if (-not (Test-Path $Config)) { Write-RunLog "stopped: config not found (see tools\discord-bot\README.md)"; exit 1 }
$bot = Join-Path $PSScriptRoot 'bot.py'
$delay = 10
while ($true) {
    $started = Get-Date
    Write-RunLog "starting the bot with $([IO.Path]::GetFileName((Split-Path -Parent (Split-Path -Parent $Python))))\Scripts\python.exe"
    & $Python $bot --config $Config
    $code = $LASTEXITCODE
    # 2 = bad config or token, 3 = discord.py missing: restarting will not help.
    if ($code -eq 2 -or $code -eq 3) { Write-RunLog "stopped (exit $code): fix the config or install discord.py, then start again"; exit $code }
    if (((Get-Date) - $started).TotalMinutes -gt 10) { $delay = 10 } else { $delay = [Math]::Min($delay * 2, 600) }
    Write-RunLog "bot exited (code $code) after $([int]((Get-Date) - $started).TotalSeconds) s; restarting in $delay s"
    Start-Sleep -Seconds $delay
}
