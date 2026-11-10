# Tests for the update on launch (tools/release/kit/scripts/play.ps1 with update.ps1).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\launch_test.ps1
# Builds a small kit from this repo's own kit scripts, with a stub in place of the emulator
# (a copy of Windows' rundll32.exe, which does nothing and ends at once), and a fake newer
# release in a local folder (no network). Then starts play.ps1 the way play.cmd does, for:
# a newer release (asked, yes), declined, auto_update = yes and = no, none newer, no network,
# a damaged download (the update fails, the old kit starts) and an update that did not finish.
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$release = Split-Path -Parent $PSScriptRoot
$kitSrc = Join-Path $release 'kit'
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Write-File([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text)
}
function Read-File([string]$Path) { if (Test-Path -LiteralPath $Path) { [System.IO.File]::ReadAllText($Path) } else { '<none>' } }
# A kit file as make_kit.ps1 writes it: Windows line ends, the port filled in.
function Copy-KitText([string]$From, [string]$To) {
    $text = [System.IO.File]::ReadAllText($From).Replace("`r`n", "`n").Replace('@PORT@', '27510')
    Write-File $To $text.Replace("`n", "`r`n")
}
# A kit of this repo's scripts. Tag: its release; Marker: a line its play.ps1 writes to
# played.txt when it runs, so a test sees which kit's play.ps1 started the game.
function New-Kit([string]$Dir, [string]$Tag, [string]$Marker) {
    foreach ($f in 'Host.bat', 'Join.bat', 'play.cmd', 'coop-options.ini') { Copy-KitText (Join-Path $kitSrc $f) (Join-Path $Dir $f) }
    foreach ($f in Get-ChildItem (Join-Path $kitSrc 'scripts') -Filter *.ps1) { Copy-KitText $f.FullName (Join-Path $Dir "scripts\$($f.Name)") }
    Copy-KitText (Join-Path $kitSrc 'coop.ini.template') (Join-Path $Dir 'scripts\coop.ini.template')
    $play = Join-Path $Dir 'scripts\play.ps1'
    $text = [System.IO.File]::ReadAllText($play)
    $at = $text.IndexOf('$current = Read-CoopIni $ini')
    $text = $text.Insert($at, "Add-Content -LiteralPath (Join-Path `$kit 'played.txt') -Value ('$Marker' + `$(if (`$NoUpdate) { ' NoUpdate' } else { '' }))`r`n")
    [System.IO.File]::WriteAllText($play, $text)
    $stub = [System.IO.File]::ReadAllBytes((Join-Path $env:SystemRoot 'System32\rundll32.exe'))
    New-Item -ItemType Directory -Force -Path (Join-Path $Dir 'emulator\user\config'), (Join-Path $Dir 'emulator\user\keys') | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $Dir 'emulator\lichii.exe'), $stub + [System.Text.Encoding]::ASCII.GetBytes("coop: loaded $Tag"))
    Write-File (Join-Path $Dir 'VERSION.txt') "kit: oot-coop-$Tag (release $Tag)`r`n"
    New-Item -ItemType Directory -Force -Path (Join-Path $Dir 'logs') | Out-Null
}

