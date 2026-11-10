# Tests for the kit updater (tools/release/kit/scripts/update.ps1 and Update.bat).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\update_test.ps1
# Unit tests of the version order, the release choice, the player's paths and the
# coop-options.ini merge, then whole updates of a small fake kit from a local fake release
# (no network): a good one, a damaged download, a file that cannot be replaced, and the
# one-file Update.bat. Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$release = Split-Path -Parent $PSScriptRoot
$updater = Join-Path $release 'kit\scripts\update.ps1'
. $updater -NoRun

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

# --- Versions
Check ((Compare-KitVersion 'v0.1.0-test2' 'v0.1.0-test10') -eq -1) 'test2 is older than test10'
Check ((Compare-KitVersion 'v0.1.0-test10' 'v0.1.0-test2') -eq 1) 'test10 is newer than test2'
Check ((Compare-KitVersion 'v0.1.0-test2' 'v0.1.0') -eq -1) 'a test build is older than its release'
Check ((Compare-KitVersion 'v0.1.0' 'v0.1.1-test1') -eq -1) 'v0.1.0 is older than v0.1.1-test1'
Check ((Compare-KitVersion 'v0.2.0' 'v0.10.0') -eq -1) 'numbers compare as numbers'
Check ((Compare-KitVersion 'v0.1.0-test2' 'v0.1.0-test2') -eq 0) 'the same version'
Check ((Compare-KitVersion '' 'v0.1.0-test1') -eq -1) 'a kit with no release is older than any release'
Check ((Compare-KitVersion 'v0.1.0-test1' 'nightly') -eq 1) 'a tag that is no version counts as the oldest'

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-update-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    # --- VERSION.txt
    $vdir = Join-Path $tmp 'v'
    New-Item -ItemType Directory -Force -Path $vdir | Out-Null
    [System.IO.File]::WriteAllLines((Join-Path $vdir 'VERSION.txt'), [string[]]@('kit: oot-coop-v0.1.0-test2 (release v0.1.0-test2)', 'built: x'))
    Check ((Get-KitVersion $vdir) -eq 'v0.1.0-test2') 'the version from a release kit''s VERSION.txt'
    [System.IO.File]::WriteAllLines((Join-Path $vdir 'VERSION.txt'), [string[]]@('kit: oot-coop-20261109-1805', 'built: x'))
    Check ((Get-KitVersion $vdir) -eq '') 'a dated test kit has no version'
    Check ((Get-KitVersion (Join-Path $tmp 'none')) -eq '') 'no VERSION.txt, no version'

    # --- Choosing the release
    function Rel($Tag, $Draft, [string[]]$Names) {
        [pscustomobject]@{ tag_name = $Tag; draft = $Draft; html_url = "page/$Tag";
            assets = @($Names | ForEach-Object { [pscustomobject]@{ name = $_; browser_download_url = "dl/$_"; size = 1 } }) }
    }
    $list = @(
        (Rel 'v0.1.0-test2' $false @('oot-coop-v0.1.0-test2.zip')),
        (Rel 'v0.1.0-test10' $false @('oot-coop-v0.1.0-test10.zip', 'oot-coop-v0.1.0-test10.zip.sha256')),
        (Rel 'v0.1.0-test3' $false @('oot-coop-v0.1.0-test3.zip', 'oot-coop-v0.1.0-test3.zip.sha256')),
        (Rel 'v9.0.0' $true @('oot-coop-v9.0.0.zip', 'oot-coop-v9.0.0.zip.sha256')),
        (Rel 'v0.2.0' $false @('Update.bat'))
    )
    $pick = Select-KitRelease $list
    Check ($pick.Tag -eq 'v0.1.0-test10') 'the newest release with a zip and a checksum, drafts never'
    Check ($pick.Sum -eq 'dl/oot-coop-v0.1.0-test10.zip.sha256') 'its checksum file'
    Check ($null -eq (Select-KitRelease @((Rel 'v0.1.0-test2' $false @('oot-coop-v0.1.0-test2.zip'))))) 'no release without a checksum'

    # --- The player's files
    foreach ($p in 'emulator\user\config\coop.ini', 'emulator\user\keys\prod.keys', 'emulator\user\load\0400486024DEC000\coop-puppet\x',
                   'emulator/user/nand/system/save', 'logs\server.log', 'coop-options.ini', 'backup-v0.1.0-test2\Host.bat') {
        Check (Test-KitPlayerPath $p) "the player's: $p"
    }
    foreach ($p in 'emulator\lichii.exe', 'Host.bat', 'scripts\play.ps1', 'emulator\userdata.dll', 'logs.txt', 'VERSION.txt') {
        Check (-not (Test-KitPlayerPath $p)) "the kit's: $p"
    }

    # --- coop-options.ini
    $mine = [string[]]@('# my notes', 'LICHII_COOP_PUPPET=0', '# LICHII_COOP_FAIRY=0', '')
    $new = [string[]]@('# --- On by default ---', '# The puppet', 'LICHII_COOP_PUPPET=1', '', '# Time of day follows the host. To turn it off:',
                       '# LICHII_COOP_TIME=0', '# No Navi:', '# LICHII_COOP_FAIRY=0', '# A new thing', 'LICHII_COOP_NEW=1')
    $m = Merge-KitOptions $mine $new 'v0.1.0-test3'
    Check ($m[1] -eq 'LICHII_COOP_PUPPET=0') 'the player''s value is kept'
    Check (@($m | Where-Object { $_ -match 'LICHII_COOP_PUPPET' }).Count -eq 1) 'a switch the player has is not added again'
    Check (@($m | Where-Object { $_ -match 'LICHII_COOP_FAIRY' }).Count -eq 1) 'a switch the player has behind # is not added again'
    Check ($m -contains '# LICHII_COOP_TIME=0') 'a new switch is added behind #'
    Check ($m -contains '# LICHII_COOP_NEW=1') 'a new switch that is on in the template is added off'
    Check ($m -contains '# Time of day follows the host. To turn it off:') 'with its comment'
    Check (-not ($m -contains '# The puppet')) 'without the comment of a switch the player has'
    Check (-not ($m -contains '# No Navi:')) 'without the comment of a switch the player has behind #'
    $same = Merge-KitOptions $mine ([string[]]@('LICHII_COOP_PUPPET=1')) 'v0.1.0-test3'
    Check ($same.Count -eq $mine.Count) 'nothing new, nothing added'

    # --- Order, paths and addresses
    $order = Get-KitInstallOrder @('VERSION.txt', 'Host.bat', 'MANIFEST.txt', 'emulator\lichii.exe', 'scripts\play.ps1')
    Check ($order[-1] -eq 'VERSION.txt') 'VERSION.txt is written last'
    Check ($order[-2] -eq 'MANIFEST.txt') 'MANIFEST.txt just before it'
    Check ($order.Count -eq 5 -and $order[0] -eq 'Host.bat') 'the rest keep their order'
    foreach ($p in 'tools\gone.txt', 'emulator\old.dll', 'a/b.txt') { Check (Test-KitRelativePath $p) "a plain kit path: $p" }
    foreach ($p in '..\outside.txt', 'tools\..\..\x', 'C:\Windows\x', '\\server\share\x', '\root.txt', 'tools\.\x', 'a\\b', '') {
        Check (-not (Test-KitRelativePath $p)) "not a plain kit path: '$p'"
    }
    $ok = 'https://github.com/daurosk/oot-remake-multiplayer/releases/download/v0.1.0-test3/oot-coop-v0.1.0-test3.zip'
    Check (Test-KitDownloadUrl $ok 'daurosk/oot-remake-multiplayer') 'a download of this project''s releases'
    foreach ($u in 'https://github.com/someone/else/releases/download/v1/x.zip', 'http://github.com/daurosk/oot-remake-multiplayer/releases/download/v1/x.zip',
                   '\\server\share\x.zip', 'C:\x.zip', 'https://github.com/daurosk/oot-remake-multiplayer/releases/download/../../x',
                   'https://example.com/daurosk/oot-remake-multiplayer/releases/download/v1/x.zip') {
        Check (-not (Test-KitDownloadUrl $u 'daurosk/oot-remake-multiplayer')) "not fetched: $u"
    }
    # --- Whole updates of a fake kit
    function Write-File([string]$Path, [string]$Text) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
        [System.IO.File]::WriteAllText($Path, $Text)
    }
    function New-OldKit([string]$Dir) {
        Write-File "$Dir\VERSION.txt" "kit: oot-coop-v0.1.0-test2 (release v0.1.0-test2)`r`n"
        Write-File "$Dir\Host.bat" 'old host'
        Write-File "$Dir\emulator\lichii.exe" 'old exe'
        Write-File "$Dir\emulator\user\config\coop.ini" 'name = Saria'
        Write-File "$Dir\emulator\user\config\qt-config.ini" 'mine'
        Write-File "$Dir\emulator\user\keys\prod.keys" 'my keys'
        Write-File "$Dir\emulator\user\nand\user\save\0000\save.bin" 'my save'
        Write-File "$Dir\emulator\user\load\0400486024DEC000\coop-puppet\romfs\pack.bin" 'my pack'
        Write-File "$Dir\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin" 'old shaders'
        Write-File "$Dir\logs\server.log" 'my log'
        Write-File "$Dir\tools\gone.txt" 'a kit file the new kit drops'
        Write-File "$Dir\notes.txt" 'my own file'
        Write-File "$Dir\coop-options.ini" "LICHII_COOP_PUPPET=0`r`n"
        Write-File "$Dir\MANIFEST.txt" "# Files`r`nHost.bat`r`nemulator\lichii.exe`r`ntools\gone.txt`r`nVERSION.txt`r`n"
    }
    # --- The emulator a kit may bring
    Write-File "$tmp\exe\coop.exe" 'xx coop: loaded xx'
    Write-File "$tmp\exe\plain.exe" 'xx plain Fritchii xx'
    Write-File "$tmp\exe\updater.exe" 'xx coop: loaded xx Check for Updates... xx'
    Check ((Test-KitEmulator "$tmp\exe\coop.exe") -eq '') 'the co-op emulator may be installed'
    Check ((Test-KitEmulator "$tmp\exe\plain.exe") -match 'not the co-op build') 'a plain Fritchii is refused'
    Check ((Test-KitEmulator "$tmp\exe\updater.exe") -match 'own updater') 'an emulator with Fritchii''s updater is refused'
    Check ((Test-KitEmulator "$tmp\exe\none.exe") -match 'no emulator') 'a kit with no emulator is refused'
    $newKit = Join-Path $tmp 'src\oot-coop-v0.1.0-test3'
    Write-File "$newKit\VERSION.txt" "kit: oot-coop-v0.1.0-test3 (release v0.1.0-test3)`r`n"
    Write-File "$newKit\Host.bat" 'new host'
    Write-File "$newKit\Update.bat" 'new updater'
    Write-File "$newKit\emulator\lichii.exe" 'new exe coop: loaded'
    Write-File "$newKit\emulator\user\config\coop.ini" 'name = '
    Write-File "$newKit\scripts\new.ps1" 'new script'
    Write-File "$newKit\coop-options.ini" "LICHII_COOP_PUPPET=1`r`n# Time of day. To turn it off:`r`n# LICHII_COOP_TIME=0`r`n"
    Write-File "$newKit\MANIFEST.txt" "# Files`r`nHost.bat`r`n"
    # The emulator's own folder as Fritchii v5.6+ ships it (qt.conf, Qt plugins).
    Write-File "$newKit\emulator\qt.conf" 'new qt.conf'
    Write-File "$newKit\emulator\plugins\platforms\qwindows.dll" 'new qwindows'
    Write-File "$newKit\emulator\Qt6Core.dll" 'new qt core'
    New-Item -ItemType Directory -Force -Path "$newKit\logs" | Out-Null
    $assets = Join-Path $tmp 'assets'
    New-Item -ItemType Directory -Force -Path $assets | Out-Null
    $zip = Join-Path $assets 'oot-coop-v0.1.0-test3.zip'
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C (Join-Path $tmp 'src') 'oot-coop-v0.1.0-test3'
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  oot-coop-v0.1.0-test3.zip`n")
    $bad = Join-Path $assets 'bad.sha256'
    [System.IO.File]::WriteAllText($bad, ('0' * 64) + "  oot-coop-v0.1.0-test3.zip`n")
    function Write-List([string]$Path, [string]$Sum) {
        $json = ConvertTo-Json -Depth 5 @(
            @{ tag_name = 'v0.1.0-test3'; draft = $false; html_url = 'local'; assets = @(
                @{ name = 'oot-coop-v0.1.0-test3.zip'; browser_download_url = $zip; size = (Get-Item $zip).Length },
                @{ name = 'oot-coop-v0.1.0-test3.zip.sha256'; browser_download_url = $Sum; size = 80 }) },
            @{ tag_name = 'v9.9.9'; draft = $true; html_url = 'local'; assets = @() }
        )
        [System.IO.File]::WriteAllText($Path, $json)
    }
    $good = Join-Path $tmp 'releases.json'; Write-List $good "$zip.sha256"
    $damaged = Join-Path $tmp 'releases-bad.json'; Write-List $damaged $bad
    function Read-File([string]$Path) { if (Test-Path -LiteralPath $Path) { [System.IO.File]::ReadAllText($Path) } else { '<none>' } }
    function Test-PlayerKept([string]$Kit, [string]$Run) {
        Check ((Read-File "$Kit\emulator\user\config\coop.ini") -eq 'name = Saria') "${Run}: coop.ini kept"
        Check ((Read-File "$Kit\emulator\user\config\qt-config.ini") -eq 'mine') "${Run}: settings kept"
        Check ((Read-File "$Kit\emulator\user\keys\prod.keys") -eq 'my keys') "${Run}: keys kept"
        Check ((Read-File "$Kit\emulator\user\nand\user\save\0000\save.bin") -eq 'my save') "${Run}: save kept"
        Check ((Read-File "$Kit\emulator\user\load\0400486024DEC000\coop-puppet\romfs\pack.bin") -eq 'my pack') "${Run}: puppet pack kept"
        Check ((Read-File "$Kit\logs\server.log") -eq 'my log') "${Run}: logs kept"
        Check ((Read-File "$Kit\notes.txt") -eq 'my own file') "${Run}: a file of the player's kept"
    }
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

    # A good update.
    $kit = Join-Path $tmp 'kit1'; New-OldKit $kit
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0) "update ran ($(($out | Select-Object -Last 2) -join ' / '))"
    Check ((Read-File "$kit\Host.bat") -eq 'new host') 'a kit file is replaced'
    Check ((Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'the emulator is replaced'
    Check ((Read-File "$kit\scripts\new.ps1") -eq 'new script') 'a new kit file is added'
    Check ((Read-File "$kit\Update.bat") -eq 'new updater') 'Update.bat is replaced too'
    Check (-not (Test-Path "$kit\tools\gone.txt")) 'a kit file the new kit dropped is removed'
    Check ((Get-KitVersion $kit) -eq 'v0.1.0-test3') 'VERSION.txt names the new version'
    Test-PlayerKept $kit 'good update'
    $opts = [System.IO.File]::ReadAllLines("$kit\coop-options.ini")
    Check ($opts[0] -eq 'LICHII_COOP_PUPPET=0') 'coop-options.ini keeps the player''s value'
    Check ($opts -contains '# LICHII_COOP_TIME=0') 'coop-options.ini gains the new switch behind #'
    Check ((Read-File "$kit\backup-v0.1.0-test2\Host.bat") -eq 'old host') 'the old Host.bat is in the backup'
    Check ((Read-File "$kit\backup-v0.1.0-test2\tools\gone.txt") -eq 'a kit file the new kit drops') 'a removed file is in the backup'
    Check ((Read-File "$kit\backup-v0.1.0-test2\coop-options.ini") -eq "LICHII_COOP_PUPPET=0`r`n") 'the old coop-options.ini is in the backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'a new emulator: the shader cache is moved away'
    Check ((Read-File "$kit\backup-v0.1.0-test2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'into the backup'
    Check (($out -join ' ') -match 'old shader cache was moved') 'and the player is told'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -match 'newest version') 'a second run has nothing to do'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit (Join-Path $tmp 'no-kit') -Check -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0) '-Check on a folder that is no kit stays quiet'

    # A damaged download changes nothing.
    $kit = Join-Path $tmp 'kit2'; New-OldKit $kit
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $damaged 2>&1
    Check ($LASTEXITCODE -ne 0 -and ($out -join ' ') -match 'damaged') 'a wrong SHA-256 stops the update'
    Check ((Read-File "$kit\Host.bat") -eq 'old host' -and (Read-File "$kit\emulator\lichii.exe") -eq 'old exe') 'and changes no file'
    Check (-not (Test-Path "$kit\backup-v0.1.0-test2")) 'and leaves no backup'

    # -Check says a new version is out, and is silent without a list.
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Check -Api $good 2>&1
    Check (($out -join ' ') -match 'new version is out: v0\.1\.0-test3') '-Check names the new version'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Check -Api (Join-Path $tmp 'missing.json') 2>&1
    Check ($LASTEXITCODE -eq 0 -and -not ($out -join '')) '-Check is silent when the list cannot be read'

    # A file that cannot be replaced: everything is put back.
    $kit = Join-Path $tmp 'kit3'; New-OldKit $kit
    $lock = [System.IO.File]::Open("$kit\emulator\lichii.exe", 'Open', 'Read', 'None')
    try {
        $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    } finally { $lock.Dispose() }
    Check ($LASTEXITCODE -ne 0 -and ($out -join ' ') -match 'old files are back') "a failed copy is rolled back"
    Check ((Read-File "$kit\Host.bat") -eq 'old host') 'Host.bat is the old one again'
    Check ((Read-File "$kit\emulator\lichii.exe") -eq 'old exe') 'the emulator is the old one'
    Check ((Read-File "$kit\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'the shader cache is back in place'
    Check (-not (Test-Path "$kit\scripts\new.ps1")) 'the new file is gone again'
    Check ((Get-KitVersion $kit) -eq 'v0.1.0-test2') 'VERSION.txt is the old one'
    Test-PlayerKept $kit 'rolled back'

    # A damaged MANIFEST.txt never removes anything outside the kit or a folder.
    $kit = Join-Path $tmp 'kit5'; New-OldKit $kit
    Write-File "$tmp\outside.txt" 'outside the kit'
    Write-File "$kit\tools\folder\inside.txt" 'in a folder'
    Write-File "$kit\MANIFEST.txt" "..\outside.txt`r`n$tmp\outside.txt`r`ntools\folder`r`ntools\gone.txt`r`n"
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0) 'update with a damaged MANIFEST.txt ran'
    Check ((Read-File "$tmp\outside.txt") -eq 'outside the kit') 'a file outside the kit is not removed'
    Check ((Read-File "$kit\tools\folder\inside.txt") -eq 'in a folder') 'a folder is not removed'
    Check (-not (Test-Path "$kit\tools\gone.txt")) 'a plain listed file is removed'

    # An update that stopped half way (window closed, power cut): VERSION.txt is still the old
    # one, update-in-progress.txt is there, and the next run finishes it.
    $kit = Join-Path $tmp 'kit6'; New-OldKit $kit
    Write-File "$kit\Host.bat" 'new host'
    Write-File "$kit\update-in-progress.txt" 'Update.bat was updating this kit'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Check -Api $good 2>&1
    Check (($out -join ' ') -match 'did not finish') '-Check says an update did not finish'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -match 'Updated to v0\.1\.0-test3') 'the next run finishes a stopped update'
    Check ((Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'the emulator is the new one'
    Check (-not (Test-Path "$kit\update-in-progress.txt")) 'the marker is gone'
    Check ((Read-File "$kit\backup-v0.1.0-test2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'a stopped update finished: the shader cache is in the backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'and no longer in use'
    Test-PlayerKept $kit 'finished update'
    # Stopped after the shader cache was moved, before the exe was replaced: the rerun leaves it
    # in the first run's backup and says where it is.
    $kit = Join-Path $tmp 'kit11'; New-OldKit $kit
    New-Item -ItemType Directory -Force -Path "$kit\backup-v0.1.0-test2\emulator\user\cache" | Out-Null
    Move-Item -LiteralPath "$kit\emulator\user\cache\shader" -Destination "$kit\backup-v0.1.0-test2\emulator\user\cache\shader"
    Write-File "$kit\update-in-progress.txt" "Update.bat was updating this kit from v0.1.0-test2 to v0.1.0-test3. Run Update.bat again to finish it.`r`n"
    Check ((Get-KitUnfinishedFrom $kit) -eq 'v0.1.0-test2') 'the marker names the version the update started from'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'a stop after the shader move: the rerun finishes'
    Check ((Read-File "$kit\backup-v0.1.0-test2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'the shader cache stays in the first backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'and is not in use'
    Check (($out -join ' ') -match 'moved by the update that stopped, is in .*backup-v0\.1\.0-test2\\emulator\\user\\cache\\shader') 'and the player is told where it is'
    # Stopped after VERSION.txt was written but before the marker went: done again too.
    $kit = Join-Path $tmp 'kit7'; New-OldKit $kit
    Write-File "$kit\VERSION.txt" "kit: oot-coop-v0.1.0-test3 (release v0.1.0-test3)`r`n"
    Write-File "$kit\update-in-progress.txt" 'Update.bat was updating this kit'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -notmatch 'newest version' -and (Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'a marker with the new VERSION.txt is done again, not "newest"'
    Check (-not (Test-Path "$kit\update-in-progress.txt")) 'and the marker is gone'
    Check ((Read-File "$kit\backup-v0.1.0-test3\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'an old marker without "from": the shader cache is kept in a backup'
    # The same with a marker that names the old version: the backup is named after it.
    $kit = Join-Path $tmp 'kit12'; New-OldKit $kit
    Write-File "$kit\VERSION.txt" "kit: oot-coop-v0.1.0-test3 (release v0.1.0-test3)`r`n"
    Write-File "$kit\update-in-progress.txt" "Update.bat was updating this kit from v0.1.0-test2 to v0.1.0-test3. Run Update.bat again to finish it.`r`n"
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Read-File "$kit\backup-v0.1.0-test2\Host.bat") -eq 'old host') 'a redo with the new VERSION.txt backs up into backup-<old version>'
    Check ((Read-File "$kit\backup-v0.1.0-test2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'the shader cache too'
    Check (-not (Test-Path "$kit\backup-v0.1.0-test3")) 'no backup named after the new version'

    # A running emulator lets others read its exe but not write it: the backup works, the
    # write fails after other files were written, and everything is put back.
    $kit = Join-Path $tmp 'kit8'; New-OldKit $kit
    $lock = [System.IO.File]::Open("$kit\emulator\lichii.exe", 'Open', 'Read', 'Read')
    try {
        $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    } finally { $lock.Dispose() }
    Check ($LASTEXITCODE -ne 0 -and ($out -join ' ') -match 'old files are back') 'a write that fails half way is rolled back'
    Check ((Read-File "$kit\Host.bat") -eq 'old host') 'a file written before it is the old one again'
    Check ((Read-File "$kit\backup-v0.1.0-test2\emulator\lichii.exe") -eq 'old exe') 'the locked file was backed up'
    Check ((Get-KitVersion $kit) -eq 'v0.1.0-test2') 'VERSION.txt is the old one'
    Check (-not (Test-Path "$kit\update-in-progress.txt")) 'no marker after a clean rollback'
    Check (-not (Test-Path "$kit\scripts\new.ps1")) 'the new file is gone again'
    Check ((Read-File "$kit\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'the shader cache is back after a write failed'
    Check (-not (Test-Path "$kit\backup-v0.1.0-test2\emulator\user\cache\shader")) 'and not left in the backup'
    Test-PlayerKept $kit 'rolled back after a write'

    # Same emulator (as test4 -> test4.1): the shader cache stays.
    $kit = Join-Path $tmp 'kit9'; New-OldKit $kit
    Copy-Item -LiteralPath "$newKit\emulator\lichii.exe" -Destination "$kit\emulator\lichii.exe" -Force
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -match 'Updated to v0\.1\.0-test3') 'an update with the same emulator ran'
    Check ((Read-File "$kit\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'same emulator: the shader cache is kept'
    Check (-not (Test-Path "$kit\backup-v0.1.0-test2\emulator\user\cache")) 'and not copied to the backup'
    Check (($out -join ' ') -notmatch 'shader') 'and nothing is said about it'
    # A new emulator and no shader cache yet: nothing to move, nothing said.
    $kit = Join-Path $tmp 'kit10'; New-OldKit $kit
    Remove-Item -LiteralPath "$kit\emulator\user\cache" -Recurse -Force
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -notmatch 'shader') 'no shader cache: the update runs and says nothing about it'
    # Fritchii v5.6 -> v5.7 (test5 -> test6): the whole emulator folder is the new one, a DLL
    # the old emulator had and the new one does not is gone, the shader cache is moved aside
    # (v5.7 prepares its shaders again), and the player's files stay.
    $kit = Join-Path $tmp 'kit-v57'; New-OldKit $kit
    Write-File "$kit\emulator\lichii.exe" 'Fritchii v5.6 coop: loaded'
    Write-File "$kit\emulator\qt.conf" 'old qt.conf'
    Write-File "$kit\emulator\plugins\platforms\qwindows.dll" 'old qwindows'
    Write-File "$kit\emulator\Qt6Core.dll" 'old qt core'
    Write-File "$kit\emulator\only-in-v56.dll" 'old dll'
    Write-File "$kit\MANIFEST.txt" "# Files`r`nHost.bat`r`nemulator\lichii.exe`r`nemulator\qt.conf`r`nemulator\plugins\platforms\qwindows.dll`r`nemulator\Qt6Core.dll`r`nemulator\only-in-v56.dll`r`nVERSION.txt`r`n"
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0) "v5.6 -> v5.7: the update ran ($(($out | Select-Object -Last 1) -join ' '))"
    Check ((Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'v5.6 -> v5.7: the emulator is the new one'
    Check ((Read-File "$kit\emulator\qt.conf") -eq 'new qt.conf') 'v5.6 -> v5.7: qt.conf is the new one'
    Check ((Read-File "$kit\emulator\plugins\platforms\qwindows.dll") -eq 'new qwindows') 'v5.6 -> v5.7: the Qt plugins are the new ones'
    Check ((Read-File "$kit\emulator\Qt6Core.dll") -eq 'new qt core') 'v5.6 -> v5.7: the DLLs are the new ones'
    Check (-not (Test-Path "$kit\emulator\only-in-v56.dll")) 'v5.6 -> v5.7: a DLL the new emulator does not have is gone'
    Check ((Read-File "$kit\backup-v0.1.0-test2\emulator\only-in-v56.dll") -eq 'old dll') 'v5.6 -> v5.7: and kept in the backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'v5.6 -> v5.7: the shader cache is moved aside'
    Check ((Read-File "$kit\backup-v0.1.0-test2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'v5.6 -> v5.7: into the backup'
    Test-PlayerKept $kit 'v5.6 -> v5.7'
    Write-File "$tmp\exe\a.exe" 'build a'; Write-File "$tmp\exe\a2.exe" 'build a'; Write-File "$tmp\exe\b.exe" 'build b'
    Check (-not (Test-KitEmulatorChanged "$tmp\exe\a.exe" "$tmp\exe\a2.exe")) 'the same exe is the same emulator'
    Check (Test-KitEmulatorChanged "$tmp\exe\a.exe" "$tmp\exe\b.exe") 'another exe is another emulator'
    Check (Test-KitEmulatorChanged "$tmp\exe\missing.exe" "$tmp\exe\b.exe") 'no old exe counts as another emulator'
    # The one-file Update.bat, dropped into an old kit that has no scripts\update.ps1.
    $kit = Join-Path $tmp 'kit 4 (with spaces)'; New-OldKit $kit
    & (Join-Path $release 'make_update_bat.ps1') -Out "$kit\Update.bat"
    $spaced = Join-Path $tmp 'release list (with spaces)'
    New-Item -ItemType Directory -Force -Path $spaced | Out-Null
    Copy-Item $good (Join-Path $spaced 'releases.json')
    $out = cmd /c "`"$kit\Update.bat`" -Yes -Api `"$spaced\releases.json`" < NUL" 2>&1
    Check ($LASTEXITCODE -eq 0) "Update.bat ran ($(($out | Select-Object -Last 2) -join ' / '))"
    Check ((Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'Update.bat updated the kit'
    Check ((Read-File "$kit\Update.bat") -eq 'new updater') 'and replaced itself'
    Test-PlayerKept $kit 'Update.bat'
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
