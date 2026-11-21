# Tests for what a kit start does again after a mod change (player report 210, kit-paths.ps1): the game's builds kept per
# stamp (Save-KitGameVariant, Find-KitGameVariant, Restore-KitGameVariant, the 'variant' need), the puppet packs kept per
# characters (Save-KitPuppetPack, Restore-KitPuppetPack), the randomizer seeds' size tables by content
# (Get-KitStaleRandoSeeds, Get-KitSeedTablesText) and the line saying why a build runs (Get-KitBuildWhyText).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\mod_restart_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts'
. (Join-Path $scripts 'kit-paths.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Write-File([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text)
}
function Read-File([string]$Path) { if (Test-Path -LiteralPath $Path) { [System.IO.File]::ReadAllText($Path) } else { $null } }

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-mod-restart-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    # --- The game's builds, kept per stamp -----------------------------------------------------------------
    $kit = Join-Path $tmp 'kit'
    Write-File (Join-Path $kit 'hookshot\PIN') "runtime=Longshot`ncommit=e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8`n"
    Write-File (Join-Path $kit 'hookshot\TOOLS.txt') "release=v3.0.0`n"
    Write-File (Join-Path $kit 'hookshot\patches\0001-a.patch') "one`n"
    Write-File (Join-Path $kit 'hookshot\hookshot_coop.dll') 'MODULE'
    $g = Join-Path $kit 'hookshot\game'
    $plain = Get-KitHookshotStamp $kit
    # A build without code mods: what Longshot's finish copies in, and its note of them.
    function Set-Build([string]$Tag, [string]$Stamp, [bool]$Patches) {
        foreach ($f in 'longshot.exe', 'longshot_vk.dll', 'SDL2.dll', 'SDL3.dll', 'image.bin') { Write-File (Join-Path $g $f) "$f $Tag" }
        Write-File (Join-Path $g '.longshot_installed.json') '{"SDL2.dll": {}, "SDL3.dll": {}, "image.bin": {}, "longshot.exe": {}, "longshot_vk.dll": {}}'
        if ($Patches) { Write-File (Join-Path $g 'exefs_patches.txt') "patches $Tag" } else { Remove-Item -LiteralPath (Join-Path $g 'exefs_patches.txt') -Force -ErrorAction SilentlyContinue }
        Write-File (Join-Path $g 'coop-built-from.txt') $Stamp
    }
    Set-Build 'plain' $plain $false
    Write-File (Join-Path $g 'hookshot_coop.dll') 'MODULE'
    Write-File (Join-Path $g 'user\save\slot0.bin') 'MY SAVE'
    Write-File (Join-Path $g 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json') '{"sound": true, "tunics": {"red": ["x"]}}'
    Write-File (Join-Path $g 'mods\contents\0400486024DEC000\coop-puppet\romfs\x') 'tunic'
    Check ((Get-KitHookshotNeed $kit) -eq '') 'need: the game as built -> nothing'
    Check (@(Get-KitGameVariantFiles $g).Count -eq 6 -and (@(Get-KitGameVariantFiles $g) -contains '.longshot_installed.json')) 'variant files: the five Longshot noted and its note (no exefs_patches.txt without code mods)'

    # Character Select's code patch put in (on): the need is a build, no kept build of that stamp yet.
    Write-File (Join-Path $g 'mods\OoT Character Select\exefs\E0F3CCFAF50065AE.ips') 'PATCHEOF'
    $withCs = Get-KitHookshotStamp $kit
    Check ((Get-KitHookshotNeed $kit) -eq 'mods') 'need: a code mod put in, nothing kept -> mods (a build)'
    Check ((Get-KitBuildWhyText $plain $withCs) -like "the mods' code patches changed*switching back later takes seconds.") 'why: the code mods changed, with how long and that it is kept'
    # The build script keeps the build there before it builds (the plain one).
    $line = Save-KitGameVariant $kit $g
    Check ($line -like '*kept*seconds*') "save: the plain build kept ($line)"
    $plainDir = Find-KitGameVariant $kit $plain $false
    Check ([bool]$plainDir) 'find: the plain build is kept'
    Check (-not (Find-KitGameVariant $kit $plain $true)) 'find: not for a DLSS game (kept apart)'
    Check (-not (Find-KitGameVariant $kit $withCs $false)) 'find: nothing kept with Character Select yet'
    Check ((Save-KitGameVariant $kit $g) -eq '') 'save: the same build again -> kept already, nothing copied'
    # The build with Character Select.
    Set-Build 'cs' $withCs $true
    Check ((Get-KitHookshotNeed $kit) -eq '') 'need: built with Character Select -> nothing'

    # Switched off on the Mods tab: the plain build is kept -> 'variant' (no build), and it is put back.
    Write-File (Join-Path $g 'mods\coop-mods.list') "coop-mods 1`noff OoT Character Select`n"
    $off = Get-KitHookshotStamp $kit
    Check ($off -ne $withCs -and $off -ne $plain) 'stamp: switched off on the Mods tab is a stamp of its own'
    # (the plain build was without the mod in the folder; switched off is another stamp: built once, then kept)
    Check ((Get-KitHookshotNeed $kit) -eq 'mods') 'need: switched off for the first time -> mods (a build)'
    [void](Save-KitGameVariant $kit $g)
    Set-Build 'off' $off $false
    Write-File (Join-Path $g 'mods\coop-mods.list') "coop-mods 1`non OoT Character Select`n"
    $on = Get-KitHookshotStamp $kit
    # (switched off and on again on the Mods tab leaves an "on" row: the game reads it as no list, so the stamp is the one
    # built with Character Select, and that build is kept -> 'variant', no build)
    Check ($on -eq $withCs) 'stamp: an "on" row for the only code mod is the stamp without a list (no change for the game)'
    Check ((Get-KitHookshotNeed $kit) -eq 'variant') 'need: Character Select switched off and on again -> variant (the build from before, no build)'
    Write-File (Join-Path $g 'mods\Second Code\exefs\E0F3CCFAF50065AE.ips') 'PATCH2EOF'
    Write-File (Join-Path $g 'mods\coop-mods.list') "coop-mods 1`non Second Code`non OoT Character Select`n"
    $moved = Get-KitHookshotStamp $kit
    Write-File (Join-Path $g 'mods\coop-mods.list') "coop-mods 1`non OoT Character Select`non Second Code`n"
    Check ($moved -ne (Get-KitHookshotStamp $kit)) 'stamp: two code mods in another order than by name -> a stamp of its own (the order decides a byte both write)'
    Remove-Item -LiteralPath (Join-Path $g 'mods\coop-mods.list') -Force
    Check ((Get-KitHookshotStamp $kit) -like '*;list=none') 'stamp: two code mods on, by name, no list -> list=none'
    Write-File (Join-Path $g 'mods\coop-mods.list') "coop-mods 1`non OoT Character Select`non Second Code`n"
    Check ((Get-KitHookshotStamp $kit) -like '*;list=none') 'stamp: the same two "on" in the order by name -> list=none (no change)'
    Remove-Item -LiteralPath (Join-Path $g 'mods\Second Code') -Recurse -Force
    Write-File (Join-Path $g 'mods\coop-mods.list') "coop-mods 1`non OoT Character Select`n"
    # (the list removed too: the same)
    Remove-Item -LiteralPath (Join-Path $g 'mods\coop-mods.list') -Force
    Check ((Get-KitHookshotStamp $kit) -eq $withCs) 'stamp: back to the folder as it was built with Character Select'
    Check ((Get-KitHookshotNeed $kit) -eq 'variant') 'need: back to a stamp kept from before -> variant (no build)'
    Check ([bool](Get-KitHookshotNeedText 'variant')) 'need text: variant'
    [void](Save-KitGameVariant $kit $g)
    $csDir = Find-KitGameVariant $kit $withCs $false
    Check ((Restore-KitGameVariant $csDir $g)) 'restore: the Character Select build put back'
    Check ((Read-File (Join-Path $g 'longshot.exe')) -eq 'longshot.exe cs' -and (Read-File (Join-Path $g 'image.bin')) -eq 'image.bin cs') 'restore: its exe and image.bin'
    Check ((Read-File (Join-Path $g 'exefs_patches.txt')) -eq 'patches cs') 'restore: its exefs_patches.txt (the Mods tab reads it)'
    Check ((Read-File (Join-Path $g 'coop-built-from.txt')) -eq $withCs) 'restore: its stamp, as the build wrote it'
    Check ((Read-File (Join-Path $g '.longshot_installed.json')) -like '*longshot_vk.dll*') 'restore: Longshot''s note comes back (its file list for the next keep)'
    Check ((Read-File (Join-Path $g 'user\save\slot0.bin')) -eq 'MY SAVE' -and (Read-File (Join-Path $g 'hookshot_coop.dll')) -eq 'MODULE') 'restore: saves and the module untouched'
    Check (-not @(Get-ChildItem -LiteralPath $g -Recurse -Filter '*.new' -File)) 'restore: no .new file left'
    Check ((Get-KitHookshotNeed $kit) -eq '') 'need: after the put back -> nothing'
    # The plain build back: exefs_patches.txt goes (that build had none).
    Check ((Restore-KitGameVariant $plainDir $g) -and -not (Test-Path -LiteralPath (Join-Path $g 'exefs_patches.txt'))) 'restore: a build without code patches takes exefs_patches.txt away'
    Check ((Read-File (Join-Path $g 'longshot.exe')) -eq 'longshot.exe plain') 'restore: the plain exe'
    # A kept copy that stopped half way (.part) or lost its files is never taken.
    $half = Join-Path (Get-KitGameVariantRoot $kit) 'aaaaaaaaaaaaaaaaaaaa.part'
    Write-File (Join-Path $half 'stamp.txt') (ConvertTo-KitVariantStamp 'release=x' $false)
    Check (-not (Find-KitGameVariant $kit 'release=x' $false)) 'find: a .part folder is not taken'
    Remove-Item -LiteralPath (Join-Path $csDir 'files\image.bin') -Force
    Check (-not (Find-KitGameVariant $kit $withCs $false)) 'find: a kept build without its image.bin is not taken'
    # Only the newest three stay.
    foreach ($i in 1..4) {
        Set-Build "n$i" "release=v3.0.0`ncommit=e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8`npatches=$i" $false
        [void](Save-KitGameVariant $kit $g)
        Start-Sleep -Milliseconds 30
    }
    $left = @(Get-ChildItem -LiteralPath (Get-KitGameVariantRoot $kit) -Directory)
    Check ($left.Count -eq 3 -and -not ($left | Where-Object { $_.Name -like '*.part' })) "keep: the newest 3 builds stay, no .part ($($left.Count))"
    Check ([bool](Find-KitGameVariant $kit "release=v3.0.0`ncommit=e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8`npatches=4" $false) -and
        -not (Find-KitGameVariant $kit "release=v3.0.0`ncommit=e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8`npatches=1" $false)) 'keep: the oldest goes'
    # The other reasons a build runs, in its one line.
    Check ((Get-KitBuildWhyText '' $plain) -like 'the first build*') 'why: the first build'
    Check ((Get-KitBuildWhyText "release=v2`ncommit=x`npatches=1" $plain) -like 'another Longshot version*') 'why: another Longshot version'
    Check ((Get-KitBuildWhyText "release=v3.0.0`ncommit=e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8`npatches=old" $plain) -like 'this update changes*') 'why: an update of the patches'
    # The build script keeps and puts back before its game cache and its build, and says why a build runs.
    $hb = [System.IO.File]::ReadAllText((Join-Path $scripts 'hookshot-build.ps1'))
    Check ($hb.IndexOf('Save-KitGameVariant $kit $Out') -gt 0 -and $hb.IndexOf('Save-KitGameVariant $kit $Out') -lt $hb.IndexOf('Find-KitHookshotCachedGame $gameWant')) 'build script: keeps the build before the game cache and the build'
    Check ($hb -match 'Step "Building the game: \$\(Get-KitBuildWhyText \$stampWas \$gameWant\)') 'build script: the build step says why and how long'

    # --- Puppet packs, kept per characters ---------------------------------------------------------------
    $tools = Join-Path $tmp 'tools'
    Write-File (Join-Path $tools 'build.py') 'print(1)'
    $toolHash = Get-PuppetToolHash $tools
    $mod = Join-Path $g 'mods\contents\0400486024DEC000\coop-puppet'
    $manifest = Join-Path $mod 'coop_puppet.json'
    function Set-Pack([string]$Tag, [string]$Characters) {
        if (Test-Path -LiteralPath $mod) { Remove-Item -LiteralPath $mod -Recurse -Force }
        Write-File $manifest ('{"sound": true, "tunics": {"red": ["x"]}, "look_files": {"gear": "coop_look/gear.bin"}, "tool_sha256": "' + $toolHash + '"' + $Characters + '}')
        Write-File (Join-Path $mod 'coop_look\gear.bin') "gear $Tag"
        Write-File (Join-Path $mod 'romfs\x') "tunic $Tag"
        Write-File (Join-Path $mod 'romfs\System\Resource\ResourceSizeTable.Product.rsizetable.zs') "table $Tag"
    }
    Remove-Item -LiteralPath (Join-Path $g 'mods\OoT Character Select') -Recurse -Force
    Set-Pack 'plain' ''
    Check (-not (Test-KitPuppetPackStale $manifest $tools)) 'pack: made for this tool and no Character Select -> current (not made again)'
    Check ((Restore-KitPuppetPack $kit $mod $tools) -eq '') 'pack: none kept yet -> nothing put back'
    # A pack made for other characters (Character Select on) is there; the plain one was kept when it was made.
    $keptPlain = Save-KitPuppetPack $kit $mod
    Check ([bool]$keptPlain -and -not (Test-Path -LiteralPath $mod)) 'pack: kept (moved out) before it is made again'
    Set-Pack 'cs' ', "characters": {"mod": "OoT Character Select", "numbers": {"1": 1}}'
    Check ([bool](Test-KitPuppetPackStale $manifest $tools)) 'pack: made with Character Select, the mod not there -> stale'
    $said = Restore-KitPuppetPack $kit $mod $tools
    Check ($said -like '*put back*') "pack: the plain pack put back instead of made ($said)"
    Check ((Read-File (Join-Path $mod 'coop_look\gear.bin')) -eq 'gear plain' -and -not (Test-KitPuppetPackStale $manifest $tools)) 'pack: it is the plain one, current'
    $table = Get-Item -LiteralPath (Join-Path $mod 'romfs\System\Resource\ResourceSizeTable.Product.rsizetable.zs')
    Check ($table.LastWriteTimeUtc -gt [DateTime]::UtcNow.AddMinutes(-1)) 'pack: its size table dated now (seeds without a content stamp are built again)'
    $cs = @(Get-ChildItem -LiteralPath (Get-KitPackKeepRoot $kit) -Directory)
    Check ($cs.Count -eq 1 -and (Read-File (Join-Path $cs[0].FullName 'coop-puppet\coop_look\gear.bin')) -eq 'gear cs') 'pack: the Character Select pack kept in its place (switching back is seconds too)'
    # Another pack tool: nothing kept fits (the pack is made).
    if (Test-Path -LiteralPath $mod) { Remove-Item -LiteralPath $mod -Recurse -Force }
    [void](Save-KitPuppetPack $kit $mod)
    Set-Pack 'cs2' ', "characters": {"mod": "OoT Character Select", "numbers": {"1": 1}}'
    [void](Save-KitPuppetPack $kit $mod)
    Write-File (Join-Path $tools 'build.py') 'print(2)'
    Check ((Restore-KitPuppetPack $kit $mod $tools) -eq '') 'pack: another pack tool -> nothing put back (made again)'
    Check (@(Get-ChildItem -LiteralPath (Get-KitPackKeepRoot $kit) -Directory).Count -le 2) 'pack: at most 2 kept'
    # An incomplete pack is never kept.
    Set-Pack 'gap' ''
    Remove-Item -LiteralPath (Join-Path $mod 'coop_look\gear.bin') -Force
    Check ((Save-KitPuppetPack $kit $mod) -eq '' -and (Test-Path -LiteralPath $manifest)) 'pack: an incomplete pack is not kept'
    # The build script: the pack is made only when stale, a kept one goes back first; -PackOnly always makes it.
    Check ($hb -match 'if \(-not \$PackOnly -and -not \(Test-KitPuppetPackStale') 'build script: a current pack is not made again'
    Check ($hb -match '\$back = Restore-KitPuppetPack \$kit \$mod \$packDir') 'build script: a kept pack goes back before one is made'
    Check ($hb -match "\`$keptPack = if \(-not \`$PackOnly\) \{ Save-KitPuppetPack \`$kit \`$mod \}") 'build script: the pack there is kept before it is made again'
    Check ($hb -match "(?s)if \(\`$code -ne 0\) \{.*Move-KitFolder \(Join-Path \`$keptPack 'coop-puppet'\) \`$mod.*the one from before stays") 'build script: a failed make puts the pack from before back'
    # (what that branch does: the kept pack moved back over a half-made one)
    Set-Pack 'old' ''
    $k2 = Save-KitPuppetPack $kit $mod
    Write-File (Join-Path $mod 'half.bin') 'half made'
    Remove-Item -LiteralPath $mod -Recurse -Force
    Check ((Move-KitFolder (Join-Path $k2 'coop-puppet') $mod) -and (Read-File (Join-Path $mod 'coop_look\gear.bin')) -eq 'gear old') 'pack: the kept pack moves back in place'

    # --- Randomizer seeds: their size tables by content --------------------------------------------------
    $exe = Join-Path $tmp 'seedgame'
    $mods = Join-Path $exe 'mods'
    $other = Join-Path $mods 'contents\0400486024DEC000\coop-puppet\romfs\System\Resource\ResourceSizeTable.Product.rsizetable.zs'
    Write-File $other 'PACK TABLE 1'
    $seed = Join-Path $mods 'randomizer-abc'
    Write-File (Join-Path $seed 'coop-seed.txt') 'seed'
    Write-File (Join-Path $seed 'coop-rando.txt') 'preset'
    Write-File (Join-Path $seed 'romfs\System\Resource\ResourceSizeTable.Product.rsizetable.zs') 'SEED TABLE'
    (Get-Item -LiteralPath (Join-Path $seed 'romfs\System\Resource\ResourceSizeTable.Product.rsizetable.zs')).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-1)
    Check (@(Get-KitStaleRandoSeeds $exe) -contains 'randomizer-abc') 'seeds: no content stamp, an older seed table -> stale (the date rule, as before)'
    $text = Get-KitSeedTablesText $mods
    Check ($text -like 'contents/0400486024DEC000/coop-puppet/romfs/System/Resource/ResourceSizeTable.Product.rsizetable.zs *') "seeds: the stamp names the other tables by path and hash ($($text.Trim()))"
    Write-File (Join-Path $seed 'coop-seed-tables.txt') $text
    Check (@(Get-KitStaleRandoSeeds $exe).Count -eq 0) 'seeds: built beside these tables (content stamp) -> not stale, though older by date'
    (Get-Item -LiteralPath $other).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(5)
    Check (@(Get-KitStaleRandoSeeds $exe).Count -eq 0) 'seeds: a table written again with the same bytes -> not stale (no module step)'
    Write-File $other 'PACK TABLE 2'
    (Get-Item -LiteralPath $other).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-5)
    Check (@(Get-KitStaleRandoSeeds $exe) -contains 'randomizer-abc') 'seeds: a table changed though dated older (a pack put back) -> stale'
    Write-File (Join-Path $mods 'Other Mod\romfs\System\Resource\ResourceSizeTable.Product.rsizetable.zs') 'OTHER'
    Write-File (Join-Path $seed 'coop-seed-tables.txt') (Get-KitSeedTablesText $mods)
    Check (@(Get-KitStaleRandoSeeds $exe).Count -eq 0) 'seeds: two other tables, both as built -> not stale'
    Remove-Item -LiteralPath (Join-Path $mods 'Other Mod') -Recurse -Force
    Check (@(Get-KitStaleRandoSeeds $exe) -contains 'randomizer-abc') 'seeds: a mod with a table taken out -> stale'
    $kp = [System.IO.File]::ReadAllText((Join-Path $scripts 'kit-paths.ps1'))
    Check ($kp -match "WriteAllText\(\(Join-Path \`$seedDir 'coop-seed-tables.txt'\), \(Get-KitSeedTablesText \`$mods\)\)") 'seeds: a seed built again by the kit gets its content stamp'
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all mod restart checks passed'