# --- The AMD rule and EDEN_NX2_* options (kit-paths.ps1, issue 118), with a fake card list.
. (Join-Path $kitSrc 'scripts\kit-paths.ps1')
function New-Card([string]$Name, [string]$Maker) { [pscustomobject]@{ Name = $Name; AdapterCompatibility = $Maker } }
$amdCard = { New-Card 'AMD Radeon RX 6750 XT' 'Advanced Micro Devices, Inc.' }
$nvCard = { New-Card 'NVIDIA GeForce RTX 3070' 'NVIDIA' }
$oldGpl = $env:EDEN_NX2_GPL
try {
    Check ((Get-AmdGpuKind $amdCard) -eq 'amd') 'an AMD card is found'
    Check ((Get-AmdGpuKind { New-Card 'Radeon RX 580 Series' '' }) -eq 'amd') 'a Radeon name alone is enough'
    Check ((Get-AmdGpuKind { New-Card 'Some card' 'Advanced Micro Devices, Inc.' }) -eq 'amd') 'the maker alone is enough'
    Check ((Get-AmdGpuKind $nvCard) -eq '') 'an NVIDIA card is not AMD'
    Check ((Get-AmdGpuKind { New-Card 'Intel(R) UHD Graphics' 'Intel Corporation'; New-Card 'AMD Radeon RX 7600' 'Advanced Micro Devices, Inc.' }) -eq 'amd') 'Intel and AMD: AMD'
    Check ((Get-AmdGpuKind { New-Card 'AMD Radeon(TM) Graphics' 'Advanced Micro Devices, Inc.'; New-Card 'NVIDIA GeForce RTX 5090' 'NVIDIA' }) -eq 'amd+nvidia') 'AMD and NVIDIA: told apart'
    Check ((Get-AmdGpuKind { throw 'no CIM' }) -eq '') 'a failing query counts as no AMD card'
    Check ((Get-AmdGpuKind { }) -eq '') 'no cards: no AMD card'

    $env:EDEN_NX2_GPL = $null
    $line = Set-AmdGplDefault $amdCard
    Check ($env:EDEN_NX2_GPL -eq '0' -and $line -eq 'AMD GPU found: shader libraries off (EDEN_NX2_GPL=0)') "AMD: EDEN_NX2_GPL=0 and one line [$line]"
    $env:EDEN_NX2_GPL = $null
    $line = Set-AmdGplDefault $nvCard
    Check (-not $env:EDEN_NX2_GPL -and -not $line) 'NVIDIA: nothing set, nothing said'
    $line = Set-AmdGplDefault { throw 'no CIM' }
    Check (-not $env:EDEN_NX2_GPL -and -not $line) 'a failing query: nothing set, nothing said'
    $line = Set-AmdGplDefault { New-Card 'AMD Radeon(TM) Graphics' 'Advanced Micro Devices, Inc.'; New-Card 'NVIDIA GeForce RTX 5090' 'NVIDIA' }
    Check (-not $env:EDEN_NX2_GPL -and $line -match 'NVIDIA.*EDEN_NX2_GPL=0') 'AMD and NVIDIA: left on, with a hint'
    $env:EDEN_NX2_GPL = '1'
    $line = Set-AmdGplDefault { New-Card 'AMD Radeon RX 6750 XT' '' }
    Check ($env:EDEN_NX2_GPL -eq '1' -and -not $line) 'the player''s EDEN_NX2_GPL wins over the AMD rule'
} finally {
    $env:EDEN_NX2_GPL = $oldGpl
}
$opt = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-options-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.ini')
try {
    [System.IO.File]::WriteAllText($opt, "auto_update = ask`r`nLICHII_COOP_PUPPET=1`r`n# EDEN_NX2_GPL=1`r`nEDEN_NX2_GPL = 1`r`nEDEN_OTHER=1`r`n")
    $o = Read-KitOptions $opt 6>$null
    Check ((($o.Keys | ForEach-Object { "$_=$($o[$_])" }) -join ',') -eq 'LICHII_COOP_PUPPET=1,EDEN_NX2_GPL=1') "coop-options.ini passes EDEN_NX2_* lines, not other EDEN_ ones [$(($o.Keys | ForEach-Object { "$_=$($o[$_])" }) -join ',')]"
} finally {
    Remove-Item -LiteralPath $opt -Force -ErrorAction SilentlyContinue
}

