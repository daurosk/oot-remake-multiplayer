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
Check ((Compare-KitVersion 'v0.1.0-beta2' 'v0.1.0-beta10') -eq -1) 'beta2 is older than beta10'
Check ((Compare-KitVersion 'v0.1.0-beta10' 'v0.1.0-beta2') -eq 1) 'beta10 is newer than beta2'
Check ((Compare-KitVersion 'v0.1.0-beta2' 'v0.1.0') -eq -1) 'a test build is older than its release'
Check ((Compare-KitVersion 'v0.1.0' 'v0.1.1-beta1') -eq -1) 'v0.1.0 is older than v0.1.1-beta1'
Check ((Compare-KitVersion 'v0.2.0' 'v0.10.0') -eq -1) 'numbers compare as numbers'
Check ((Compare-KitVersion 'v0.1.0-beta2' 'v0.1.0-beta2') -eq 0) 'the same version'
Check ((Compare-KitVersion '' 'v0.1.0-beta1') -eq -1) 'a kit with no release is older than any release'
Check ((Compare-KitVersion 'v0.1.0-beta1' 'nightly') -eq 1) 'a tag that is no version counts as the oldest'
# The names from 0.10.1 on (SemVer: 0.<test number>.<hotfix>): newer than every old v0.1.0-<build> tag (such as
# the old test names, shaped as v0.1.0-beta10 here), and in order among themselves.
Check ((Compare-KitVersion 'v0.10.1' 'v0.1.0-beta10') -eq 1) 'v0.10.1 is newer than an old v0.1.0-<build> tag'
Check ((Compare-KitVersion 'v0.1.0-beta10' 'v0.10.1') -eq -1) 'an old v0.1.0-<build> tag is older than v0.10.1'
Check ((Compare-KitVersion 'v0.11.0' 'v0.10.1') -eq 1) 'v0.11.0 is newer than v0.10.1'
Check ((Compare-KitVersion 'v0.11.1' 'v0.11.0') -eq 1) 'a hotfix v0.11.1 is newer than v0.11.0'

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-update-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    # --- VERSION.txt
    $vdir = Join-Path $tmp 'v'
    New-Item -ItemType Directory -Force -Path $vdir | Out-Null
    [System.IO.File]::WriteAllLines((Join-Path $vdir 'VERSION.txt'), [string[]]@('kit: oot-coop-v0.1.0-beta2 (release v0.1.0-beta2)', 'built: x'))
    Check ((Get-KitVersion $vdir) -eq 'v0.1.0-beta2') 'the version from a release kit''s VERSION.txt'
    [System.IO.File]::WriteAllLines((Join-Path $vdir 'VERSION.txt'), [string[]]@('kit: oot-coop-20261109-1805', 'built: x'))
    Check ((Get-KitVersion $vdir) -eq '') 'a dated test kit has no version'
    Check ((Get-KitVersion (Join-Path $tmp 'none')) -eq '') 'no VERSION.txt, no version'

    # --- Choosing the release
    function Rel($Tag, $Draft, [string[]]$Names) {
        [pscustomobject]@{ tag_name = $Tag; draft = $Draft; html_url = "page/$Tag";
            assets = @($Names | ForEach-Object { [pscustomobject]@{ name = $_; browser_download_url = "dl/$_"; size = 1 } }) }
    }
    $list = @(
        (Rel 'v0.1.0-beta2' $false @('oot-coop-v0.1.0-beta2.zip')),
        (Rel 'v0.1.0-beta10' $false @('oot-coop-v0.1.0-beta10.zip', 'oot-coop-v0.1.0-beta10.zip.sha256')),
        (Rel 'v0.1.0-beta3' $false @('oot-coop-v0.1.0-beta3.zip', 'oot-coop-v0.1.0-beta3.zip.sha256')),
        (Rel 'v9.0.0' $true @('oot-coop-v9.0.0.zip', 'oot-coop-v9.0.0.zip.sha256')),
        (Rel 'v0.2.0' $false @('Update.bat'))
    )
    $pick = Select-KitRelease $list
    Check ($pick.Tag -eq 'v0.1.0-beta10') 'the newest release with a zip and a checksum, drafts never'
    Check ($pick.Sum -eq 'dl/oot-coop-v0.1.0-beta10.zip.sha256') 'its checksum file'
    Check ($null -eq (Select-KitRelease @((Rel 'v0.1.0-beta2' $false @('oot-coop-v0.1.0-beta2.zip'))))) 'no release without a checksum'

    # --- The player's files
    foreach ($p in 'emulator\user\config\coop.ini', 'emulator\user\keys\prod.keys', 'emulator\user\load\0400486024DEC000\coop-puppet\x',
                   'emulator/user/nand/system/save', 'logs\server.log', 'coop-options.ini', 'backup-v0.1.0-beta2\Host.bat') {
        Check (Test-KitPlayerPath $p) "the player's: $p"
    }
    foreach ($p in 'emulator\lichii.exe', 'Host.bat', 'scripts\play.ps1', 'emulator\userdata.dll', 'logs.txt', 'VERSION.txt') {
        Check (-not (Test-KitPlayerPath $p)) "the kit's: $p"
    }

    # --- coop-options.ini
    $mine = [string[]]@('# my notes', 'LICHII_COOP_PUPPET=0', '# LICHII_COOP_FAIRY=0', '')
    $new = [string[]]@('# --- On by default ---', '# The puppet', 'LICHII_COOP_PUPPET=1', '', '# Time of day follows the host. To turn it off:',
                       '# LICHII_COOP_TIME=0', '# No Navi:', '# LICHII_COOP_FAIRY=0', '# A new thing', 'LICHII_COOP_NEW=1')
    $m = Merge-KitOptions $mine $new 'v0.1.0-beta3'
    Check ($m[1] -eq 'LICHII_COOP_PUPPET=0') 'the player''s value is kept'
    Check (@($m | Where-Object { $_ -match 'LICHII_COOP_PUPPET' }).Count -eq 1) 'a switch the player has is not added again'
    Check (@($m | Where-Object { $_ -match 'LICHII_COOP_FAIRY' }).Count -eq 1) 'a switch the player has behind # is not added again'
    Check ($m -contains '# LICHII_COOP_TIME=0') 'a new switch is added behind #'
    Check ($m -contains 'LICHII_COOP_NEW=1') 'a new switch that is on in the template is added on, as the template has it'
    Check ($m -contains '# Time of day follows the host. To turn it off:') 'with its comment'
    Check (-not ($m -contains '# The puppet')) 'without the comment of a switch the player has'
    Check (-not ($m -contains '# No Navi:')) 'without the comment of a switch the player has behind #'
    $same = Merge-KitOptions $mine ([string[]]@('LICHII_COOP_PUPPET=1')) 'v0.1.0-beta3'
    Check ($same.Count -eq $mine.Count) 'nothing new, nothing added'
    # Any key, not only known names (an older updater knew a list and never added server_allow).
    $m = Merge-KitOptions ([string[]]@('auto_update = no', 'LICHII_COOP_PUPPET=1')) ([string[]]@('auto_update = ask', '# Who may join:',
        '# server_allow = 25.1.2.3', '# Shader libraries:', '# EDEN_NX2_GPL=0', 'some_new_key = 7')) 'v0.1.0-beta3'
    Check ($m -contains '# server_allow = 25.1.2.3' -and $m -contains '# Who may join:') 'server_allow is added with its comment'
    Check ($m -contains '# EDEN_NX2_GPL=0') 'an EDEN_NX2_ key is added'
    Check ($m -contains 'some_new_key = 7') 'a key of a name no updater knew is added'
    Check ($m[0] -eq 'auto_update = no' -and @($m | Where-Object { $_ -match 'auto_update' }).Count -eq 1) 'the player''s auto_update is kept, not added again'
    # The real template: an old file that has only some of its keys gets every one, loses none.
    $kitOptions = [System.IO.File]::ReadAllLines((Join-Path $release 'kit\coop-options.ini'))
    $old = [string[]]@('# my own note', 'auto_update = yes', 'LICHII_COOP_PUPPET=0', 'LICHII_COOP_OBJECTS=1')
    $m = Merge-KitOptions $old $kitOptions 'v0.1.0-beta3'
    $missing = @((Get-KitOptionNames $kitOptions).Keys | Where-Object { -not (Get-KitOptionNames $m).ContainsKey($_) })
    Check ($missing.Count -eq 0) "every key of the kit's coop-options.ini is in the merged file [$($missing -join ',')]"
    Check ((@($m)[0..3] -join '|') -eq ($old -join '|')) 'the player''s lines stay first and unchanged'
    foreach ($k in 'server_allow', 'server_bind', 'public_address_lookup', 'EDEN_NX2_GPL', 'pvp', 'color') {
        Check ((Get-KitOptionNames $kitOptions).ContainsKey($k)) "the kit's coop-options.ini names $k"
    }
    Check ($m -contains '# color = blue') 'an older kit gets the color line, behind #, so its colour stays its player number''s'
    Check (@($m | Where-Object { $_ -match '^# Each player''s Link wears a tunic' }).Count -eq 1) 'with its comment'
    $again = Merge-KitOptions $m $kitOptions 'v0.1.0-beta4'
    Check ($again.Count -eq $m.Count) 'merging twice adds nothing the second time'
    # The new kit's own merge is used when it has one.
    $nk = Join-Path $tmp 'newmerge'
    New-Item -ItemType Directory -Force -Path "$nk\scripts" | Out-Null
    [System.IO.File]::WriteAllText("$nk\scripts\update.ps1", "param([switch]`$NoRun)`r`nfunction Merge-KitOptions([string[]]`$Mine, [string[]]`$New, [string]`$Version) { return ,([string[]](@(`$Mine) + @('from the new kit'))) }`r`n")
    $m = Merge-KitOptionsWith $nk ([string[]]@('a=1', 'b=2')) ([string[]]@('c=3')) 'v0.1.0-beta3'
    Check (($m -join '|') -eq 'a=1|b=2|from the new kit') "the new kit's own Merge-KitOptions is used [$($m -join '|')]"
    Check ((Get-Command Merge-KitOptions).ScriptBlock.File -eq $updater) 'and it does not replace this updater''s own'
    [System.IO.File]::WriteAllText("$nk\scripts\update.ps1", "param([switch]`$NoRun)`r`nfunction Merge-KitOptions([string[]]`$Mine, [string[]]`$New, [string]`$Version) { return ,([string[]]@()) }`r`n")
    $m = Merge-KitOptionsWith $nk ([string[]]@('a=1')) ([string[]]@('c=3')) 'v0.1.0-beta3'
    Check (($m -join '|') -match '^a=1\|.*c=3$') "a new kit's merge that drops lines is not used [$($m -join '|')]"
    $m = Merge-KitOptionsWith (Join-Path $tmp 'none') ([string[]]@('a=1')) ([string[]]@('c=3')) 'v0.1.0-beta3'
    Check (($m -join '|') -match 'c=3$') 'no new kit script: this updater''s merge'
    # On start (-Status): keys from scripts\coop-options.default.ini an older update did not add.
    $sk = Join-Path $tmp 'startkit'
    New-Item -ItemType Directory -Force -Path "$sk\scripts" | Out-Null
    [System.IO.File]::WriteAllLines("$sk\scripts\coop-options.default.ini", $kitOptions)
    [System.IO.File]::WriteAllLines("$sk\coop-options.ini", $old)
    Check (Update-KitOptions $sk) 'a kit left short by an older updater is filled in'
    $now = [System.IO.File]::ReadAllLines("$sk\coop-options.ini")
    Check (@($now -match '^# server_allow = ').Count -eq 1 -and $now[1] -eq 'auto_update = yes') 'with server_allow, the player''s values kept'
    Check (-not (Update-KitOptions $sk)) 'and only once'
    Remove-Item -LiteralPath "$sk\coop-options.ini"
    Check ((Update-KitOptions $sk) -and (Test-Path -LiteralPath "$sk\coop-options.ini")) 'no coop-options.ini: the default one is written'
    Remove-Item -LiteralPath "$sk\scripts\coop-options.default.ini"
    Check (-not (Update-KitOptions $sk)) 'a kit without the default file is left alone'

    # --- Order, paths and addresses
    $order = Get-KitInstallOrder @('VERSION.txt', 'Host.bat', 'MANIFEST.txt', 'emulator\lichii.exe', 'scripts\play.ps1')
    Check ($order[-1] -eq 'VERSION.txt') 'VERSION.txt is written last'
    Check ($order[-2] -eq 'MANIFEST.txt') 'MANIFEST.txt just before it'
    Check ($order.Count -eq 5 -and $order[0] -eq 'Host.bat') 'the rest keep their order'
    foreach ($p in 'tools\gone.txt', 'emulator\old.dll', 'a/b.txt') { Check (Test-KitRelativePath $p) "a plain kit path: $p" }
    foreach ($p in '..\outside.txt', 'tools\..\..\x', 'C:\Windows\x', '\\server\share\x', '\root.txt', 'tools\.\x', 'a\\b', '') {
        Check (-not (Test-KitRelativePath $p)) "not a plain kit path: '$p'"
    }
    $ok = 'https://github.com/daurosk/oot-remake-multiplayer/releases/download/v0.1.0-beta3/oot-coop-v0.1.0-beta3.zip'
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
        Write-File "$Dir\VERSION.txt" "kit: oot-coop-v0.1.0-beta2 (release v0.1.0-beta2)`r`n"
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
    $newKit = Join-Path $tmp 'src\oot-coop-v0.1.0-beta3'
    Write-File "$newKit\VERSION.txt" "kit: oot-coop-v0.1.0-beta3 (release v0.1.0-beta3)`r`n"
    Write-File "$newKit\Host.bat" 'new host'
    Write-File "$newKit\Update.bat" 'new updater'
    Write-File "$newKit\emulator\lichii.exe" 'new exe coop: loaded'
    Write-File "$newKit\emulator\user\config\coop.ini" 'name = '
    Write-File "$newKit\scripts\new.ps1" 'new script'
    # A verifier that would pass anything: the updater must never run the download's own.
    Write-File "$newKit\scripts\verify_kit.ps1" 'exit 0'
    Write-File "$newKit\coop-options.ini" "LICHII_COOP_PUPPET=1`r`n# Time of day. To turn it off:`r`n# LICHII_COOP_TIME=0`r`n"
    Write-File "$newKit\MANIFEST.txt" "# Files`r`nHost.bat`r`n"
    # The emulator's own folder as Fritchii v5.6+ ships it (qt.conf, Qt plugins).
    Write-File "$newKit\emulator\qt.conf" 'new qt.conf'
    Write-File "$newKit\emulator\plugins\platforms\qwindows.dll" 'new qwindows'
    Write-File "$newKit\emulator\Qt6Core.dll" 'new qt core'
    New-Item -ItemType Directory -Force -Path "$newKit\logs" | Out-Null
    $assets = Join-Path $tmp 'assets'
    New-Item -ItemType Directory -Force -Path $assets | Out-Null
    $zip = Join-Path $assets 'oot-coop-v0.1.0-beta3.zip'
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C (Join-Path $tmp 'src') 'oot-coop-v0.1.0-beta3'
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  oot-coop-v0.1.0-beta3.zip`n")
    $bad = Join-Path $assets 'bad.sha256'
    [System.IO.File]::WriteAllText($bad, ('0' * 64) + "  oot-coop-v0.1.0-beta3.zip`n")
    function Write-List([string]$Path, [string]$Sum) {
        $json = ConvertTo-Json -Depth 5 @(
            @{ tag_name = 'v0.1.0-beta3'; draft = $false; html_url = 'local'; assets = @(
                @{ name = 'oot-coop-v0.1.0-beta3.zip'; browser_download_url = $zip; size = (Get-Item $zip).Length },
                @{ name = 'oot-coop-v0.1.0-beta3.zip.sha256'; browser_download_url = $Sum; size = 80 }) },
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
    Check ((Get-KitVersion $kit) -eq 'v0.1.0-beta3') 'VERSION.txt names the new version'
    Test-PlayerKept $kit 'good update'
    $opts = [System.IO.File]::ReadAllLines("$kit\coop-options.ini")
    Check ($opts[0] -eq 'LICHII_COOP_PUPPET=0') 'coop-options.ini keeps the player''s value'
    Check ($opts -contains '# LICHII_COOP_TIME=0') 'coop-options.ini gains the new switch behind #'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\Host.bat") -eq 'old host') 'the old Host.bat is in the backup'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\tools\gone.txt") -eq 'a kit file the new kit drops') 'a removed file is in the backup'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\coop-options.ini") -eq "LICHII_COOP_PUPPET=0`r`n") 'the old coop-options.ini is in the backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'a new emulator: the shader cache is moved away'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'into the backup'
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
    Check (-not (Test-Path "$kit\backup-v0.1.0-beta2")) 'and leaves no backup'

    # The release signature: this kit's own scripts\verify_kit.ps1 (0 good, 1 bad, 2 cannot check).
    $env:OOT_COOP_VERIFY_NO_AUTH_CHECK = '1'
    function Set-Verifier([string]$Kit, [int]$Code, [string]$Say) {
        Write-File "$Kit\scripts\verify_kit.ps1" ("param([string]`$Zip, [string]`$Repo)`r`n" +
            "[System.IO.File]::WriteAllText('$Kit\verified.txt', `"`$Zip|`$Repo`")`r`nWrite-Host '$Say'`r`nexit $Code`r`n")
    }
    $skit = Join-Path $tmp 'kit-sig-bad'; New-OldKit $skit; Set-Verifier $skit 1 'FAIL: no valid attestation (test)'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $skit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -ne 0 -and ($out -join ' ') -match 'signature does not check') 'a zip whose signature fails is refused (the download''s own verifier is not used)'
    Check ((Read-File "$skit\Host.bat") -eq 'old host' -and (Read-File "$skit\emulator\lichii.exe") -eq 'old exe') 'and changes no file'
    Check (-not (Test-Path "$skit\backup-v0.1.0-beta2")) 'and leaves no backup'
    Check ((Read-File "$skit\verified.txt") -match 'oot-coop-v0\.1\.0-beta3\.zip\|daurosk/oot-remake-multiplayer$') 'the verifier got the downloaded zip and the repo'
    Check (($out -join ' ') -match 'verify_signature = no') 'and the player is told how to skip it'
    Add-Content -LiteralPath "$skit\coop-options.ini" -Value 'verify_signature = no'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $skit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Read-File "$skit\Host.bat") -eq 'new host' -and ($out -join ' ') -match 'Signature check skipped') 'verify_signature = no: updated on the SHA-256 alone'
    Check ((Read-File "$skit\scripts\verify_kit.ps1") -eq 'exit 0') 'the new kit brings its verifier'
    $skit = Join-Path $tmp 'kit-sig-good'; New-OldKit $skit; Set-Verifier $skit 0 'OK (test)'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $skit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Read-File "$skit\emulator\lichii.exe") -eq 'new exe coop: loaded' -and ($out -join ' ') -match 'Signature checked') 'a good signature: updated, emulator too, and said'
    Test-PlayerKept $skit 'signed update'
    $skit = Join-Path $tmp 'kit-sig-none'; New-OldKit $skit; Set-Verifier $skit 2 'Could not check the signature: no gh (test)'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $skit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Read-File "$skit\Host.bat") -eq 'new host' -and ($out -join ' ') -match 'Signature not checked: Could not check') 'cannot check the signature: updated on the SHA-256, and said'
    Remove-Item Env:\OOT_COOP_VERIFY_NO_AUTH_CHECK

    # -Check says a new version is out, and says so when it cannot read the list.
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Check -Api $good 2>&1
    Check (($out -join ' ') -match 'new version is out: v0\.1\.0-beta3') '-Check names the new version'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Check -Api (Join-Path $tmp 'missing.json') 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -match 'Could not check') '-Check says when the list cannot be read'

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
    Check ((Get-KitVersion $kit) -eq 'v0.1.0-beta2') 'VERSION.txt is the old one'
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
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -match 'Updated to v0\.1\.0-beta3') 'the next run finishes a stopped update'
    Check ((Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'the emulator is the new one'
    Check (-not (Test-Path "$kit\update-in-progress.txt")) 'the marker is gone'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'a stopped update finished: the shader cache is in the backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'and no longer in use'
    Test-PlayerKept $kit 'finished update'
    # Stopped after the shader cache was moved, before the exe was replaced: the rerun leaves it
    # in the first run's backup and says where it is.
    $kit = Join-Path $tmp 'kit11'; New-OldKit $kit
    New-Item -ItemType Directory -Force -Path "$kit\backup-v0.1.0-beta2\emulator\user\cache" | Out-Null
    Move-Item -LiteralPath "$kit\emulator\user\cache\shader" -Destination "$kit\backup-v0.1.0-beta2\emulator\user\cache\shader"
    Write-File "$kit\update-in-progress.txt" "Update.bat was updating this kit from v0.1.0-beta2 to v0.1.0-beta3. Run Update.bat again to finish it.`r`n"
    Check ((Get-KitUnfinishedFrom $kit) -eq 'v0.1.0-beta2') 'the marker names the version the update started from'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'a stop after the shader move: the rerun finishes'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'the shader cache stays in the first backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'and is not in use'
    Check (($out -join ' ') -match 'moved by the update that stopped, is in .*backup-v0\.1\.0-beta2\\emulator\\user\\cache\\shader') 'and the player is told where it is'
    # Stopped after VERSION.txt was written but before the marker went: done again too.
    $kit = Join-Path $tmp 'kit7'; New-OldKit $kit
    Write-File "$kit\VERSION.txt" "kit: oot-coop-v0.1.0-beta3 (release v0.1.0-beta3)`r`n"
    Write-File "$kit\update-in-progress.txt" 'Update.bat was updating this kit'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -notmatch 'newest version' -and (Read-File "$kit\emulator\lichii.exe") -eq 'new exe coop: loaded') 'a marker with the new VERSION.txt is done again, not "newest"'
    Check (-not (Test-Path "$kit\update-in-progress.txt")) 'and the marker is gone'
    Check ((Read-File "$kit\backup-v0.1.0-beta3\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'an old marker without "from": the shader cache is kept in a backup'
    # The same with a marker that names the old version: the backup is named after it.
    $kit = Join-Path $tmp 'kit12'; New-OldKit $kit
    Write-File "$kit\VERSION.txt" "kit: oot-coop-v0.1.0-beta3 (release v0.1.0-beta3)`r`n"
    Write-File "$kit\update-in-progress.txt" "Update.bat was updating this kit from v0.1.0-beta2 to v0.1.0-beta3. Run Update.bat again to finish it.`r`n"
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Read-File "$kit\backup-v0.1.0-beta2\Host.bat") -eq 'old host') 'a redo with the new VERSION.txt backs up into backup-<old version>'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'the shader cache too'
    Check (-not (Test-Path "$kit\backup-v0.1.0-beta3")) 'no backup named after the new version'

    # A running emulator lets others read its exe but not write it: the backup works, the
    # write fails after other files were written, and everything is put back.
    $kit = Join-Path $tmp 'kit8'; New-OldKit $kit
    $lock = [System.IO.File]::Open("$kit\emulator\lichii.exe", 'Open', 'Read', 'Read')
    try {
        $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    } finally { $lock.Dispose() }
    Check ($LASTEXITCODE -ne 0 -and ($out -join ' ') -match 'old files are back') 'a write that fails half way is rolled back'
    Check ((Read-File "$kit\Host.bat") -eq 'old host') 'a file written before it is the old one again'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\emulator\lichii.exe") -eq 'old exe') 'the locked file was backed up'
    Check ((Get-KitVersion $kit) -eq 'v0.1.0-beta2') 'VERSION.txt is the old one'
    Check (-not (Test-Path "$kit\update-in-progress.txt")) 'no marker after a clean rollback'
    Check (-not (Test-Path "$kit\scripts\new.ps1")) 'the new file is gone again'
    Check ((Read-File "$kit\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'the shader cache is back after a write failed'
    Check (-not (Test-Path "$kit\backup-v0.1.0-beta2\emulator\user\cache\shader")) 'and not left in the backup'
    Test-PlayerKept $kit 'rolled back after a write'

    # Same emulator (an update that keeps lichii.exe): the shader cache stays.
    $kit = Join-Path $tmp 'kit9'; New-OldKit $kit
    Copy-Item -LiteralPath "$newKit\emulator\lichii.exe" -Destination "$kit\emulator\lichii.exe" -Force
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -match 'Updated to v0\.1\.0-beta3') 'an update with the same emulator ran'
    Check ((Read-File "$kit\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'same emulator: the shader cache is kept'
    Check (-not (Test-Path "$kit\backup-v0.1.0-beta2\emulator\user\cache")) 'and not copied to the backup'
    Check (($out -join ' ') -notmatch 'shader') 'and nothing is said about it'
    # A new emulator and no shader cache yet: nothing to move, nothing said.
    $kit = Join-Path $tmp 'kit10'; New-OldKit $kit
    Remove-Item -LiteralPath "$kit\emulator\user\cache" -Recurse -Force
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $kit -Yes -Api $good 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -notmatch 'shader') 'no shader cache: the update runs and says nothing about it'
    # Fritchii v5.6 -> v5.7: the whole emulator folder is the new one, a DLL
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
    Check ((Read-File "$kit\backup-v0.1.0-beta2\emulator\only-in-v56.dll") -eq 'old dll') 'v5.6 -> v5.7: and kept in the backup'
    Check (-not (Test-Path "$kit\emulator\user\cache\shader")) 'v5.6 -> v5.7: the shader cache is moved aside'
    Check ((Read-File "$kit\backup-v0.1.0-beta2\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'v5.6 -> v5.7: into the backup'
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

    # --- An update to a pre-release end to end, as testers met it: Host.bat's start check (-Status), then
    # -Launch while the host's server window from the last game is still open. The release
    # list is shaped like GitHub's: pre-releases, an Update.bat asset, '<hash>  name' sha256.
    $e2e = Join-Path $tmp 'e2e'
    $k7 = Join-Path $e2e 'oot-coop-v0.1.0-beta7'
    Write-File "$k7\VERSION.txt" "kit: oot-coop-v0.1.0-beta7 (release v0.1.0-beta7)`r`nbuilt: x`r`n"
    Write-File "$k7\Host.bat" 'beta7 host'
    Write-File "$k7\emulator\lichii.exe" 'beta7 exe coop: loaded'
    Write-File "$k7\emulator\user\config\coop.ini" 'name = Saria'
    Write-File "$k7\emulator\user\nand\user\save\0000\save.bin" 'my save'
    Write-File "$k7\coop-options.ini" "# beta7 options`r`nauto_update = ask`r`nLICHII_COOP_PUPPET=1`r`n"
    Write-File "$k7\MANIFEST.txt" "Host.bat`r`nemulator\lichii.exe`r`nVERSION.txt`r`n"
    Copy-Item -LiteralPath $updater -Destination (Join-Path (New-Item -ItemType Directory -Force -Path "$k7\scripts").FullName 'update.ps1')
    $src8 = Join-Path $e2e 'src'
    $k8 = Join-Path $src8 'oot-coop-v0.1.0-beta8'
    Write-File "$k8\VERSION.txt" "kit: oot-coop-v0.1.0-beta8 (release v0.1.0-beta8)`r`nbuilt: x`r`n"
    Write-File "$k8\Host.bat" 'beta8 host'
    Write-File "$k8\emulator\lichii.exe" 'beta8 exe coop: loaded'
    Write-File "$k8\MANIFEST.txt" "Host.bat`r`nemulator\lichii.exe`r`nscripts\update.ps1`r`nscripts\coop-options.default.ini`r`nVERSION.txt`r`n"
    New-Item -ItemType Directory -Force -Path "$k8\scripts", "$k8\emulator\user", "$k8\logs" | Out-Null
    Copy-Item -LiteralPath $updater -Destination "$k8\scripts\update.ps1"
    Copy-Item -LiteralPath (Join-Path $release 'kit\coop-options.ini') -Destination "$k8\coop-options.ini"
    Copy-Item -LiteralPath (Join-Path $release 'kit\coop-options.ini') -Destination "$k8\scripts\coop-options.default.ini"
    $rel8 = Join-Path $e2e 'assets'
    New-Item -ItemType Directory -Force -Path $rel8 | Out-Null
    $zip8 = Join-Path $rel8 'oot-coop-v0.1.0-beta8.zip'
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip8 -C $src8 'oot-coop-v0.1.0-beta8'
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
    [System.IO.File]::WriteAllText("$zip8.sha256", (Get-FileHash -LiteralPath $zip8 -Algorithm SHA256).Hash.ToLowerInvariant() + "  oot-coop-v0.1.0-beta8.zip`n")
    Write-File "$rel8\Update.bat" 'update bat'
    function Asset($Name, $Path) { @{ name = $Name; browser_download_url = $Path; size = (Get-Item $Path).Length } }
    $list8 = Join-Path $e2e 'releases.json'
    [System.IO.File]::WriteAllText($list8, (ConvertTo-Json -Depth 5 @(
        @{ tag_name = 'v0.1.0-beta8'; name = 'v0.1.0-beta8 (early test)'; draft = $false; prerelease = $true; html_url = 'local'; assets = @(
            (Asset 'oot-coop-v0.1.0-beta8.zip' $zip8), (Asset 'oot-coop-v0.1.0-beta8.zip.sha256' "$zip8.sha256"), (Asset 'Update.bat' "$rel8\Update.bat")) },
        @{ tag_name = 'v0.1.0-beta7'; name = 'v0.1.0-beta7 (early test)'; draft = $false; prerelease = $true; html_url = 'local'; assets = @(
            (Asset 'Update.bat' "$rel8\Update.bat")) })))
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File "$k7\scripts\update.ps1" -Status -Kit $k7 -Api $list8 2>&1
    Check ((($out | Where-Object { "$_".Trim() }) -join '|') -eq 'newer v0.1.0-beta8') "beta7 -> beta8: the start check finds the pre-release [$($out -join '|')]"
    # The host's server window from the last game: a stand-in coop_server.exe of this kit.
    Copy-Item -LiteralPath "$env:SystemRoot\System32\PING.EXE" -Destination (Join-Path (New-Item -ItemType Directory -Force -Path "$k7\server").FullName 'coop_server.exe')
    $srv = Start-Process -FilePath "$k7\server\coop_server.exe" -ArgumentList '-n', '120', '127.0.0.1' -WindowStyle Hidden -PassThru
    try {
        $out = & $ps -NoProfile -ExecutionPolicy Bypass -File "$k7\scripts\update.ps1" -Launch -Kit $k7 -Api $list8 2>&1
        Check ($LASTEXITCODE -eq 3) "beta7 -> beta8: -Launch updates and says start the new kit [$($LASTEXITCODE): $(($out | Select-Object -Last 2) -join ' / ')]"
        Check (($out -join ' ') -match "Stopping this kit's co-op server") 'beta7 -> beta8: the old server left running is stopped, and the player is told'
        Check ($srv.HasExited -or $srv.WaitForExit(5000)) 'beta7 -> beta8: and it is no longer running'
    } finally { if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue } }
    Check ((Get-KitVersion $k7) -eq 'v0.1.0-beta8') 'beta7 -> beta8: VERSION.txt names beta8'
    Check ((Read-File "$k7\Host.bat") -eq 'beta8 host' -and (Read-File "$k7\emulator\lichii.exe") -eq 'beta8 exe coop: loaded') 'beta7 -> beta8: the kit files are beta8''s'
    Check ((Read-File "$k7\emulator\user\nand\user\save\0000\save.bin") -eq 'my save' -and (Read-File "$k7\emulator\user\config\coop.ini") -eq 'name = Saria') 'beta7 -> beta8: save and name kept'
    $o8 = [System.IO.File]::ReadAllLines("$k7\coop-options.ini")
    Check ($o8[0] -eq '# beta7 options' -and $o8[1] -eq 'auto_update = ask') 'beta7 -> beta8: the player''s options stay first'
    foreach ($key in 'server_allow', 'server_bind', 'public_address_lookup', 'EDEN_NX2_GPL') {
        Check (@($o8 -match "^\s*#?\s*$key\s*=").Count -eq 1) "beta7 -> beta8: coop-options.ini gains $key once"
    }
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File "$k7\scripts\update.ps1" -Status -Kit $k7 -Api $list8 2>&1
    Check (-not (($out | Where-Object { "$_".Trim() }) -join '')) 'beta7 -> beta8: after it, the start check is quiet'
    # Failures say why and how to update by hand.
    $none = Join-Path $e2e 'no-such-list.json'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File "$k7\scripts\update.ps1" -Status -Kit $k7 -Api $none 2>&1
    Check ((($out -join ' ')) -match '^failed \S') "no answer: the start check says 'failed <why>' [$($out -join ' ')]"
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File "$k7\scripts\update.ps1" -Yes -Kit $k7 -Api $none 2>&1
    Check ($LASTEXITCODE -eq 1 -and ($out -join ' ') -match 'Update failed: \S' -and ($out -join ' ') -match 'To update by hand') 'no answer: Update.bat says why and the steps by hand'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File "$k7\scripts\update.ps1" -Check -Kit $k7 -Api $none 2>&1
    Check (($out -join ' ') -match 'Could not check') '-Check says it could not check'
    Check ((Get-KitErrorText 'The remote server returned an error: (403) Forbidden.') -match 'try again later') 'a refusal from GitHub is explained'
    # The emulator still running: not stopped, the update fails with what to do.
    $k9 = Join-Path $e2e 'kit-emu-running'
    New-OldKit $k9
    Copy-Item -LiteralPath "$env:SystemRoot\System32\PING.EXE" -Destination "$k9\emulator\lichii.exe" -Force
    $emuP = Start-Process -FilePath "$k9\emulator\lichii.exe" -ArgumentList '-n', '120', '127.0.0.1' -WindowStyle Hidden -PassThru
    try {
        $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Launch -Kit $k9 -Api $good 2>&1
        Check ($LASTEXITCODE -eq 1 -and ($out -join ' ') -match 'still running' -and ($out -join ' ') -match 'To update by hand') "emulator running: the update stops and says what to do [$(($out | Select-Object -First 3) -join ' / ')]"
        Check (-not $emuP.HasExited) 'emulator running: it is not stopped'
        Check ((Get-KitVersion $k9) -eq 'v0.1.0-beta2') 'emulator running: nothing changed'
    } finally { Stop-Process -Id $emuP.Id -Force -ErrorAction SilentlyContinue; $emuP.WaitForExit(5000) | Out-Null }
    # --- An emulator kit updated to a Hookshot-only kit (make_kit_hookshot.ps1): the emulator and its .bat files go
    # (to the backup), the player's emulator\user stays and is named, the desktop shortcut follows the new launcher.
    $hsSrc = Join-Path $tmp 'hsrc\oot-coop-v0.1.0-beta4'
    Write-File "$hsSrc\VERSION.txt" "kit: oot-coop-v0.1.0-beta4 (release v0.1.0-beta4)`r`nkind: hookshot`r`n"
    Write-File "$hsSrc\Start OoT Co-op.bat" 'new launcher'
    Write-File "$hsSrc\scripts\start.ps1" 'new start'
    Write-File "$hsSrc\hookshot\PIN" "repo=https://github.com/NotRexed/Hookshot`r`ncommit=$('a' * 40)`r`n"
    Write-File "$hsSrc\hookshot\hookshot_coop.dll" 'module'
    Write-File "$hsSrc\scripts\verify_kit.ps1" 'exit 0'
    Write-File "$hsSrc\coop-options.ini" "LICHII_COOP_PUPPET=1`r`n"
    Write-File "$hsSrc\MANIFEST.txt" "# Files`r`nStart OoT Co-op.bat`r`nscripts\start.ps1`r`nhookshot\PIN`r`nhookshot\hookshot_coop.dll`r`nVERSION.txt`r`n"
    $hsZip = Join-Path $assets 'oot-coop-v0.1.0-beta4.zip'
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $hsZip -C (Join-Path $tmp 'hsrc') 'oot-coop-v0.1.0-beta4'
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
    [System.IO.File]::WriteAllText("$hsZip.sha256", "$((Get-FileHash -LiteralPath $hsZip -Algorithm SHA256).Hash.ToLowerInvariant())  oot-coop-v0.1.0-beta4.zip`n")
    $hsList = Join-Path $tmp 'releases-hs.json'
    [System.IO.File]::WriteAllText($hsList, (ConvertTo-Json -Depth 5 @(
        @{ tag_name = 'v0.1.0-beta4'; draft = $false; html_url = 'local'; assets = @(
            @{ name = 'oot-coop-v0.1.0-beta4.zip'; browser_download_url = $hsZip; size = (Get-Item $hsZip).Length },
            @{ name = 'oot-coop-v0.1.0-beta4.zip.sha256'; browser_download_url = "$hsZip.sha256"; size = 80 }) })))
    Check (Test-KitFolder $hsSrc) 'a Hookshot-only kit is a kit folder'
    Check (-not (Test-KitFolder (Join-Path $tmp 'hsrc'))) 'its parent is not'
    Check ((Test-KitNewKit $hsSrc) -eq '') 'a Hookshot-only kit may be installed'
    Remove-Item -LiteralPath "$hsSrc\hookshot\hookshot_coop.dll"
    Check ((Test-KitNewKit $hsSrc) -match 'hookshot_coop\.dll') 'a Hookshot-only kit without the module is refused'
    Write-File "$hsSrc\hookshot\hookshot_coop.dll" 'module'
    $hk = Join-Path $tmp 'kit-to-hs'; New-OldKit $hk
    Write-File "$hk\Play-OoT-Coop.bat" 'old launcher'
    Write-File "$hk\MANIFEST.txt" "# Files`r`nHost.bat`r`nPlay-OoT-Coop.bat`r`nemulator\lichii.exe`r`ntools\gone.txt`r`nVERSION.txt`r`n"
    $desk = Join-Path $tmp 'desktop'
    New-Item -ItemType Directory -Force -Path $desk | Out-Null
    $sh = New-Object -ComObject WScript.Shell
    $lnk = $sh.CreateShortcut((Join-Path $desk 'Play OoT Co-op.lnk')); $lnk.TargetPath = "$hk\Play-OoT-Coop.bat"; $lnk.Save()
    $env:OOT_COOP_DESKTOP = $desk
    try { $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $hk -Yes -Api $hsList 2>&1 } finally { Remove-Item env:OOT_COOP_DESKTOP }
    $text = $out -join ' '
    Check ($LASTEXITCODE -eq 0 -and (Get-KitVersion $hk) -eq 'v0.1.0-beta4') "emulator kit -> Hookshot kit: updated [$(($out | Select-Object -Last 3) -join ' / ')]"
    Check (-not (Test-Path "$hk\emulator\lichii.exe") -and -not (Test-Path "$hk\Host.bat") -and -not (Test-Path "$hk\Play-OoT-Coop.bat")) 'emulator kit -> Hookshot kit: lichii.exe, Host.bat and Play-OoT-Coop.bat removed'
    Check ((Read-File "$hk\backup-v0.1.0-beta2\emulator\lichii.exe") -eq 'old exe') 'emulator kit -> Hookshot kit: the emulator is in the backup'
    Check ((Read-File "$hk\Start OoT Co-op.bat") -eq 'new launcher') 'emulator kit -> Hookshot kit: the new launcher is there'
    Test-PlayerKept $hk 'emulator kit -> Hookshot kit'
    Check ((Read-File "$hk\emulator\user\cache\shader\0100F2C0115B6000\vulkan.bin") -eq 'old shaders') 'emulator kit -> Hookshot kit: the shader cache is the player''s, left alone'
    Check ($text -match 'Longshot only' -and $text -match 'emulator.user' -and $text -match 'Start OoT Co-op\.bat') "emulator kit -> Longshot kit: says what went and what stayed [$text]"
    Check ($sh.CreateShortcut((Join-Path $desk 'Play OoT Co-op.lnk')).TargetPath -ieq "$hk\Start OoT Co-op.bat") 'emulator kit -> Hookshot kit: the desktop shortcut starts the new launcher'
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Kit $hk -Check -Api $hsList 2>&1
    Check ($LASTEXITCODE -eq 0 -and ($out -join ' ') -notmatch 'not a co-op kit') "a Hookshot-only kit can be updated again [$($out -join ' ')]"
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
