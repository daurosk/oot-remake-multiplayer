# An old kit, as released, updates to a new release made by make_kit_hookshot.ps1: with the OLD kit's own updater
# (its scripts\update.ps1, run from a copy as its play.ps1 does), from a local release list (OOT_COOP_UPDATE_API)
# that names the new release's assets as GitHub will. No network, nothing published, no game run.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\old_kit_update_test.ps1 `
#       -OldZip <Squadala-v0.11.3-windows.zip or a newer kit> -NewDir <make_kit_hookshot -OutRoot folder> -Tag v0.11.4
#
# -OldZip  the old release's zip (best the one downloaded from its GitHub release; its .sha256 beside it is checked).
#          From v0.11.4 a kit up to v0.11.2 cannot see the new release (no oot-coop-* asset): it goes through v0.11.3.
# -NewDir  the folder make_kit_hookshot.ps1 wrote Squadala-<Tag>-windows.zip and its .sha256 to
# -Tag     the new release's tag
# -Work    a folder for the test (default: a new temp folder, removed after)
#
# Checks: the old updater sees the new release (-Status), installs it (-Launch, exit 3), and leaves a working new kit:
# VERSION.txt and every MANIFEST.txt file are the new kit's, the start file is there, the player's coop-options.ini
# values, hookshot\work, the game folder (coop.ini, the built game) and logs are kept, the new options are added,
# and the next start's game build sees that the patches changed (so it brings the game up to date). Exits 1 on a FAIL.
# -CharSel: the game has the old kit's OoT Character Select (part 'ui', its code patch built in). After the update the
# new kit's start steps run with a stand-in of its pinned release (zip in hookshot\downloads, a fake tool, a stand-in
# RomFS, Python from PATH): the need is 'pin', the step brings the new release, a 'no' to the build puts the old one
# back and the next start brings it again, and after the build (its stamp written as hookshot-build.ps1 writes it)
# the game needs no build. Each step is the new kit's own function, in play.ps1's order; the build itself is not run.
param(
    [Parameter(Mandatory)][string]$OldZip,
    [Parameter(Mandatory)][string]$NewDir,
    [Parameter(Mandatory)][ValidatePattern('^v\d')][string]$Tag,
    [string]$Work = '',
    [switch]$CharSel
)
$ErrorActionPreference = 'Stop'
$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Sha([string]$P) { (Get-FileHash -LiteralPath $P -Algorithm SHA256).Hash.ToLowerInvariant() }
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$ps = Join-Path $PSHOME 'powershell.exe'

