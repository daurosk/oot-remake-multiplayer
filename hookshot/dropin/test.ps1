# SPDX-License-Identifier: GPL-3.0-or-later
# The drop-in's test with no game: builds out\winmm.dll (build.ps1), out\selftest.exe and a stand-in game
# out\fake\longshot.exe (fake_longshot.c), then runs the stand-in seven ways and checks squadala-dropin.log:
#   1. hook on: attached, 2 of 2 symbols, hooked, the guest word, frame 1 / 600 / 1200, the game's body ran 1200 times
#      through the trampoline, forwarded winmm calls answered;
#   2. "badword": the guest word differs -> pass-through, no frame lines, the body still runs 1200 times;
#   3. SQUADALA_DROPIN=0: forwarding only, no hook.
#   4. "--crash-watch" (the game's own crash helper, Longshot host/hos_crash_dialog.h): forwarding only, no hook.
#   5. "--crash-watcher" (not the helper's word): hooked as the game (the argv[1] check is exact, kernel32 only);
#   6. the helper's command line exactly as Longshot quotes it ("--crash-watch" "<pid>" ... "<folder>\\" ...): forwarding only
#      (run v1-dropin-43b1a3d9 FAILED on this: 43b1a3d91 hooked the quoted helper);
#   7. a quoted other word ("--crash-watcher"): hooked as the game. Every log line carries [pid N].
# Exit 0 = PASS. Usage: powershell -ExecutionPolicy Bypass -File hookshot\dropin\test.ps1 [-Toolchain <llvm-mingw bin>] [-Python <python.exe>]
# (both passed on to build.ps1; CI's build.yml windows job runs this with Longshot's own llvm-mingw and Python)
param([string]$Toolchain = '', [string]$Python = '')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))
if (-not $Toolchain) { $Toolchain = Join-Path $root 'hookshot-tools\llvm-mingw-20261104-ucrt-x86_64\bin' }
$cc = Join-Path $Toolchain 'x86_64-w64-mingw32-clang.exe'
$out = Join-Path $here 'out'
$fake = Join-Path $out 'fake'
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $here 'build.ps1'), '-Toolchain', $Toolchain)
if ($Python) { $buildArgs += @('-Python', $Python) }
& powershell @buildArgs
if ($LASTEXITCODE -ne 0) { Write-Output 'TEST FAIL build.ps1'; exit 1 }
& $cc -O1 -o (Join-Path $out 'selftest.exe') (Join-Path $here 'selftest.c')
if ($LASTEXITCODE -ne 0) { Write-Output 'TEST FAIL selftest.c'; exit 1 }
New-Item -ItemType Directory -Force $fake | Out-Null
& $cc -O0 -o (Join-Path $fake 'longshot.exe') (Join-Path $here 'fake_longshot.c') -lwinmm
if ($LASTEXITCODE -ne 0) { Write-Output 'TEST FAIL fake_longshot.c'; exit 1 }
Copy-Item -LiteralPath (Join-Path $out 'winmm.dll') -Destination (Join-Path $fake 'winmm.dll') -Force
$log = Join-Path $fake 'squadala-dropin.log'
$fails = 0
function Run-Case([string]$name, [string]$arg, [string]$envValue, [string[]]$want, [string[]]$wantNot) {
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    $env:SQUADALA_DROPIN = $envValue
    if ($arg.StartsWith('RAW:')) {
        # the command line exactly as given (its own quotes), as Longshot starts its helper: ProcessStartInfo.Arguments is
        # passed through unchanged, where PowerShell's & would re-quote it
        $psi = New-Object System.Diagnostics.ProcessStartInfo (Join-Path $fake 'longshot.exe'), $arg.Substring(4)
        $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true
        $pr = [System.Diagnostics.Process]::Start($psi); $o = $pr.StandardOutput.ReadToEnd(); $pr.WaitForExit(); $code = $pr.ExitCode
    } else {
        $o = if ($arg) { & (Join-Path $fake 'longshot.exe') $arg } else { & (Join-Path $fake 'longshot.exe') }
        $code = $LASTEXITCODE
    }
    Remove-Item Env:SQUADALA_DROPIN -ErrorAction SilentlyContinue
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    $bad = @()
    if ($code -ne 0) { $bad += "exit $code ($o)" }
    foreach ($w in $want) { if ($text -notmatch $w) { $bad += "missing '$w'" } }
    foreach ($w in $wantNot) { if ($text -match $w) { $bad += "unexpected '$w'" } }
    if ($bad.Count) { $script:fails++; Write-Output "CASE $name FAIL: $($bad -join '; ')"; Write-Output $text }
    else { Write-Output "CASE $name PASS ($o)" }
}
Run-Case 'hook-on' '' '' @('\[pid [0-9]+\] dropin: attached', '2 of 2 wanted found', 'hooked at', "the game's pacibsp: hook on",
    'dropin: frame 1 \(x0 0x1000, lr guest 0x806e6dd8\)', 'dropin: frame 600 ', 'dropin: frame 1200 ') @('left out', 'pass-through')
Run-Case 'bad-word' 'badword' '' @('hooked at', 'NOT the expected word: pass-through') @('dropin: frame [0-9]')
Run-Case 'off' '' '0' @('SQUADALA_DROPIN=0: forwarding winmm only') @('hooked at', 'dropin: frame [0-9]')
Run-Case 'crash-watch-like' '--crash-watcher' '' @('hooked at', "the game's pacibsp: hook on") @('crash-watch helper')
Run-Case 'crash-watch' '--crash-watch' '' @("\[pid [0-9]+\] dropin: the game's --crash-watch helper: forwarding winmm only") @('hooked at', 'dropin: frame [0-9]', 'COFF symbols')
Run-Case 'crash-watch-quoted' 'RAW:"--crash-watch" "93332" "2ec0e5f965819e33dfd8b0bc1b26cfaa" "C:\lsb\v3.2.0\game\\" "C:\lsb\v3.2.0\game\logs\longshot_log.txt"' '' @("dropin: the game's --crash-watch helper: forwarding winmm only") @('hooked at', 'dropin: frame [0-9]', 'COFF symbols')
Run-Case 'quoted-not-helper' 'RAW:"--crash-watcher" "1"' '' @('hooked at', "the game's pacibsp: hook on") @('crash-watch helper')
$st = & (Join-Path $out 'selftest.exe') (Join-Path $fake 'longshot.exe') 2>&1
Write-Output "selftest on the stand-in (expected to miss the real game's other symbols): $(@($st)[0])"
if ($fails) { Write-Output "TEST FAIL ($fails case(s))"; exit 1 }
Write-Output 'TEST PASS'
exit 0
