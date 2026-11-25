# SPDX-License-Identifier: GPL-3.0-or-later
# exefs spike, unattended run on a stock Fritchii (no co-op patches): the subsdk9 mod, coop_server on 127.0.0.1:27510,
# the game, then the module's lines in the emulator's log. Exit 0 = PASS (the server's VERIFY_CONNECT reached the
# module in the game), 1 = FAIL, 2 = set-up problem. Results and a screenshot go to -Out.
#   powershell -File run-fritchii.ps1 -Emu <Fritchii folder> -Game <.nup> -From <kit emulator folder for keys+firmware>
#     -Server <coop_server.exe> -Mod <build.ps1's out folder> -Out <results folder> [-Minutes 6] [-Display DISPLAY2]
param(
    [Parameter(Mandatory)][string]$Emu,
    [Parameter(Mandatory)][string]$Game,
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$Server,
    [Parameter(Mandatory)][string]$Mod,
    [Parameter(Mandatory)][string]$Out,
    [int]$Minutes = 6,
    [string]$Display = '',
    [string]$Python = (Join-Path $PSScriptRoot '..\..\..\tools-venv\Scripts\python.exe')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$result = Join-Path $Out 'result.txt'
function Say([string]$s) { $l = "$((Get-Date).ToUniversalTime().ToString('HH:mm:ss')) $s"; Write-Host $l; Add-Content -LiteralPath $result $l }
Set-Content -LiteralPath $result "exefs-spike run on $Emu"

# --- a portable user folder beside the emulator: keys, firmware and config from the kit's emulator; saves start empty
$user = Join-Path $Emu 'user'
foreach ($d in 'keys', 'config', 'nand\system') {
    $src = Join-Path $From "user\$d"
    if (-not (Test-Path -LiteralPath $src)) { Say "SETUP: $src missing"; exit 2 }
    $dst = Join-Path $user $d
    if (-not (Test-Path -LiteralPath $dst)) {
        New-Item -ItemType Directory -Force $dst | Out-Null
        robocopy $src $dst /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Say "SETUP: copy of $src failed ($LASTEXITCODE)"; exit 2 }
    }
}
# the kit's own co-op mods are not this emulator's: only the spike mod in load\
$load = Join-Path $user 'load\0400486024DEC000'
if (Test-Path -LiteralPath $load) { Remove-Item -Recurse -Force -LiteralPath $load }
New-Item -ItemType Directory -Force (Join-Path $load 'coop-exefs\exefs') | Out-Null
Copy-Item -Force (Join-Path $Mod 'subsdk9') (Join-Path $load 'coop-exefs\exefs\subsdk9')
Say "mod: $(Join-Path $load 'coop-exefs\exefs\subsdk9') ($((Get-Item (Join-Path $Mod 'subsdk9')).Length) bytes)"
$log = Join-Path $user 'log\eden_log.txt'
if (Test-Path -LiteralPath $log) { Remove-Item -Force -LiteralPath $log }

# --- the server (loopback only), then the game
$srvLog = Join-Path $Out 'coop_server.log'
$srv = Start-Process -FilePath $Server -PassThru -WindowStyle Hidden -RedirectStandardOutput $srvLog `
    -RedirectStandardError (Join-Path $Out 'coop_server.err.log') `
    -ArgumentList @('--bind', '127.0.0.1', '--port', '27510', '--password', 'exefs-spike', '--run-for-ms', "$(($Minutes + 1) * 60000)")
Start-Sleep -Seconds 2
if ($srv.HasExited) { Say "SETUP: coop_server stopped at once (port 27510 taken?)"; exit 2 }
$exe = Join-Path $Emu 'fritchii.exe'
$game = Start-Process -FilePath $exe -WorkingDirectory $Emu -PassThru -ArgumentList @('-g', "`"$Game`"")
Say "game pid $($game.Id), server pid $($srv.Id)"
$place = Join-Path $PSScriptRoot '..\..\tools\autotest\placement.py'
$pargs = @('-I', $place, 'place', '--pid', "$($game.Id)", '--wait', '60')
if ($Display) { $pargs += @('--display', $Display) }
& $Python @pargs 2>&1 | ForEach-Object { Say "place: $_" }

# --- wait for the module's lines
$deadline = (Get-Date).AddMinutes($Minutes)
$verdict = ''
while ((Get-Date) -lt $deadline -and -not $game.HasExited) {
    Start-Sleep -Seconds 5
    if (-not (Test-Path -LiteralPath $log)) { continue }
    $lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Where-Object { $_ -match '\[coop-exefs\]' })
    if ($lines -match 'VERIFY_CONNECT: server answered') { $verdict = 'PASS'; break }
    if ($lines -match 'no reply after|not found|FAILED|not empty') { $verdict = 'FAIL'; break }
}
Start-Sleep -Seconds 3

# --- a screenshot of the game's window (the game ran with the mod in it)
try {
    Add-Type -AssemblyName System.Drawing
    Add-Type @'
using System; using System.Runtime.InteropServices;
public static class SpikeWin { [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Ri, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r); }
'@
    $game.Refresh()
    $r = New-Object SpikeWin+R
    if ($game.MainWindowHandle -ne 0 -and [SpikeWin]::GetWindowRect($game.MainWindowHandle, [ref]$r)) {
        $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.Ri - $r.L)), ([Math]::Max(1, $r.B - $r.T))
        [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
        $bmp.Save((Join-Path $Out 'game.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        Say "screenshot: $(Join-Path $Out 'game.png')"
    } else { Say 'screenshot: no game window' }
} catch { Say "screenshot failed: $_" }

# --- close gently, keep the logs
if (-not $game.HasExited) { [void]$game.CloseMainWindow(); if (-not $game.WaitForExit(30000)) { Say 'game did not close in 30 s: stopped'; Stop-Process -Id $game.Id -Force } }
if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force }
if (Test-Path -LiteralPath $log) {
    Copy-Item -Force $log (Join-Path $Out 'eden_log.txt')
    Get-Content -LiteralPath $log | Where-Object { $_ -match '\[coop-exefs\]|subsdk9|coop-exefs|LayeredExeFS' } | ForEach-Object { Say "log: $_" }
}
if (-not $verdict) { $verdict = 'FAIL'; Say 'no verdict line from the module before the time ran out (or the game closed)' }
Say "RESULT $verdict"
if ($verdict -eq 'PASS') { exit 0 } else { exit 1 }