$own = -not $Work
if ($own) { $Work = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-oldkit-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)) }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$savedApi = $env:OOT_COOP_UPDATE_API
$kitPy = ''
try {
    # --- The old kit, unzipped as a player has it, with a player's things in it
    if (Test-Path -LiteralPath "$OldZip.sha256") {
        Check ((Sha $OldZip) -eq ([regex]::Match([System.IO.File]::ReadAllText("$OldZip.sha256"), '[0-9a-fA-F]{64}').Value.ToLowerInvariant())) 'the old zip matches its .sha256'
    }
    $play = Join-Path $Work 'player'
    New-Item -ItemType Directory -Force -Path $play | Out-Null
    & $tar -x -f $OldZip -C $play
    $kit = @(Get-ChildItem -LiteralPath $play -Directory)[0].FullName
    $oldVersion = ([System.IO.File]::ReadAllLines((Join-Path $kit 'VERSION.txt'))[0])
    Write-Host "old kit: $oldVersion"
    $optFile = Join-Path $kit 'coop-options.ini'
    $opt = [System.IO.File]::ReadAllText($optFile) -replace '(?m)^pvp = off', 'pvp = on'
    # (the local zip has no attestation: the signature check is the release-attest workflow's, on the real assets)
    $opt += "`r`n# mine`r`nverify_signature = no`r`n"
    [System.IO.File]::WriteAllText($optFile, $opt)
    $game = Join-Path $kit 'hookshot\game'
    # (-CharSel: longshot.exe beside mods\, where the start's Character Select step and the stamp's exefs line both look)
    $exeRel = if ($CharSel) { 'hookshot\game' } else { 'hookshot\game\bin' }
    New-Item -ItemType Directory -Force -Path (Join-Path $kit $exeRel), (Join-Path $kit 'hookshot\work\build') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $kit "$exeRel\longshot.exe"), 'built game')
    [System.IO.File]::WriteAllText((Join-Path $kit "$exeRel\coop.ini"), "name=Tester`r`n")
    [System.IO.File]::WriteAllText((Join-Path $kit 'hookshot\work\build\obj.o'), 'compiled')
    [System.IO.File]::WriteAllText((Join-Path $kit 'logs\coop.log'), 'a log')
    # The game as the old kit built it: its stamp, as hookshot-build.ps1 writes it.
    function Get-Stamp([string]$K) {
        $kv = @{}
        foreach ($f in 'PIN', 'TOOLS.txt') {
            Get-Content -LiteralPath (Join-Path $K "hookshot\$f") | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $kv["$f.$($Matches[1])"] = $Matches[2].Trim() } }
        }
        $ph = (@(Get-ChildItem -LiteralPath (Join-Path $K 'hookshot\patches') -Filter '*.patch' -File | Sort-Object Name) | ForEach-Object { Sha $_.FullName }) -join ','
        return "release=$($kv['TOOLS.txt.release'])`ncommit=$($kv['PIN.commit'])`npatches=$ph"
    }
    $oldStamp = Get-Stamp $kit
    # (an IPS32 code patch with one record: the kit reads and strips it as his)
    function Get-Ips([string]$Data) { , [byte[]](@([System.Text.Encoding]::ASCII.GetBytes('IPS32')) + @(0, 1, 0x49, 0x10, 0, 4) + @([System.Text.Encoding]::ASCII.GetBytes($Data)) + @([System.Text.Encoding]::ASCII.GetBytes('EEOF'))) }
    $csCode = 'exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips'
    $csOld = ''
    if ($CharSel) {
        # The old kit's Character Select as its start put it in (part 'ui': the player said no to the NPC characters),
        # and the game built with its code patch: the stamp's exefs line, by the old kit's own functions.
        $cs = Join-Path $game 'mods\OoT Character Select'
        foreach ($f in 'romfs\UI\Old.bin', 'characters.json', 'exefs\x') { New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $cs $f)) | Out-Null }
        foreach ($f in 'romfs\UI\Old.bin', 'characters.json') { [System.IO.File]::WriteAllText((Join-Path $cs $f), 'old') }
        [System.IO.File]::WriteAllBytes((Join-Path $cs $csCode), (Get-Ips 'old1'))
        $q = Join-Path $Work 'old-cs.ps1'
        [System.IO.File]::WriteAllText($q, 'param($K, $G, $Out); . (Join-Path $K ''scripts\kit-paths.ps1''); [System.IO.File]::WriteAllText($Out, (Get-KitCharacterSelectWant (Get-KitCharacterSelectPin $K) ''ui'') + ''|'' + (Get-KitExefsStampLine $G))')
        & $ps -NoProfile -ExecutionPolicy Bypass -File $q $kit $game (Join-Path $Work 'old-cs.txt') | Out-Null
        $csOld, $exefsOld = [System.IO.File]::ReadAllText((Join-Path $Work 'old-cs.txt')) -split '\|', 2
        [System.IO.File]::WriteAllText((Join-Path $cs 'coop-charsel.txt'), "$csOld`n")
        Check ($csOld -match ' ui cz' -and $exefsOld -match '^\nexefs=OoT Character Select/exefs/') "the old kit's Character Select stamp and the game's code patch line [$csOld]"
        $oldStamp += $exefsOld
        $opt += "character_select = no`r`n"
        [System.IO.File]::WriteAllText($optFile, $opt)
    }
    [System.IO.File]::WriteAllText((Join-Path $game 'coop-built-from.txt'), $oldStamp)
    Check ((Select-String -LiteralPath (Join-Path $kit 'scripts\hookshot-build.ps1') -SimpleMatch '$stamp = "release=$($toolsPin.release)`ncommit=$($pin.commit)`npatches=$patchHash"' -Quiet)) 'the old kit''s game stamp is the one this test writes'
    $mine = @{}
    foreach ($f in "$exeRel\longshot.exe", "$exeRel\coop.ini", 'hookshot\game\coop-built-from.txt', 'hookshot\work\build\obj.o', 'logs\coop.log') {
        $mine[$f] = Sha (Join-Path $kit $f)
    }

    # --- The new release, as GitHub lists it
    $newWin = Join-Path $NewDir "Squadala-$Tag-windows.zip"
    foreach ($f in $newWin, "$newWin.sha256") { Check (Test-Path -LiteralPath $f) "the new release has $([System.IO.Path]::GetFileName($f))" }
    Check (-not (Test-Path -LiteralPath (Join-Path $NewDir "oot-coop-$Tag.zip"))) "the new release has no oot-coop-$Tag.zip (Squadala- names only, from v0.11.4)"
    foreach ($z in @($newWin)) {
        $line = [System.IO.File]::ReadAllText("$z.sha256").Trim()
        Check ($line -eq "$(Sha $z)  $([System.IO.Path]::GetFileName($z))") "$([System.IO.Path]::GetFileName($z)).sha256 names its own file and hash"
    }
    $assets = @()
    foreach ($f in @(Get-ChildItem -LiteralPath $NewDir -File | Where-Object { $_.Name -like "oot-coop-$Tag*" -or $_.Name -like "Squadala-$Tag*" -or $_.Name -eq 'Update.bat' })) {
        $assets += [pscustomobject]@{ name = $f.Name; browser_download_url = $f.FullName; size = $f.Length }
    }
    # (the Linux kit goes up beside it: the Windows updater must never take it)
    foreach ($ln in @("Squadala-$Tag-linux.zip")) {
        $lin = Join-Path $Work $ln
        [System.IO.File]::WriteAllText($lin, 'not a windows kit')
        [System.IO.File]::WriteAllText("$lin.sha256", "$(Sha $lin)  $ln`n")
        $assets += [pscustomobject]@{ name = $ln; browser_download_url = $lin; size = 17 }
        $assets += [pscustomobject]@{ name = "$ln.sha256"; browser_download_url = "$lin.sha256"; size = 90 }
    }
    $list = Join-Path $Work 'releases.json'
    $rel = @([pscustomobject]@{ tag_name = $Tag; draft = $false; html_url = "page/$Tag"; assets = $assets })
    [System.IO.File]::WriteAllText($list, (ConvertTo-Json -InputObject $rel -Depth 5))
    $env:OOT_COOP_UPDATE_API = $list

    # --- What the old kit's play.ps1 does on start: -Status, then the update from a copy of its updater with -Launch
    $updater = Join-Path $kit 'scripts\update.ps1'
    $status = (& $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Status -Kit $kit | Out-String).Trim()
    Check ($status -eq "newer $Tag") "the old kit's -Status says 'newer $Tag' [$status]"
    $copy = Join-Path $Work 'update-copy.ps1'
    Copy-Item -LiteralPath $updater -Destination $copy
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $copy -Kit $kit -Launch 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host "     | $_" }
    Check ($code -eq 3) "the old kit's update -Launch ends with 3 (updated: start the new kit) [$code]"

    # --- A working new kit
    $newKit = Join-Path $Work 'new'
    New-Item -ItemType Directory -Force -Path $newKit | Out-Null
    & $tar -x -f $newWin -C $newKit
    $newKit = @(Get-ChildItem -LiteralPath $newKit -Directory)[0].FullName
    $v = [System.IO.File]::ReadAllLines((Join-Path $kit 'VERSION.txt'))[0]
    $newV = [System.IO.File]::ReadAllLines((Join-Path $newKit 'VERSION.txt'))[0]
    # A release gate's test kit names its commit after the tag ('(release v0.11.12-test-<sha8>)'); a published kit has the
    # bare tag. Either way the old updater must leave exactly the new zip's VERSION.txt.
    Check ($newV -match "\(release $([regex]::Escape($Tag))(-test-[0-9a-f]{8})?\)") "the new zip's VERSION.txt is release $Tag or its test kit [$newV]"
    Check ($v -eq $newV) "VERSION.txt is the new release's [$v]"
    Check (-not (Test-Path -LiteralPath (Join-Path $kit 'update-in-progress.txt'))) 'no unfinished-update marker'
    $files = @([System.IO.File]::ReadAllLines((Join-Path $newKit 'MANIFEST.txt')) | Where-Object { $_ -and -not $_.StartsWith('#') })
    $bad = @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $kit $_)) -or (Sha (Join-Path $kit $_)) -ne (Sha (Join-Path $newKit $_)) })
    Check ($bad.Count -eq 0) "all $($files.Count) files of the new kit are in place [$(($bad | Select-Object -First 5) -join ', ')]"
    $oldTag = [regex]::Match($oldVersion, 'release ([^ )]+)').Groups[1].Value
    $oldFiles = @([System.IO.File]::ReadAllLines((Join-Path $kit "backup-$oldTag\MANIFEST.txt")) | Where-Object { $_ -and -not $_.StartsWith('#') })
    $left = @($oldFiles | Where-Object { $files -notcontains $_ -and (Test-Path -LiteralPath (Join-Path $kit $_)) })
    Check ($left.Count -eq 0) "no file of the old kit that the new one dropped is left [$(($left | Select-Object -First 5) -join ', ')]"
    $top = @(Get-ChildItem -LiteralPath $kit -File | ForEach-Object Name | Sort-Object)
    $newTop = @(Get-ChildItem -LiteralPath $newKit -File | ForEach-Object Name | Sort-Object)
    Check ((Compare-Object $top $newTop).Count -eq 0) "the kit's top folder is the new kit's [$($top -join ', ')]"
    Check (Test-Path -LiteralPath (Join-Path $kit 'Start Squadala.bat')) 'the start file Start Squadala.bat is there'
    Check (-not (Test-Path -LiteralPath (Join-Path $kit 'Start OoT Co-op.bat'))) 'one start file: the old updater removed Start OoT Co-op.bat (by its MANIFEST.txt)'
    # The new kit's first start (play.ps1 -NoUpdate after the old updater: update.ps1 -Tidy) moves a shortcut that started it.
    $links = Join-Path $Work 'links'
    New-Item -ItemType Directory -Force -Path $links | Out-Null
    $wsh = New-Object -ComObject WScript.Shell
    $l = $wsh.CreateShortcut((Join-Path $links 'My co-op.lnk')); $l.TargetPath = Join-Path $kit 'Start OoT Co-op.bat'; $l.Save()
    $env:OOT_COOP_SHORTCUT_DIRS = $links
    try { & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kit 'scripts\update.ps1') -Kit $kit -Tidy | Out-Null } finally { Remove-Item env:OOT_COOP_SHORTCUT_DIRS }
    Check ($wsh.CreateShortcut((Join-Path $links 'My co-op.lnk')).TargetPath -ieq (Join-Path $kit 'Start Squadala.bat')) 'one start file: the new kit''s -Tidy points the shortcut at Start Squadala.bat'
    Check ((Select-String -LiteralPath (Join-Path $kit 'scripts\update.ps1') -SimpleMatch 'function Get-KitReleaseZip' -Quiet)) 'the new kit''s updater knows the -windows name'
    $o = [System.IO.File]::ReadAllText($optFile)
    Check ($o -match '(?m)^pvp = on' -and $o -match '(?m)^# mine' -and $o -match '(?m)^verify_signature = no') 'coop-options.ini keeps the player''s values'
    $want = @([System.IO.File]::ReadAllLines((Join-Path $newKit 'coop-options.ini')) | Where-Object { $_ -match '^\s*[#;]?\s*([A-Za-z_][A-Za-z0-9_]*)\s*=' } |
        ForEach-Object { [regex]::Match($_, '([A-Za-z_][A-Za-z0-9_]*)\s*=').Groups[1].Value } | Sort-Object -Unique)
    $missing = @($want | Where-Object { $o -notmatch ('(?m)^\s*[#;]?\s*' + $_ + '\s*=') })
    Check ($missing.Count -eq 0) "coop-options.ini has every key of the new kit's [$($missing -join ', ')]"
    foreach ($f in $mine.Keys) {
        Check ((Test-Path -LiteralPath (Join-Path $kit $f)) -and (Sha (Join-Path $kit $f)) -eq $mine[$f]) "kept: $f"
    }
    # The new kit's play.ps1 (started next with -NoUpdate and the old one's arguments) takes those arguments.
    $np = Join-Path $kit 'scripts\play.ps1'
    $cmd = Get-Command -Name $np -ErrorAction SilentlyContinue
    $params = if ($cmd) { @($cmd.Parameters.Keys) } else { @() }
    $lack = @('NoUpdate', 'Mode', 'Server', 'Name', 'Port', 'Hookshot' | Where-Object { $params -notcontains $_ })
    Check ($cmd -and $lack.Count -eq 0) "the new play.ps1 takes -NoUpdate -Mode -Server -Name -Port -Hookshot [$($lack -join ', ')]"
    # The game: the kit's stamp is now the new release's. When the release changed Longshot's pin, tools or patches, the
    # next start's build sees that and brings the game up to date (incremental: hookshot\work kept); when it did not
    # (v0.11.2 -> v0.11.3: the module only), the built game is reused as it is.
    $newStamp = Get-Stamp $kit
    Check ((Select-String -LiteralPath (Join-Path $kit 'scripts\hookshot-build.ps1') -SimpleMatch '$stamp = "release=$($toolsPin.release)`ncommit=$($pin.commit)`npatches=$patchHash"' -Quiet)) 'the new kit''s game stamp is made the same way'
    Check ($newStamp -eq (Get-Stamp $newKit)) 'the kit''s game stamp is the new release''s'
    if ($newStamp -eq $oldStamp) {
        Write-Host "     | the same Longshot pin, tools and patches as $oldTag`: the built game is reused (no game build on the next start)"
    } else {
        Check ($newStamp -ne $oldStamp) 'the game''s stamp no longer matches: the next start builds the game again'
    }
    # (the kit's Python for Character Select's tool: the one on PATH, never the WindowsApps stub; none: that part is skipped)
    $pyHome = ''
    if ($CharSel) {
        $py = (Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1).Source
        if ($py -and $py -notmatch 'WindowsApps') { try { $pyHome = "$(& $py -I -c 'import sys; print(sys.base_prefix)' 2>$null)".Trim() } catch { $pyHome = '' } }
        if (-not $pyHome -or -not (Test-Path -LiteralPath (Join-Path $pyHome 'python.exe'))) { $pyHome = ''; Write-Host '     | no Python on PATH: the Character Select part is skipped' }
    }
    if ($CharSel -and $pyHome) {
        # The new kit's pinned Character Select, as a stand-in of the same shape: its zip (mod folder + build-from-your-game
        # tool) and characters.json in hookshot\downloads, so nothing is downloaded. The fake tool writes one file.
        $pinFile = Join-Path $kit 'hookshot\CHARACTER-SELECT.txt'
        $rTag = ''
        Get-Content -LiteralPath $pinFile | ForEach-Object { if ($_ -match '^release=(.*)$') { $rTag = $Matches[1].Trim() } }
        $src = Join-Path $Work 'cs-src'
        $top = Join-Path $src "CS-$rTag\mods\OoT Character Select"
        foreach ($f in 'romfs\UI\New.bin', 'romfs\Model\Npc_Zelda.bfres.zs') {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent (Join-Path $top $f)) | Out-Null
            [System.IO.File]::WriteAllText((Join-Path $top $f), 'new')
        }
        New-Item -ItemType Directory -Force -Path (Join-Path $top 'exefs'), (Join-Path $src "CS-$rTag\build-from-your-game") | Out-Null
        # (his code patch's layout, as launch_test.ps1 makes it: the kit's own customizer regions and the cz_head record)
        $ipsMaker = Join-Path $Work 'new-ips.ps1'
        [System.IO.File]::WriteAllText($ipsMaker, @'
param($K, $Out)
. (Join-Path $K 'scripts\kit-paths.ps1')
$ms = New-Object System.IO.MemoryStream
$put = { param([long]$At, [byte[]]$B)
    $h = [byte[]]@((($At -shr 24) -band 255), (($At -shr 16) -band 255), (($At -shr 8) -band 255), ($At -band 255), (($B.Length -shr 8) -band 255), ($B.Length -band 255))
    $ms.Write($h, 0, 6); $ms.Write($B, 0, $B.Length) }
$ms.Write([System.Text.Encoding]::ASCII.GetBytes('IPS32'), 0, 5)
& $put 0x00014910 ([System.Text.Encoding]::ASCII.GetBytes('keep new'))
foreach ($g in $KitCharselCustomizer) { & $put ($g[1] - $KitCharselIpsShift) ([byte[]](1, 2, 3, 4)) }
$ms.Write([byte[]](0x00, 0x9B, 0x77, 0xB0, 0, 0, 0, 16, 0xAA), 0, 9)
$ms.Write([System.Text.Encoding]::ASCII.GetBytes('EEOF'), 0, 4)
[System.IO.File]::WriteAllBytes($Out, $ms.ToArray())
'@)
        & $ps -NoProfile -ExecutionPolicy Bypass -File $ipsMaker $kit (Join-Path $top $csCode) | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $src "CS-$rTag\build-from-your-game\player.py"),
            "import os, sys`na = sys.argv`nmod = a[a.index('--mod') + 1]`nrom = a[a.index('--romfs') + 1]`n" +
            "if not os.path.isfile(os.path.join(rom, 'Pack', 'ZsDic.pack.zs')): sys.exit('no RomFS')`n" +
            "os.makedirs(os.path.join(mod, 'romfs', 'Model'), exist_ok=True)`nopen(os.path.join(mod, 'romfs', 'Model', 'Made_By_Tool.bin'), 'w').write('x')`n")
        $dl = Join-Path $kit 'hookshot\downloads'
        New-Item -ItemType Directory -Force -Path $dl | Out-Null
        $csZip = Join-Path $Work "CS-$rTag.zip"
        & $tar -a -c -f $csZip -C $src "CS-$rTag"
        $json = Join-Path $dl 'charsel-characters.json'
        [System.IO.File]::WriteAllText($json, '{"format": 1, "mod": "OoT Character Select", "characters": []}')
        [System.IO.File]::WriteAllText($pinFile, "release=$rTag`nurl=https://example.invalid/cs`nzip=CS-$rTag.zip`nzip_sha256=$(Sha $csZip)`nzip_folder=CS-$rTag/mods/OoT Character Select`n" +
            "tool_folder=CS-$rTag/build-from-your-game`nfolder=OoT Character Select`noverlay_url=https://example.invalid/characters.json`noverlay_sha256=$(Sha $json)`nips_url=`nips_sha256=`n")
        # (the game's RomFS beside its mods, and the kit's Python: a junction to the one on PATH)
        New-Item -ItemType Directory -Force -Path (Join-Path $game 'romfs\Pack'), (Join-Path $kit 'hookshot\tools') | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $game 'romfs\Pack\ZsDic.pack.zs'), 'z')
        $kitPy = Join-Path $kit 'hookshot\tools\python'
        if (-not (Test-Path -LiteralPath $kitPy)) { New-Item -ItemType Junction -Path $kitPy -Value $pyHome | Out-Null }
        $s = Join-Path $Work 'new-cs.ps1'
        [System.IO.File]::WriteAllText($s, @'
param($K, $Zip, $Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $K 'scripts\kit-paths.ps1')
$env:OOT_COOP_OFFLINE = '0'
$g = Get-KitHookshotGame $K
$m = Join-Path $g 'mods'
$st = { Read-KitFirstLine (Join-Path $m 'OoT Character Select\coop-charsel.txt') }
$r = [ordered]@{}
$r.need1 = Get-KitHookshotNeed $K
Copy-Item -LiteralPath $Zip -Destination (Join-Path $K 'hookshot\downloads') -Force
[void](Invoke-KitCharacterSelectStep $K $g)
$r.cs1 = & $st
$r.tool1 = Test-Path -LiteralPath (Join-Path $m 'OoT Character Select\romfs\Model\Made_By_Tool.bin')
$r.need2 = Get-KitHookshotNeed $K
[void](Undo-KitCharacterSelect $K $m)   # (a 'no' to the build: play.ps1's Restore-KitCharacterSelectStart)
$r.csUndo = & $st
$r.needUndo = Get-KitHookshotNeed $K
Copy-Item -LiteralPath $Zip -Destination (Join-Path $K 'hookshot\downloads') -Force
[void](Invoke-KitCharacterSelectStep $K $g)   # (the next start)
$r.cs2 = & $st
[void](Sync-KitCharselNativePatch $K (Join-Path $m 'contents\0400486024DEC000\coop-puppet'))   # (the build: hookshot-build.ps1's steps before its stamp)
[System.IO.File]::WriteAllText((Join-Path $g 'coop-built-from.txt'), (Get-KitHookshotStamp $K))
Copy-Item -LiteralPath (Join-Path $K 'hookshot\hookshot_coop.dll') -Destination $g -Force -ErrorAction SilentlyContinue
Confirm-KitCharacterSelect $K $m
$r.need3 = Get-KitHookshotNeed $K
$r.built = [System.IO.File]::ReadAllText((Join-Path $g 'coop-built-from.txt'))
$r.wantNow = Get-KitHookshotStamp $K
[System.IO.File]::WriteAllText($Out, (ConvertTo-Json -InputObject $r))
'@)
        & $ps -NoProfile -ExecutionPolicy Bypass -File $s $kit $csZip (Join-Path $Work 'new-cs.json') 2>&1 | ForEach-Object { Write-Host "     | $_" }
        $r = Get-Content -LiteralPath (Join-Path $Work 'new-cs.json') -Raw | ConvertFrom-Json
        $wantUi = "^$([regex]::Escape($rTag)) [0-9a-f]{64} [0-9a-f]{64} - ui "
        $newRel = ($newStamp -split "`n")[0]
        Check ($r.need1 -eq 'pin') "Character Select game: the new kit's start sees 'pin' (built for the Longshot of $oldTag) [$($r.need1)]"
        Check ($r.cs1 -match $wantUi -and $r.tool1) "Character Select game: the start's step brings $rTag, part ui, its files made from the game [$($r.cs1)]"
        Check ($r.need2 -eq 'pin') "Character Select game: still 'pin' before the build [$($r.need2)]"
        Check ($r.csUndo -eq $csOld -and $r.needUndo -eq 'pin') "Character Select game: a 'no' to the build puts the old copy back, 'pin' stays [$($r.csUndo) / $($r.needUndo)]"
        Check ($r.cs2 -match $wantUi) "Character Select game: the next start brings $rTag again [$($r.cs2)]"
        Check ($r.need3 -in '', 'module' -and $r.built.StartsWith("$newRel`n") -and $r.built -match 'exefs=OoT Character Select/exefs/') "Character Select game: after the build no game build is needed [need '$($r.need3)'], the stamp names $newRel and the new code patch"
        if ($r.need3 -notin '', 'module') { Write-Host "     | built: $($r.built -replace "`n", ' / ')"; Write-Host "     | wants: $($r.wantNow -replace "`n", ' / ')" }
    }
} finally {
    $env:OOT_COOP_UPDATE_API = $savedApi
    if ($kitPy -and (Test-Path -LiteralPath $kitPy)) { [System.IO.Directory]::Delete($kitPy) }   # (the junction, not the Python behind it)
    if ($own) { Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
exit 0