# --- The puppet pack hint (kit-paths.ps1, issue 131): sound, and a pack older than the tool.
$ph = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-pack-hint-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    $man = Join-Path $ph 'coop_puppet.json'; $tool = Join-Path $ph 'puppet_pack'
    Write-File (Join-Path $tool 'build.py') '# tool'
    (Get-Item (Join-Path $tool 'build.py')).LastWriteTimeUtc = [datetime]::UtcNow.AddHours(-2)
    Write-File $man '{"tool": "tools/puppet_pack", "actors": []}'
    Check ((Get-PuppetPackHint $man $tool) -match 'without the other players'' sounds.*make-puppet-pack\.cmd') 'a pack without "sound": run make-puppet-pack again'
    Write-File $man '{"actors": [], "sound": false}'
    Check ((Get-PuppetPackHint $man $tool) -match 'sounds') 'a --no-sound pack: the same line'
    Write-File $man '{"actors": [], "sound": true}'
    Check ($null -eq (Get-PuppetPackHint $man $tool)) 'a pack with sound, newer than the tool: nothing said'
    Check ($null -eq (Get-PuppetPackHint $man (Join-Path $ph 'no-tool'))) 'no tool folder: nothing said'
    (Get-Item (Join-Path $tool 'build.py')).LastWriteTimeUtc = [datetime]::UtcNow.AddHours(1)
    Check ((Get-PuppetPackHint $man $tool) -match 'older than this kit''s pack tool') 'a pack older than the tool: run make-puppet-pack again'
    Write-File $man '{ not json'
    Check ((Get-PuppetPackHint $man $tool) -match 'could not be read') 'a broken coop_puppet.json: a line, no throw'
} finally {
    Remove-Item -LiteralPath $ph -Recurse -Force -ErrorAction SilentlyContinue
}
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-launch-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$oldApi = $env:OOT_COOP_UPDATE_API
try {
    # The fake newer release: a kit zip, its checksum, and the release list.
    $src = Join-Path $tmp 'src'
    New-Kit (Join-Path $src 'oot-coop-v0.1.0-test4') 'v0.1.0-test4' 'new'
    $assets = Join-Path $tmp 'assets'
    New-Item -ItemType Directory -Force -Path $assets | Out-Null
    $zip = Join-Path $assets 'oot-coop-v0.1.0-test4.zip'
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $src 'oot-coop-v0.1.0-test4'
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  oot-coop-v0.1.0-test4.zip`n")
    [System.IO.File]::WriteAllText("$assets\bad.sha256", ('0' * 64) + "`n")
    function Write-List([string]$Path, [string]$Sum) {
        $json = ConvertTo-Json -Depth 5 @(@{ tag_name = 'v0.1.0-test4'; draft = $false; html_url = 'local'; assets = @(
            @{ name = 'oot-coop-v0.1.0-test4.zip'; browser_download_url = $zip; size = (Get-Item $zip).Length },
            @{ name = 'oot-coop-v0.1.0-test4.zip.sha256'; browser_download_url = $Sum; size = 80 }) })
        [System.IO.File]::WriteAllText($Path, $json)
    }
    $good = Join-Path $tmp 'releases.json'; Write-List $good "$zip.sha256"
    $damaged = Join-Path $tmp 'releases-bad.json'; Write-List $damaged "$assets\bad.sha256"

    # play.cmd -Server 127.0.0.1 -Name Saria: no questions, starts the stub. $Answer is what
    # the player types at the update question (nothing: the default).
    function Start-Play([string]$Kit, [string]$Answer, [string]$Api) {
        $env:OOT_COOP_UPDATE_API = $Api
        $in = Join-Path $tmp 'answer.txt'
        [System.IO.File]::WriteAllText($in, "$Answer`r`n")
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $script:out = cmd /c "`"$Kit\play.cmd`" -Server 127.0.0.1 -Name Saria -Port 27510 < `"$in`"" 2>&1
        $script:code = $LASTEXITCODE
        $script:seconds = $sw.Elapsed.TotalSeconds
    }
    function Get-Played([string]$Kit) { @(Get-Content -LiteralPath (Join-Path $Kit 'played.txt') -ErrorAction SilentlyContinue | ForEach-Object { $_.Trim() }) }
    function Test-Started([string]$Kit, [string]$Run) {
        Check (($out -join ' ') -match 'Emulator started') "${Run}: the game starts"
        Check ((Read-File "$Kit\emulator\user\config\coop.ini") -match '(?m)^name = Saria') "${Run}: coop.ini is written"
    }

    # A newer release, the player says yes: the kit updates and the NEW play.ps1 starts the game.
    $kit = Join-Path $tmp 'kit yes (1)'; New-Kit $kit 'v0.1.0-test3' 'old'
    Write-File "$kit\emulator\user\keys\prod.keys" 'my keys'
    Start-Play $kit 'y' $good
    Check ($code -eq 0) "asked, yes: play ran ($(($out | Select-Object -Last 1) -join ''))"
    Check (($out -join ' ') -match 'Co-op v0\.1\.0-test4 is out\. Update now\?') 'the question names the new version'
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate') 'the old play.ps1 hands over to the new one (and does not start the game itself)'
    Check ((Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'v0\.1\.0-test4') 'the kit is the new version'
    Check ((Read-File "$kit\emulator\lichii.exe") -match 'coop: loaded v0\.1\.0-test4') 'the emulator is the new one'
    Check ((Read-File "$kit\emulator\user\keys\prod.keys") -eq 'my keys') 'the keys are kept'
    Test-Started $kit 'asked, yes'

    # No key at all (an unattended start): yes.
    $kit = Join-Path $tmp 'kit default'; New-Kit $kit 'v0.1.0-test3' 'old'
    Start-Play $kit '' $good
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate') 'no answer: updated and the new kit started'

    # Declined: the old kit starts as it is.
    $kit = Join-Path $tmp 'kit no'; New-Kit $kit 'v0.1.0-test3' 'old'
    Start-Play $kit 'n' $good
    Check (((Get-Played $kit) -join '|') -eq 'old') "declined: only the old play.ps1 ran [$((Get-Played $kit) -join '|')]"
    Check ((Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'v0\.1\.0-test3') 'declined: the kit is unchanged'
    Test-Started $kit 'declined'

    # auto_update = yes: no question.
    $kit = Join-Path $tmp 'kit auto yes'; New-Kit $kit 'v0.1.0-test3' 'old'
    $o = "$kit\coop-options.ini"; [System.IO.File]::WriteAllText($o, ([System.IO.File]::ReadAllText($o) -replace 'auto_update = ask', 'auto_update = yes'))
    Start-Play $kit 'n' $good
    Check (($out -join ' ') -notmatch 'Update now\?' -and ($out -join ' ') -match 'auto_update = yes') 'auto_update = yes: no question'
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate') 'auto_update = yes: updated and the new kit started'
    Check ([System.IO.File]::ReadAllText($o) -match '(?m)^auto_update = yes') 'auto_update = yes: the player''s choice is kept'

    # auto_update = no: only says so.
    $kit = Join-Path $tmp 'kit auto no'; New-Kit $kit 'v0.1.0-test3' 'old'
    $o = "$kit\coop-options.ini"; [System.IO.File]::WriteAllText($o, ([System.IO.File]::ReadAllText($o) -replace 'auto_update = ask', 'auto_update = no'))
    Start-Play $kit 'y' $good
    Check (($out -join ' ') -match 'run Update\.bat' -and ($out -join ' ') -notmatch 'Update now\?') 'auto_update = no: says one is out, no question'
    Check (((Get-Played $kit) -join '|') -eq 'old' -and (Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'test3') 'auto_update = no: the old kit starts unchanged'

    # Nothing newer.
    $kit = Join-Path $tmp 'kit newest'; New-Kit $kit 'v0.1.0-test4' 'old'
    Start-Play $kit 'y' $good
    Check (($out -join ' ') -notmatch 'is out' -and ((Get-Played $kit) -join '|') -eq 'old') 'nothing newer: no question, the kit starts'
    Test-Started $kit 'nothing newer'

    # No network (the list cannot be read): starts as now, quickly.
    $kit = Join-Path $tmp 'kit offline'; New-Kit $kit 'v0.1.0-test3' 'old'
    Start-Play $kit 'y' (Join-Path $tmp 'no-such-list.json')
    Check (($out -join ' ') -notmatch 'is out' -and ((Get-Played $kit) -join '|') -eq 'old') 'no network: no question, the kit starts'
    Check ($seconds -lt 15) ("no network: started in {0:N1} s" -f $seconds)
    Test-Started $kit 'no network'

    # A damaged download: the update fails, nothing changes, the old kit starts.
    $kit = Join-Path $tmp 'kit damaged'; New-Kit $kit 'v0.1.0-test3' 'old'
    Start-Play $kit 'y' $damaged
    Check (($out -join ' ') -match 'damaged' -and ($out -join ' ') -match 'this version starts as it is') 'a failed update says so'
    Check (((Get-Played $kit) -join '|') -eq 'old' -and (Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'test3') 'a failed update: the old kit starts unchanged'
    Test-Started $kit 'failed update'

    # An update that did not finish (update-in-progress.txt): asked again, and done.
    $kit = Join-Path $tmp 'kit unfinished'; New-Kit $kit 'v0.1.0-test4' 'old'
    Write-File "$kit\update-in-progress.txt" 'Update.bat was updating this kit'
    Start-Play $kit 'y' $good
    Check (($out -join ' ') -match 'did not finish') 'an unfinished update is named'
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate' -and -not (Test-Path "$kit\update-in-progress.txt")) 'an unfinished update is done, then the new kit starts'

    # EDEN_NX2_GPL from coop-options.ini reaches the emulator and the AMD rule leaves it.
    $kit = Join-Path $tmp 'kit gpl'; New-Kit $kit 'v0.1.0-test4' 'old'
    Add-Content -LiteralPath "$kit\coop-options.ini" -Value 'EDEN_NX2_GPL=1'
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Check (($out -join ' ') -match 'Option: EDEN_NX2_GPL=1' -and ($out -join ' ') -notmatch 'AMD GPU found') 'EDEN_NX2_GPL in coop-options.ini is passed and wins over the AMD rule'
    Test-Started $kit 'EDEN_NX2_GPL option'

    # A puppet pack made before sound: one line asks for make-puppet-pack, and the game starts.
    $kit = Join-Path $tmp 'kit pack'; New-Kit $kit 'v0.1.0-test4' 'old'
    Write-File "$kit\emulator\user\load\0400486024DEC000\coop-puppet\coop_puppet.json" '{"actors": []}'
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Check (($out -join ' ') -match 'Puppet pack: found\. Puppet pack: made without the other players'' sounds\. Run make-puppet-pack\.cmd again') "an old puppet pack: the sound line [$(($out | Select-String 'Puppet pack') -join ' / ')]"
    Test-Started $kit 'old puppet pack'
    # The launch files run all on one line (the update may replace them while they run).
    foreach ($f in 'Host.bat', 'Join.bat', 'play.cmd') {
        $last = @(Get-Content (Join-Path $kitSrc $f) | Where-Object { $_ -and $_ -notmatch '^(@echo off|rem )' })
        Check ($last.Count -eq 1 -and $last[0] -match '& exit /b$') "$f is one command line ending in exit /b"
    }
} finally {
    $env:OOT_COOP_UPDATE_API = $oldApi
    Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($tmp, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
