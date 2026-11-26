# Tests for the kit start's size-table step without a working Python (player report row 235):
# Invoke-KitSizeTableMerge / Move-KitMenuFilesAside in tools/release/kit/scripts/kit-paths.ps1.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\menu_park_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\kit-paths.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

$rel = 'Mals/EUen.Nin_NX_NVN.zs'
$res = 'System\Resource'
$table = 'ResourceSizeTable.Product.100.rsizetable.zs'

# A fresh exe folder: the pack (its menu file, manifest, own size table), a kit with merge_tables.py; $WithOther adds
# Character Select with a size table of the same name.
function New-Tree([bool]$WithOther) {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-menupark-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    $pack = Join-Path $root 'exe\mods\contents\0400486024DEC000\coop-puppet'
    New-Item -ItemType Directory -Force -Path (Join-Path $pack 'romfs\Mals'), (Join-Path $pack "romfs\$res"), (Join-Path $root 'kit\tools\puppet_pack') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $pack 'romfs\Mals\EUen.Nin_NX_NVN.zs'), 'menu')
    [System.IO.File]::WriteAllText((Join-Path $pack "romfs\$res\$table"), 'pack table')
    [System.IO.File]::WriteAllText((Join-Path $pack 'coop_puppet.json'), "{`"title_menu`": [`"$rel`"], `"pause_menu`": []}")
    [System.IO.File]::WriteAllText((Join-Path $root 'kit\tools\puppet_pack\merge_tables.py'), '')
    if ($WithOther) {
        $cs = Join-Path $root 'exe\mods\OoT Character Select\romfs\System\Resource'
        New-Item -ItemType Directory -Force -Path $cs | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $cs $table), 'cs table')
    }
    return [pscustomobject]@{ Root = $root; Kit = (Join-Path $root 'kit'); Exe = (Join-Path $root 'exe'); Pack = $pack }
}
function In-Play($t) { Test-Path -LiteralPath (Join-Path $t.Pack 'romfs\Mals\EUen.Nin_NX_NVN.zs') }
function Kept($t) { Test-Path -LiteralPath (Join-Path $t.Pack '_coop-menus\Mals\EUen.Nin_NX_NVN.zs') }

