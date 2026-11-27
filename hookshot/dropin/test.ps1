# SPDX-License-Identifier: GPL-3.0-or-later
# The drop-in's test with no game: builds out\winmm.dll (build.ps1), out\selftest.exe and a stand-in game
# out\fake\longshot.exe (fake_longshot.c), then runs the stand-in three ways and checks squadala-dropin.log:
#   1. hook on: attached, 2 of 2 symbols, hooked, the guest word, frame 1 / 600 / 1200, the game's body ran 1200 times
#      through the trampoline, forwarded winmm calls answered;
#   2. "badword": the guest word differs -> pass-through, no frame lines, the body still runs 1200 times;
#   3. SQUADALA_DROPIN=0: forwarding only, no hook.
# Exit 0 = PASS. Usage: powershell -ExecutionPolicy Bypass -File hookshot\dropin\test.ps1
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))
$cc = Join-Path $root 'hookshot-tools\llvm-mingw-20261104-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe'
$out = Join-Path $here 'out'
$fake = Join-Path $out 'fake'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'build.ps1')
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
    $o = if ($arg) { & (Join-Path $fake 'longshot.exe') $arg } else { & (Join-Path $fake 'longshot.exe') }
    $code = $LASTEXITCODE
    Remove-Item Env:SQUADALA_DROPIN -ErrorAction SilentlyContinue
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    $bad = @()
    if ($code -ne 0) { $bad += "exit $code ($o)" }
    foreach ($w in $want) { if ($text -notmatch $w) { $bad += "missing '$w'" } }
    foreach ($w in $wantNot) { if ($text -match $w) { $bad += "unexpected '$w'" } }
    if ($bad.Count) { $script:fails++; Write-Output "CASE $name FAIL: $($bad -join '; ')"; Write-Output $text }
    else { Write-Output "CASE $name PASS ($o)" }
}
Run-Case 'hook-on' '' '' @('dropin: attached', '2 of 2 wanted found', 'hooked at', "the game's pacibsp: hook on",
    'dropin: frame 1 \(x0 0x1000, lr guest 0x806e6dd8\)', 'dropin: frame 600 ', 'dropin: frame 1200 ') @('left out', 'pass-through')
Run-Case 'bad-word' 'badword' '' @('hooked at', 'NOT the expected word: pass-through') @('dropin: frame [0-9]')
Run-Case 'off' '' '0' @('SQUADALA_DROPIN=0: forwarding winmm only') @('hooked at', 'dropin: frame [0-9]')
$st = & (Join-Path $out 'selftest.exe') (Join-Path $fake 'longshot.exe') 2>&1
Write-Output "selftest on the stand-in (expected to miss the real game's other symbols): $(@($st)[0])"
if ($fails) { Write-Output "TEST FAIL ($fails case(s))"; exit 1 }
Write-Output 'TEST PASS'
exit 0
