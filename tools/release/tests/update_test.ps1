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
    Check (-not (Test-Path "$kit\scripts\new.ps1")) 'the new file is gone again'
    Check ((Get-KitVersion $kit) -eq 'v0.1.0-test2') 'VERSION.txt is the old one'
    Test-PlayerKept $kit 'rolled back'

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