$made = @()
try {
    # 1. No Python, another mod has a size table: the menu file goes aside, the line says so.
    $t = New-Tree $true; $made += $t
    $out = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe)
    Check ((-not (In-Play $t)) -and (Kept $t)) 'no Python + another size table: the menu file is kept aside'
    Check (($out -join ' ') -match 'kept aside') 'the window says so'

    # 2. No Python, the pack's table is the only one: nothing moves.
    $t = New-Tree $false; $made += $t
    $out = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe)
    Check ((In-Play $t) -and -not (Kept $t)) 'no Python, pack only: the menus stay'

    # 3. A "Python" that dies before main (a Traceback, exit 1): same as 1.
    $fake = Join-Path $env:TEMP ('oot-coop-fakepy-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.cmd')
    [System.IO.File]::WriteAllText($fake, "@echo off`r`necho Traceback (most recent call last):`r`necho ImportError: nope`r`nexit /b 1`r`n")
    $t = New-Tree $true; $made += $t
    $out = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe $fake)
    Check ((-not (In-Play $t)) -and (Kept $t)) 'Python dies before main: the menu file is kept aside'
    $t = New-Tree $false; $made += $t
    $null = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe $fake)
    Check ((In-Play $t) -and -not (Kept $t)) 'Python dies before main, pack only: the menus stay'

    # 4. A "Python" that says its own Menus line and exits 1 (the merge ran and reported): nothing more is moved here.
    $fake2 = Join-Path $env:TEMP ('oot-coop-fakepy2-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.cmd')
    [System.IO.File]::WriteAllText($fake2, "@echo off`r`necho Menus: said by merge_tables.py`r`nexit /b 1`r`n")
    $t = New-Tree $true; $made += $t
    $null = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe $fake2)
    Check (In-Play $t) 'Python that reported its own Menus line: the kit parks nothing on top'
    Remove-Item -LiteralPath $fake, $fake2 -Force -ErrorAction SilentlyContinue

    # 5. charsel-native-0119: the pack names its lists in "menu_keys" (puppet_pack ui_pack.MENU_KEYS); co-op's own
    #    Characters tab's files (charsel_native: the screens table) go aside with the menus, all together.
    $scr = 'RSDB/UIScreen.Product.111.rstbl.byml.zs'
    $t = New-Tree $true; $made += $t
    New-Item -ItemType Directory -Force -Path (Join-Path $t.Pack 'romfs\RSDB') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $t.Pack 'romfs\RSDB\UIScreen.Product.111.rstbl.byml.zs'), 'screens')
    [System.IO.File]::WriteAllText((Join-Path $t.Pack 'coop_puppet.json'),
        "{`"title_menu`": [`"$rel`"], `"pause_menu`": [], `"charsel_native`": [`"$scr`"], `"menu_keys`": [`"title_menu`", `"pause_menu`", `"charsel_native`"]}")
    $null = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe)
    Check ((-not (In-Play $t)) -and (Kept $t) -and -not (Test-Path -LiteralPath (Join-Path $t.Pack 'romfs\RSDB\UIScreen.Product.111.rstbl.byml.zs')) -and
           (Test-Path -LiteralPath (Join-Path $t.Pack '_coop-menus\RSDB\UIScreen.Product.111.rstbl.byml.zs'))) 'menu_keys with charsel_native: the Characters tab''s files go aside with the menus'
    # 6. A pack without "menu_keys" (made before it): the two old lists only.
    $t = New-Tree $true; $made += $t
    New-Item -ItemType Directory -Force -Path (Join-Path $t.Pack 'romfs\RSDB') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $t.Pack 'romfs\RSDB\UIScreen.Product.111.rstbl.byml.zs'), 'screens')
    [System.IO.File]::WriteAllText((Join-Path $t.Pack 'coop_puppet.json'), "{`"title_menu`": [`"$rel`"], `"pause_menu`": [], `"charsel_native`": [`"$scr`"]}")
    $null = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe)
    Check ((Kept $t) -and (Test-Path -LiteralPath (Join-Path $t.Pack 'romfs\RSDB\UIScreen.Product.111.rstbl.byml.zs'))) 'no menu_keys (an old pack): only title_menu and pause_menu go aside'
    # 7. MsgNotFound on slot 0 (P-parked): the Characters tab's code patch goes with its files. Parked -> the code
    #    out (Sync-KitCharselNativePatch 'out', so the stamp changes and the game is built without it); put back ->
    #    in again. Never the tab's code over the game's own menu files.
    $t = New-Tree $true; $made += $t
    $ips = Join-Path $t.Kit 'tools\puppet_pack\charsel\data\charsel-native.ips'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ips) | Out-Null
    [System.IO.File]::WriteAllText($ips, 'IPS32 code')
    $code = Join-Path $t.Pack 'exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips'
    $screens = Join-Path $t.Pack 'romfs\RSDB\UIScreen.Product.111.rstbl.byml.zs'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $screens) | Out-Null
    [System.IO.File]::WriteAllText($screens, 'screens')
    [System.IO.File]::WriteAllText((Join-Path $t.Pack 'coop_puppet.json'),
        "{`"title_menu`": [`"$rel`"], `"pause_menu`": [], `"charsel_native`": [`"$scr`"], `"menu_keys`": [`"title_menu`", `"pause_menu`", `"charsel_native`"]}")
    Check (-not (Test-KitCharselNativeParked $t.Pack)) 'the tab''s files in romfs: not parked'
    Check ((Sync-KitCharselNativePatch $t.Kit $t.Pack) -eq 'in' -and (Test-Path -LiteralPath $code)) 'not parked: the tab''s code goes in'
    $null = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe)   # (no Python + another size table: parked)
    Check (Test-KitCharselNativeParked $t.Pack) 'the merge parked the tab''s files: parked'
    Check ((Sync-KitCharselNativePatch $t.Kit $t.Pack) -eq 'out' -and -not (Test-Path -LiteralPath $code)) 'parked: the tab''s code goes out with them'
    Check ((Sync-KitCharselNativePatch $t.Kit $t.Pack) -eq '') 'parked, code out: no change (no build loop)'
    # (put back, as merge_tables.py guard_menus does when the table holds them again)
    Move-Item -LiteralPath (Join-Path $t.Pack '_coop-menus\RSDB\UIScreen.Product.111.rstbl.byml.zs') -Destination $screens
    Check (-not (Test-KitCharselNativeParked $t.Pack)) 'put back: not parked'
    Check ((Sync-KitCharselNativePatch $t.Kit $t.Pack) -eq 'in' -and (Test-Path -LiteralPath $code)) 'put back: the tab''s code goes in again'
    # A pack made since (its file in romfs) beside an old kept copy: in play, the code stays.
    New-Item -ItemType Directory -Force -Path (Join-Path $t.Pack '_coop-menus\RSDB') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $t.Pack '_coop-menus\RSDB\UIScreen.Product.111.rstbl.byml.zs'), 'old screens')
    Check (-not (Test-KitCharselNativeParked $t.Pack) -and (Sync-KitCharselNativePatch $t.Kit $t.Pack) -eq '') 'an old kept copy beside the file in romfs: not parked, the code stays'
    # 8. Row 265 (review-01112-g): play.ps1's Hookshot starts run the size-table merge AFTER the self-tunic step
    #    (Use-KitPuppetPack puts your tunic tile in; merge_self_models renames other mods' Link models to it) and
    #    before the game starts, every time: never skipped because the need-check loop merged earlier.
    $lines = @([System.IO.File]::ReadAllLines((Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\play.ps1')))
    $starts = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch 'Use-KitPuppetPack .*coop-puppet\\coop_puppet\.json') { continue }
        $end = $i + 1
        while ($end -lt $lines.Count -and $lines[$end] -notmatch '^\s*\$game = Start-Process') { $end++ }
        $merge = @($lines[$i..$end] | Where-Object { $_ -match '^\s*foreach \(\$l in @\(Invoke-KitSizeTableMerge \$kit \$emu\)\) \{ Write-Host \$l \}' })
        if ($lines[$i] -match "Join-Path \`$emu") {
            $starts++
            Check ($merge.Count -eq 1 -and $end -lt $lines.Count) "play.ps1 Hookshot start (line $($i + 1)): the size-table merge runs, unconditionally, after the self-tunic step and before the game"
        }
    }
    Check ($starts -eq 2) "play.ps1: both Hookshot starts (title and play) checked ($starts)"
    Check (-not (Select-String -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\play.ps1') -SimpleMatch 'mergeFresh' -Quiet)) 'play.ps1: no flag skips the start''s merge'
} finally {
    foreach ($t in $made) { Remove-Item -LiteralPath $t.Root -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
