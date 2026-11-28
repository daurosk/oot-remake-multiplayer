# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the drop-in proof winmm.dll (hookshot/dropin) with the kit's llvm-mingw. Out: hookshot/dropin/out/winmm.dll
# Usage: powershell -ExecutionPolicy Bypass -File hookshot\dropin\build.ps1 [-Toolchain <llvm-mingw bin>] [-Python <python.exe>]
# (CI, build.yml windows job: both from Longshot's tools.lock that tools\hookshot\host_compile_check.ps1 unpacks)
param([string]$Toolchain = "", [string]$Python = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))   # the folder holding the checkouts
if (-not $Toolchain) { $Toolchain = Join-Path $root "hookshot-tools\llvm-mingw-20261104-ucrt-x86_64\bin" }
$cc = Join-Path $Toolchain "x86_64-w64-mingw32-clang.exe"
if (-not (Test-Path $cc)) { Write-Output "SETUP-FAIL no compiler at $cc"; exit 2 }
$py = if ($Python) { $Python } else { Join-Path $root "tools-venv\Scripts\python.exe" }
if (-not (Test-Path $py)) { Write-Output "SETUP-FAIL no python at $py"; exit 2 }
$out = Join-Path $here "out"
New-Item -ItemType Directory -Force $out | Out-Null
$sysWinmm = Join-Path $env:SystemRoot "System32\winmm.dll"
& $py -I (Join-Path $here "gen_winmm.py") $sysWinmm $out
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD-FAIL gen_winmm"; exit 1 }
& $cc -O2 -shared -o (Join-Path $out "winmm.dll") (Join-Path $here "dropin.c") (Join-Path $out "winmm_names.c") `
    (Join-Path $out "winmm_stubs.S") (Join-Path $out "winmm.def") -static "-Wl,--kill-at"
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD-FAIL compile"; exit 1 }
Write-Output ("BUILT " + (Join-Path $out "winmm.dll") + " " + (Get-Item (Join-Path $out "winmm.dll")).Length + " bytes")
