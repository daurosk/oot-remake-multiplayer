# Tests for the kit's guard against a game made of two builds (kit-paths.ps1, play.ps1, hookshot-build.ps1):
# the build files (longshot.exe, image.bin, longshot_vk.dll, the SDL files) against the sha1s in Longshot's own note
# (.longshot_installed.json), and the test kit's marker hookshot\hold-game.txt (patches and pin of the kit against the
# stamp of its game). A mixed set is never started, never kept as a variant, never copied, never built on.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\kit_build_files_test.ps1
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
function Get-Sha1([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA1).Hash.ToLowerInvariant() }

$commit = 'e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-build-files-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    $kit = Join-Path $tmp 'kit'
    Write-File (Join-Path $kit 'hookshot\PIN') "repo=https://git.magicbean.dev/MagicBeanDev/Longshot`ncommit=$commit`n"
    Write-File (Join-Path $kit 'hookshot\TOOLS.txt') "release=v3.0.0`nlock_sha256=aa`nsource_commit=$commit`nsource_sha256=bb`n"
    Write-File (Join-Path $kit 'hookshot\patches\0001-a.patch') "one`n"
    Write-File (Join-Path $kit 'hookshot\hookshot_coop.dll') 'MODULE'
    $g = Join-Path $kit 'hookshot\game'
    $stamp = Get-KitHookshotStamp $kit
    $files = 'longshot.exe', 'longshot_vk.dll', 'SDL2.dll', 'SDL3.dll', 'image.bin'

    # A build as Longshot's finish leaves it: the files and its note with their real sha1s.
    function Set-Build([string]$Root, [string]$Tag, [string]$Stamp) {
        foreach ($f in $files) { Write-File (Join-Path $Root $f) "$f $Tag" }
        $rows = $files | ForEach-Object { '"' + $_ + '": {"sha1": "' + (Get-Sha1 (Join-Path $Root $_)) + '", "stat": [1, 2]}' }
        Write-File (Join-Path $Root '.longshot_installed.json') ('{' + ($rows -join ', ') + '}')
        if ($Stamp) { Write-File (Join-Path $Root 'coop-built-from.txt') $Stamp }
        # (a game with its puppet pack and module in, as the start leaves it: the need is then about the build files only)
        if ($Stamp) {
            Write-File (Join-Path $Root 'hookshot_coop.dll') 'MODULE'
            Write-File (Join-Path $Root 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json') '{"sound": true, "tunics": {"red": ["x"]}}'
            Write-File (Join-Path $Root 'mods\contents\0400486024DEC000\coop-puppet\romfs\x') 'tunic'
        }
    }
    Set-Build $g 'one' $stamp

    # --- A clean set ---------------------------------------------------------------------------------------
    Check (@(Get-KitGameBuildMismatch $g).Count -eq 0) 'mismatch: a clean set -> none'
    Check ((Get-KitHookshotNeed $kit) -eq '') 'need: a clean set -> nothing'
    $line = Save-KitGameVariant $kit $g
    Check ($line -like '*kept*') "save: a clean set is kept ($line)"
    $dir = Find-KitGameVariant $kit $stamp $false
    Check ([bool]$dir) 'find: the clean set is found'
    Check ((Restore-KitGameVariant $dir $g)) 'restore: the clean set goes back'
    Check ((Get-KitHookshotNeed $kit) -eq '') 'need: after the put back -> nothing'

    # --- One file of another build, for each build file ----------------------------------------------------
    foreach ($f in 'image.bin', 'longshot_vk.dll', 'longshot.exe') {
        $keep = Read-File (Join-Path $g $f)
        Write-File (Join-Path $g $f) "$f FROM ANOTHER BUILD"
        $bad = @(Get-KitGameBuildMismatch $g)
        Check ($bad.Count -eq 1 -and $bad[0] -like "$f *not the file Longshot put into this game*") "mismatch: $f swapped -> one line that names it"
        Check ((Get-KitHookshotNeed $kit) -eq 'mixed') "need: $f swapped -> mixed (never started)"
        $text = Get-KitHookshotNeedText 'mixed' $g $true $kit
        Check ($text -like "*$f*" -and $text -like '*Nothing was changed*' -and $text -like '*Start Squadala.bat asks*' -and $text -notlike '*Build-Hookshot-Coop.bat*') "need text (Longshot-only kit): names $f, says nothing was changed, the start asks, no Build-Hookshot-Coop.bat"
        $text = Get-KitHookshotNeedText 'mixed' $g $false $kit
        Check ($text -like '*Build-Hookshot-Coop.bat -Rebuild*') 'need text (full kit): names Build-Hookshot-Coop.bat -Rebuild'
        Check ((Get-KitHookshotNeedQuestion 'mixed') -like '*again*') 'need question: mixed asks whether to build the game again'
        Check ((Save-KitGameVariant $kit $g) -like '*not kept*') "save: a set with $f swapped is not kept as a variant"
        Write-File (Join-Path $g $f) $keep
    }
    Check ((Get-KitHookshotNeed $kit) -eq '') 'need: the files put back -> nothing'
    Remove-Item -LiteralPath (Join-Path $g 'image.bin') -Force
    $bad = @(Get-KitGameBuildMismatch $g)
    Check ($bad.Count -eq 1 -and $bad[0] -like 'image.bin is missing*' -and (Get-KitHookshotNeed $kit) -eq 'mixed') 'mismatch: a file the note lists is missing -> mixed'
    Write-File (Join-Path $g 'image.bin') 'image.bin one'
    Check ((Get-KitHookshotNeed $kit) -eq '') 'need: the file back -> nothing'

    # --- A variant that is mixed (an old build kept from mixed files) ---------------------------------------
    $mixedRoot = Join-Path $tmp 'mixedvariant'
    Set-Build $mixedRoot 'two' $null
    Write-File (Join-Path $mixedRoot 'image.bin') 'image.bin from another build'
    $oldStamp = "release=v3.0.0`ncommit=$commit`npatches=old"
    $badDir = Join-Path (Get-KitGameVariantRoot $kit) 'bbbbbbbbbbbbbbbbbbbb'
    New-Item -ItemType Directory -Force -Path $badDir | Out-Null
    Copy-Item -LiteralPath $mixedRoot -Destination (Join-Path $badDir 'files') -Recurse
    Write-File (Join-Path $badDir 'stamp.txt') (ConvertTo-KitVariantStamp $oldStamp $false)
    Write-File (Join-Path $badDir 'built-from.txt') $oldStamp
    Check (-not (Find-KitGameVariant $kit $oldStamp $false)) 'find: a kept build whose files are of two builds is never taken'
    Check (-not (Restore-KitGameVariant $badDir $g)) 'restore: a kept build of two builds is refused'
    Check ((Read-File (Join-Path $g 'image.bin')) -eq 'image.bin one') 'restore: the game was not touched by the refusal'

    # --- Nothing to compare: no refusal ----------------------------------------------------------------------
    $fresh = Join-Path $tmp 'fresh'
    foreach ($f in $files) { Write-File (Join-Path $fresh $f) "$f x" }
    Check (@(Get-KitGameBuildMismatch $fresh).Count -eq 0) 'mismatch: a game without Longshot''s note (a fresh install) -> no refusal'
    Write-File (Join-Path $fresh '.longshot_installed.json') '{"image.bin": {}, "longshot.exe": {"stat": [1]}}'
    Check (@(Get-KitGameBuildMismatch $fresh).Count -eq 0) 'mismatch: a note without sha1s (an older Longshot) -> no refusal'
    Write-File (Join-Path $fresh '.longshot_installed.json') 'not json {'
    Check (@(Get-KitGameBuildMismatch $fresh).Count -eq 0) 'mismatch: a note that does not parse -> no refusal'
    Write-File (Join-Path $fresh '.longshot_installed.json') ('{"..\\x.bin": {"sha1": "' + ('a' * 40) + '"}, "C:\\x.bin": {"sha1": "' + ('a' * 40) + '"}}')
    Check (@(Get-KitGameBuildMismatch $fresh).Count -eq 0) 'mismatch: a note naming a path outside the game is not followed'
    Check (@(Get-KitGameBuildMismatch (Join-Path $tmp 'no-such-folder')).Count -eq 0) 'mismatch: no folder -> no refusal'

    # --- hookshot\hold-game.txt: a test kit never builds its game for other patches or pin -------------------
    Write-File (Join-Path $kit 'hookshot\patches\0002-b.patch') "two`n"
    $newStamp = Get-KitHookshotStamp $kit
    Check ($newStamp -ne $stamp -and (Get-KitHookshotNeed $kit) -eq 'patches') 'hold: without the marker, other patches -> patches (a build, as before)'
    Check ((Get-KitHoldGameText $kit $g) -eq '') 'hold: without the marker, no text'
    Write-File (Get-KitHoldGameFile $kit) "test kit`n"
    Check ((Get-KitHookshotNeed $kit) -eq 'held') 'hold: with the marker, other patches -> held (never built)'
    $text = Get-KitHookshotNeedText 'held' $g $true $kit
    Check ($text -like '*patches*' -and $text -like '*hold-game.txt*' -and $text -like '*Nothing was changed*' -and $text -like '*delete hookshot\hold-game.txt*' -and $text -like '*copy the kit''s hookshot\patches*') 'hold: the text names what differs and both ways to go on'
    Check ((Get-KitHookshotNeedQuestion 'held') -eq '') 'hold: no question'
    Remove-Item -LiteralPath (Join-Path $kit 'hookshot\patches\0002-b.patch') -Force
    Check ((Get-KitHookshotNeed $kit) -eq '' -and (Get-KitHoldGameText $kit $g) -eq '') 'hold: with the marker and the patches the game was built from -> nothing'
    Write-File (Join-Path $kit 'hookshot\PIN') "repo=https://git.magicbean.dev/MagicBeanDev/Longshot`ncommit=$('f' * 40)`n"
    $text = Get-KitHoldGameText $kit $g
    Check ($text -like '*commit*' -and (Get-KitHookshotNeed $kit) -eq 'held') 'hold: another pin -> held, the text names the commit'
    Write-File (Join-Path $kit 'hookshot\PIN') "repo=https://git.magicbean.dev/MagicBeanDev/Longshot`ncommit=$commit`n"
    Remove-Item -LiteralPath (Get-KitHoldGameFile $kit) -Force
    Remove-Item -LiteralPath $g -Recurse -Force
    Write-File (Get-KitHoldGameFile $kit) "test kit`n"
    Check ((Get-KitHoldGameText $kit $g) -eq '' -and (Get-KitHookshotNeed $kit) -ne 'held') 'hold: with the marker but no game yet -> the first build is not held'
    Remove-Item -LiteralPath (Get-KitHoldGameFile $kit) -Force

    # --- play.ps1 and hookshot-build.ps1 --------------------------------------------------------------------
    $play = [System.IO.File]::ReadAllText((Join-Path $scripts 'play.ps1'))
    $i = $play.IndexOf("`$need -eq 'held'")
    Check ($i -gt 0 -and $play.IndexOf('exit 1', $i) -gt $i -and $play.IndexOf('exit 1', $i) -lt $play.IndexOf('hookshot-build.ps1', $i)) 'play.ps1: a held kit stops before any build or start'
    # A mixed game: the start asks (the question is not empty, see the need question check), builds with -Rebuild on yes,
    # and a no or a failed build never reaches the game start (Restore-KitCharacterSelectStart, which goes on to start, is skipped).
    Check ($play -match "\`$buildArgs = if \(\`$need -eq 'mixed'\) \{ @\('-Rebuild'\) \}" -and $play -match "hookshot-build\.ps1'\) @buildArgs") 'play.ps1: a mixed game is built again with -Rebuild when the player says yes'
    Check (([regex]::Matches($play, "\`$need -ne 'mixed' -and \(Restore-KitCharacterSelectStart\)")).Count -eq 2) 'play.ps1: a no or a failed build on a mixed game never goes on to start the game'

    $hb = [System.IO.File]::ReadAllText((Join-Path $scripts 'hookshot-build.ps1'))
    $guard = $hb.IndexOf('Get-KitHoldGameText $kit $Out')
    Check ($guard -gt 0 -and $guard -lt $hb.IndexOf('Save-KitGameVariant $kit $Out') -and $guard -lt $hb.IndexOf('Find-KitHookshotCachedGame $gameWant')) 'build script: the guards come before the game is kept, copied or built'
    Check ($hb.IndexOf('Get-KitGameBuildMismatch $Out', $hb.IndexOf('Complete-KitGameWrite $Out') - 3000) -gt 0 -or $hb -match 'Get-KitGameBuildMismatch \$Out\)\r?\n\s+if \(\$badAfter') 'build script: the finished build is checked against the note'
    $pk = Get-Content -LiteralPath (Join-Path $scripts 'kit-paths.ps1') -Raw
    Check (([regex]::Matches($pk, 'Get-KitGameBuildMismatch \$g\)\.Count\) \{ continue \}')).Count -eq 2) 'kit-paths: neither game copy source (game cache, another kit) is taken with mixed files'

    # The build script itself, run on a fake kit (it stops at the guards, before any download or build).
    function New-FakeKit([string]$Name) {
        $k = Join-Path $tmp $Name
        New-Item -ItemType Directory -Force -Path (Join-Path $k 'scripts') | Out-Null
        Copy-Item -Path (Join-Path $scripts '*') -Destination (Join-Path $k 'scripts') -Recurse
        foreach ($rel in 'hookshot\PIN', 'hookshot\TOOLS.txt', 'hookshot\patches\0001-a.patch', 'hookshot\hookshot_coop.dll') {
            Write-File (Join-Path $k $rel) (Read-File (Join-Path $kit $rel))
        }
        return $k
    }
    function Invoke-Build([string]$K, [string[]]$More = @()) {
        $keep = @($env:LOCALAPPDATA, $env:OOT_COOP_NO_SHORTCUT, $env:OOT_COOP_NO_LONGSHOT_ASK)
        $env:LOCALAPPDATA = Join-Path $tmp 'appdata'; $env:OOT_COOP_NO_SHORTCUT = '1'; $env:OOT_COOP_NO_LONGSHOT_ASK = '1'
        $ErrorActionPreference = 'Continue'
        try { $o = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $K 'scripts\hookshot-build.ps1') @More 2>&1) -join "`n"; $c = $LASTEXITCODE }
        finally {
            $env:LOCALAPPDATA = $keep[0]; $env:OOT_COOP_NO_SHORTCUT = $keep[1]
            if ($null -eq $keep[2]) { Remove-Item env:OOT_COOP_NO_LONGSHOT_ASK -ErrorAction SilentlyContinue } else { $env:OOT_COOP_NO_LONGSHOT_ASK = $keep[2] }
            $ErrorActionPreference = 'Stop'
        }
        return [pscustomobject]@{ Code = $c; Text = $o }
    }

    # Mixed game, no -Rebuild: refused, nothing changed.
    $k1 = New-FakeKit 'fake-mixed'
    $g1 = Join-Path $k1 'hookshot\game'
    Set-Build $g1 'one' (Get-KitHookshotStamp $k1)
    Write-File (Join-Path $g1 'image.bin') 'image.bin FROM ANOTHER BUILD'
    $before = (Get-ChildItem -LiteralPath $g1 -Recurse -File | ForEach-Object { "$($_.FullName)|$($_.Length)" }) -join ';'
    $r = Invoke-Build $k1
    Check ($r.Code -ne 0 -and $r.Text -like '*image.bin*' -and $r.Text -like '*Nothing was changed*' -and $r.Text -like '*-Rebuild*') "build: a mixed game is refused with the file named [$($r.Code)]"
    $after = (Get-ChildItem -LiteralPath $g1 -Recurse -File | ForEach-Object { "$($_.FullName)|$($_.Length)" }) -join ';'
    Check ($before -eq $after -and -not (Test-KitGameUnfinished $g1) -and -not (Test-Path -LiteralPath (Get-KitGameVariantRoot $k1))) 'build: the refusal changed nothing and kept nothing'

    # The same in a Longshot-only (release) kit: it has no Build-Hookshot-Coop.bat, so the text says the start asks.
    $k1r = New-FakeKit 'fake-mixed-release'
    Write-File (Join-Path $k1r 'Start Squadala.bat') '@echo off'
    $g1r = Join-Path $k1r 'hookshot\game'
    Set-Build $g1r 'one' (Get-KitHookshotStamp $k1r)
    Write-File (Join-Path $g1r 'image.bin') 'image.bin FROM ANOTHER BUILD'
    $r = Invoke-Build $k1r
    Check ($r.Code -ne 0 -and $r.Text -like '*image.bin*' -and $r.Text -like '*Start Squadala.bat asks*' -and $r.Text -notlike '*Build-Hookshot-Coop.bat*') "build (release kit, e.g. NVIDIA DLSS.bat): the refusal does not name Build-Hookshot-Coop.bat [$($r.Code)]"

    # A held kit with other patches than its game: refused, nothing changed.
    $k2 = New-FakeKit 'fake-held'
    $g2 = Join-Path $k2 'hookshot\game'
    Set-Build $g2 'one' (Get-KitHookshotStamp $k2)
    Write-File (Join-Path $k2 'hookshot\patches\0002-b.patch') "two`n"
    Write-File (Get-KitHoldGameFile $k2) "test kit`n"
    $r = Invoke-Build $k2
    Check ($r.Code -ne 0 -and $r.Text -like '*hold-game.txt*' -and $r.Text -like '*patches*' -and $r.Text -like '*Nothing was changed*') "build: a held kit with other patches is refused [$($r.Code)]"
    Check ((Read-File (Join-Path $g2 'image.bin')) -eq 'image.bin one' -and -not (Test-KitGameUnfinished $g2)) 'build: the held game is untouched'
    $r = Invoke-Build $k2 @('-Rebuild')
    Check ($r.Code -ne 0 -and $r.Text -like '*hold-game.txt*') "build: -Rebuild does not get past the marker either [$($r.Code)]"
    Check (-not (Test-KitGameUnfinished $g2) -and (Read-File (Join-Path $g2 'image.bin')) -eq 'image.bin one') 'build: -Rebuild on a held kit changed nothing'
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "$failed check(s) failed."; exit 1 }
Write-Host 'All checks passed.'
