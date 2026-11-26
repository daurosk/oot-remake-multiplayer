# Tests for the Windows kit's puppet pack tool (tools/release/kit_pack.ps1, make_kit_hookshot.ps1's Copy-KitPack and
# Test-KitPackData): the tool is staged as the kit stages it, then run from that copy alone (kit_pack_check.py, python -I):
# every module build.py imports, every path a module names in the kit, and the data each module reads. 0.11.12's
# integrate-01110 db12ad12 kit copied charsel_ui.py and coop_menu.py but not charsel\data and coopmenu\data, and the pack
# stopped on the players' PCs (FileNotFoundError charsel/data/manifest.json); CI built only the Linux kit.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\kit_pack_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$release = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $release)
. (Join-Path $release 'kit_text.ps1')
. (Join-Path $release 'kit_pack.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { Write-Host 'FAIL no python on PATH (CI: windows-latest has one)'; exit 1 }
$check = Join-Path $PSScriptRoot 'kit_pack_check.py'
# The staged tool run on its own: its exit code (0 all there).
function Invoke-Check([string]$Kit) {
    $ErrorActionPreference = 'Continue'   # (PowerShell 5.1: a native command's stderr line under Stop throws)
    $out = & $python -I $check $Kit 2>&1
    foreach ($l in $out) { Write-Host "     $l" }
    return $LASTEXITCODE
}
function New-Kit {
    $kit = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-kitpack-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $kit | Out-Null
    $mods = @(Copy-KitPack $repo $kit 27510)
    return [pscustomobject]@{ Kit = $kit; Mods = $mods; Tool = (Join-Path $kit 'tools\puppet_pack') }
}

$made = @()
try {
    # 1. The kit as make_kit_hookshot.ps1 stages it: the whole tool runs from it.
    $k = New-Kit; $made += $k
    Check (($k.Mods -contains 'charsel_ui.py') -and ($k.Mods -contains 'coop_menu.py') -and ($k.Mods -contains 'ui_pack.py')) 'the import walk takes charsel_ui.py, coop_menu.py and ui_pack.py'
    $miss = @(Test-KitPackData $repo $k.Kit)
    Check ($miss.Count -eq 0) "every pack data file and strings file in the kit ($($miss -join ', '))"
    Check ((Invoke-Check $k.Kit) -eq 0) 'the staged tool imports, and finds every file it names, from the kit alone'

    # 2. The db12ad12 kit: the modules without their data folders.
    $k = New-Kit; $made += $k
    Remove-Item -LiteralPath (Join-Path $k.Tool 'charsel'), (Join-Path $k.Tool 'coopmenu') -Recurse -Force
    $miss = @(Test-KitPackData $repo $k.Kit)
    Check (($miss -contains 'charsel\data\manifest.json') -and ($miss -contains 'coopmenu\data\map.cdelta')) 'no data folders: the self-check names them'
    Check ((Invoke-Check $k.Kit) -ne 0) 'no data folders: the staged tool fails'

    # 3. One delta missing, one changed.
    $k = New-Kit; $made += $k
    Remove-Item -LiteralPath (Join-Path $k.Tool 'coopmenu\data\map.cdelta') -Force
    [System.IO.File]::AppendAllText((Join-Path $k.Tool 'charsel\data\p00.cdelta'), 'x')
    $miss = @(Test-KitPackData $repo $k.Kit)
    Check (($miss -contains 'coopmenu\data\map.cdelta') -and ($miss -contains 'charsel\data\p00.cdelta')) 'a missing and a changed data file: both named'
    Check ((Invoke-Check $k.Kit) -ne 0) 'a missing and a changed data file: the staged tool fails'

    # 4. A module or the strings missing.
    $k = New-Kit; $made += $k
    Remove-Item -LiteralPath (Join-Path $k.Tool 'msbt.py') -Force
    Check ((Invoke-Check $k.Kit) -ne 0) 'a module build.py needs (msbt.py, via ui_pack.py) missing: the staged tool fails'
    $k = New-Kit; $made += $k
    Remove-Item -LiteralPath (Join-Path $k.Tool 'strings') -Recurse -Force
    Check ((@(Test-KitPackData $repo $k.Kit) -contains 'strings\en.txt')) 'no strings: the self-check names en.txt'
    Check ((Invoke-Check $k.Kit) -ne 0) 'no strings: the staged tool fails'
} finally {
    foreach ($k in $made) { Remove-Item -LiteralPath $k.Kit -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
