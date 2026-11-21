# Tests for the update on launch (tools/release/kit/scripts/play.ps1 with update.ps1).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\launch_test.ps1
# Builds a small kit from this repo's own kit scripts, with a stub in place of the emulator
# (a copy of Windows' rundll32.exe, which does nothing and ends at once), and a fake newer
# release in a local folder (no network). Then starts play.ps1 the way play.cmd does, for:
# a newer release (asked, yes), declined, auto_update = yes and = no, none newer, no network,
# a damaged download (the update fails, the old kit starts) and an update that did not finish.
# Then "Start Squadala.bat" (start.ps1) and each check it makes before the game starts.
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
# A kit file as make_kit.ps1 writes it (its own copy function): Windows line ends, the port
# filled in, except in coop.ini.template.
. (Join-Path $release 'kit_text.ps1')
function Copy-KitFile([string]$From, [string]$To) { Copy-KitText $From $To 27510 }
# A kit of this repo's scripts. Tag: its release; Marker: a line its play.ps1 writes to
# played.txt when it runs, so a test sees which kit's play.ps1 started the game.
function New-Kit([string]$Dir, [string]$Tag, [string]$Marker) {
    foreach ($f in 'Host.bat', 'Join.bat', 'play.cmd', 'coop-options.ini') { Copy-KitFile (Join-Path $kitSrc $f) (Join-Path $Dir $f) }
    foreach ($f in Get-ChildItem (Join-Path $kitSrc 'scripts') -Filter *.ps1) { Copy-KitFile $f.FullName (Join-Path $Dir "scripts\$($f.Name)") }
    Copy-KitFile (Join-Path $kitSrc 'coop.ini.template') (Join-Path $Dir 'scripts\coop.ini.template')
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
# --- Built-in graphics only: the 30 FPS hint (kit-paths.ps1, docs/setup.md "Low frame rate").
function Test-Hint([scriptblock]$Cards) { [bool](Get-GpuSpeedHint $Cards) }
Check (Test-Hint { New-Card 'Intel(R) UHD Graphics 620' 'Intel Corporation' }) 'Intel UHD alone: hint'
Check (Test-Hint { New-Card 'Intel(R) Arc(TM) Graphics' 'Intel Corporation' }) 'Intel Arc built-in graphics: hint'
Check (Test-Hint { New-Card 'AMD Radeon(TM) Graphics' 'Advanced Micro Devices, Inc.' }) 'Ryzen built-in Radeon: hint'
Check (Test-Hint { New-Card 'AMD Radeon(TM) RX Vega 10 Graphics' 'Advanced Micro Devices, Inc.' }) 'Ryzen RX Vega 10: hint'
Check (Test-Hint { New-Card 'AMD Radeon 780M Graphics' 'Advanced Micro Devices, Inc.'; New-Card 'Microsoft Basic Display Adapter' '(Standard display types)' }) 'Radeon 780M and a basic adapter: hint'
Check ((Get-GpuSpeedHint { New-Card 'Intel(R) UHD Graphics' 'Intel Corporation' }) -match '30 FPS.*Low frame rate') 'the hint names the 30 FPS button and the setup section'
Check (-not (Test-Hint { New-Card 'Intel(R) UHD Graphics' 'Intel Corporation'; New-Card 'NVIDIA GeForce RTX 3050 Laptop GPU' 'NVIDIA' })) 'laptop with NVIDIA: no hint'
Check (-not (Test-Hint { New-Card 'AMD Radeon(TM) Graphics' 'Advanced Micro Devices, Inc.'; New-Card 'AMD Radeon RX 6600M' 'Advanced Micro Devices, Inc.' })) 'Ryzen with a Radeon RX: no hint'
Check (-not (Test-Hint { New-Card 'AMD Radeon RX 7900 XTX' 'Advanced Micro Devices, Inc.' })) 'Radeon RX: no hint'
Check (-not (Test-Hint { New-Card 'AMD Radeon RX Vega 64' 'Advanced Micro Devices, Inc.' })) 'Radeon RX Vega 64 card: no hint'
Check (-not (Test-Hint { New-Card 'Intel(R) Arc(TM) A770 Graphics' 'Intel Corporation' })) 'Intel Arc A770 card: no hint'
Check (-not (Test-Hint { New-Card 'Parsec Virtual Display Adapter' 'Parsec Cloud, Inc.' })) 'only a virtual adapter: no hint'
Check (-not (Test-Hint { })) 'no cards: no hint'
Check (-not (Test-Hint { throw 'no CIM' })) 'a failing query: no hint'
$fixedList = @(New-Card 'Intel(R) UHD Graphics' 'Intel Corporation')
Check (Test-Hint ({ $fixedList }.GetNewClosure())) 'a card list read once (play.ps1) works as the query'

$opt = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-options-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.ini')
try {
    [System.IO.File]::WriteAllText($opt, "auto_update = ask`r`nLICHII_COOP_PUPPET=1`r`n# EDEN_NX2_GPL=1`r`nEDEN_NX2_GPL = 1`r`nEDEN_OTHER=1`r`n")
    $o = Read-KitOptions $opt 6>$null
    Check ((($o.Keys | ForEach-Object { "$_=$($o[$_])" }) -join ',') -eq 'LICHII_COOP_PUPPET=1,EDEN_NX2_GPL=1') "coop-options.ini passes EDEN_NX2_* lines, not other EDEN_ ones [$(($o.Keys | ForEach-Object { "$_=$($o[$_])" }) -join ',')]"
    # server_allow / server_bind (net-security): read for the server, never set for the emulator.
    [System.IO.File]::WriteAllText($opt, "# server_allow = 9.9.9.9`r`nserver_allow = 25.1.2.3, 26.0.0.0/8 `r`nserver_bind=25.1.2.3`r`nLICHII_COOP_PUPPET=1`r`n")
    $said = Read-KitOptions $opt 6>&1 | Where-Object { $_ -is [System.Management.Automation.InformationRecord] }
    $o = Read-KitOptions $opt 6>$null
    Check ((($o.Keys) -join ',') -eq 'LICHII_COOP_PUPPET' -and -not $said) 'server_allow and server_bind are not emulator switches and are not warned about'
    $a = Read-KitServerAccess $opt
    Check ($a.Allow -eq '25.1.2.3, 26.0.0.0/8' -and $a.Bind -eq '25.1.2.3') "server_allow and server_bind are read [$($a.Allow)|$($a.Bind)]"
    [System.IO.File]::WriteAllText($opt, "# server_allow = 9.9.9.9`r`n")
    $a = Read-KitServerAccess $opt
    Check ($a.Allow -eq '' -and $a.Bind -eq '') 'server_allow behind # is off'
    Check (-not $a.Pvp) 'no pvp line: PvP is off'
    # pvp (the host's server only): read for the server, never set for the emulator.
    [System.IO.File]::WriteAllText($opt, "pvp = on`r`nLICHII_COOP_PUPPET=1`r`n")
    $said = Read-KitOptions $opt 6>&1 | Where-Object { $_ -is [System.Management.Automation.InformationRecord] }
    $o = Read-KitOptions $opt 6>$null
    Check ((($o.Keys) -join ',') -eq 'LICHII_COOP_PUPPET' -and -not $said) 'pvp is not an emulator switch and is not warned about'
    Check ((Read-KitServerAccess $opt).Pvp) 'pvp = on is read'
    foreach ($off in 'pvp = off', '# pvp = on', 'pvp = maybe', 'pvp =') {
        [System.IO.File]::WriteAllText($opt, "$off`r`n")
        Check (-not (Read-KitServerAccess $opt).Pvp) "'$off': PvP is off"
    }
    [System.IO.File]::WriteAllText($opt, "# server_allow = 9.9.9.9`r`n")
    $a = Read-KitServerAccess $opt
    # server_password (join-password): $null when no line sets it or it is empty (Host.bat makes one).
    Check ($null -eq $a.Password) 'no server_password line: Password is $null (Host.bat makes one)'
    [System.IO.File]::WriteAllText($opt, "# --- Who may join ---`r`n# server_password = `r`nLICHII_COOP_PUPPET=1`r`n")
    Check ($null -eq (Read-KitServerAccess $opt).Password) 'server_password behind # is not set'
    Set-KitServerPassword $opt 'hunter2'
    $text = [System.IO.File]::ReadAllText($opt)
    Check ($text -match '(?m)^server_password = hunter2\r?$' -and $text -notmatch '# server_password' -and $text -match 'LICHII_COOP_PUPPET=1') "Set-KitServerPassword takes the place of the # line [$text]"
    Check ((Read-KitServerAccess $opt).Password -eq 'hunter2') 'server_password is read back'
    $said = Read-KitOptions $opt 6>&1 | Where-Object { $_ -is [System.Management.Automation.InformationRecord] }
    Check (-not $said -and -not (Read-KitOptions $opt 6>$null).Contains('server_password')) 'server_password is not an emulator switch and is not warned about'
    Set-KitServerPassword $opt ''
    $p = (Read-KitServerAccess $opt).Password
    Check ($null -eq $p -and ([regex]::Matches([System.IO.File]::ReadAllText($opt), 'server_password')).Count -eq 1) "an empty server_password line is not 'no password': Host.bat makes one (every session has one) [$p]"
    # color (puppet-colors): passed as LICHII_COOP_COLOR when it is a colour, with a comment after it too.
    . (Join-Path $kitSrc 'scripts\coop-ini.ps1')
    foreach ($good in 'blue', 'Purple', '#FF8020', 'ff8020') { Check (Test-CoopColor $good) "a colour: $good" }
    foreach ($bad in 'blurple', '#FF802', '#GG8020', '') { Check (-not (Test-CoopColor $bad)) "not a colour: '$bad'" }
    # privacy-name (0.11.1 player report): a new player's offered name is random, a valid co-op name, never this PC's names.
    foreach ($i in 1..50) {
        $n = New-CoopName
        Check ((Test-CoopName $n) -and $n -match '^[A-Za-z]+-\d{4}$' -and $n -ne $env:USERNAME -and $n -ne $env:COMPUTERNAME) "a random name: $n"
    }
    Check ((Read-File (Join-Path $kitSrc 'scripts\play.ps1')) -match 'if \(\$newName\) \{ \$Name = New-CoopName \}') 'play.ps1 offers a new player a random name'
    # The forms players type (10.1 report: a hex colour did nothing): one text for every reader.
    foreach ($pair in @(@('#FF8020', '#FF8020'), @('ff8020', '#FF8020'), @('"#ff8020"', '#FF8020'), @("'0xFF8020'", '#FF8020'),
                         @('#F82', '#FF8822'), @(' Orange ', 'orange'), @('blurple', ''),
                         @('#FF8020FF', '#FF8020'), @('FF802080', '#FF8020'), @('rgb(255, 128, 32)', '#FF8020'), @('rgb(300,0,0)', ''))) {
        Check ((ConvertTo-CoopColor $pair[0]) -eq $pair[1]) "colour '$($pair[0])' reads as '$($pair[1])'"
    }
    foreach ($hex in '#FF8020', 'FF8020', '"#FF8020"', '0xFF8020', '#FF8020FF', 'rgb(255,128,32)') { Check ((Get-KitTunic $hex) -eq 'orange') "own tunic of $hex is orange" }
    $optFile = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-color-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.ini')
    [System.IO.File]::WriteAllText($optFile, "color = `"#ff8020`"  # mine`r`n")
    Check ((Read-KitOptions $optFile)['LICHII_COOP_COLOR'] -eq '#FF8020') 'coop-options.ini color = "#ff8020" passes #FF8020 on'
    Remove-Item -LiteralPath $optFile -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($opt, "# color = red`r`ncolor = #FF8020  # mine`r`nLICHII_COOP_PUPPET=1`r`n")
    $said = Read-KitOptions $opt 6>&1 | Where-Object { $_ -is [System.Management.Automation.InformationRecord] }
    $o = Read-KitOptions $opt 6>$null
    Check ($o['LICHII_COOP_COLOR'] -eq '#FF8020' -and $o['LICHII_COOP_PUPPET'] -eq '1' -and -not $said) "color is passed as LICHII_COOP_COLOR [$($o['LICHII_COOP_COLOR'])]"
    [System.IO.File]::WriteAllText($opt, "color = blurple`r`n")
    $said = Read-KitOptions $opt 6>&1 | Where-Object { $_ -is [System.Management.Automation.InformationRecord] }
    Check ($said -and -not (Read-KitOptions $opt 6>$null).Contains('LICHII_COOP_COLOR')) 'a bad color is said and not passed'
    [System.IO.File]::WriteAllText($opt, "color =`r`n# color = blue`r`n")
    Check (-not (Read-KitOptions $opt 6>$null).Contains('LICHII_COOP_COLOR')) 'an empty or commented color passes nothing'
    [System.IO.File]::WriteAllText($opt, "auto_update = ask`r`n")
    Set-KitServerPassword $opt 'x'
    Check ([System.IO.File]::ReadAllText($opt) -eq "auto_update = ask`r`nserver_password = x`r`n") 'no server_password line at all: added at the end'
} finally {
    Remove-Item -LiteralPath $opt -Force -ErrorAction SilentlyContinue
}

# --- The puppet pack hint (kit-paths.ps1, issue 131): sound, and the pack tool's hash.
$ph = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-pack-hint-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    # The same files and hash as test_tool_hash in tools\puppet_pack\tests: both sides agree.
    $tool = Join-Path $ph 'puppet_pack'
    New-Item -ItemType Directory -Force -Path $tool | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $tool 'b_c.py'), [System.Text.Encoding]::ASCII.GetBytes("print(1)`n"))
    [System.IO.File]::WriteAllBytes((Join-Path $tool 'Zeta.py'), [System.Text.Encoding]::ASCII.GetBytes("x`r`n"))
    [System.IO.File]::WriteAllBytes((Join-Path $tool '__init__.py'), [byte[]]@())
    [System.IO.File]::WriteAllBytes((Join-Path $tool 'a.pyc'), [System.Text.Encoding]::ASCII.GetBytes('no'))
    $hash = 'b9d822a64cc6285dd1aa300d77f64ea0c86f775240885dad79ae4f2f7694f5e0'
    Check ((Get-PuppetToolHash $tool) -eq $hash) "the pack tool hash is build.py's [$(Get-PuppetToolHash $tool)]"
    $man = Join-Path $ph 'coop_puppet.json'
    Write-File (Join-Path $ph 'romfs\x') 'tunic'   # (the "x" the tunics below name: a complete pack)
    Write-File $man '{"tool": "tools/puppet_pack", "actors": []}'
    Check ((Get-PuppetPackHint $man $tool) -match 'without the other players'' sounds.*make-puppet-pack\.cmd') 'a pack without "sound": run make-puppet-pack again'
    Write-File $man '{"actors": [], "sound": false}'
    Check ((Get-PuppetPackHint $man $tool) -match 'sounds') 'a --no-sound pack: the same line'
    Write-File $man "{`"actors`": [], `"sound`": true, `"tunics`": {`"Red`": [`"x`"]}, `"tool_sha256`": `"$hash`"}"
    (Get-Item (Join-Path $tool 'b_c.py')).LastWriteTimeUtc = [datetime]::UtcNow.AddHours(10)
    Check ($null -eq (Get-PuppetPackHint $man $tool)) 'the same tool, its files dated hours ahead (a zip from another time zone): nothing said'
    Check ($null -eq (Get-PuppetPackHint $man (Join-Path $ph 'no-tool'))) 'no tool folder: nothing said'
    Write-File $man '{"actors": [], "sound": true, "tunics": {"Red": ["x"]}}'
    Check ((Get-PuppetPackHint $man $tool) -match 'older version of the pack tool') 'sound and no tool hash (a pack from before the hash): made again (pack-retry)'
    Check ($null -eq (Get-PuppetPackHint $man (Join-Path $ph 'no-tool'))) 'no tool hash and no tool folder: nothing said'
    Write-File $man '{"actors": [], "sound": true}'
    Check ((Get-PuppetPackHint $man $tool) -match 'without the tunic colours.*green.*make-puppet-pack\.cmd') 'a pack from before the tunics: run make-puppet-pack again (#202)'
    Write-File $man "{`"actors`": [], `"sound`": true, `"tunics`": {}, `"tunics_off`": true, `"tool_sha256`": `"$hash`"}"
    Check ($null -eq (Get-PuppetPackHint $man $tool)) 'a --tunics none pack: no tunic line'
    Write-File $man '{"actors": [], "sound": true, "tunics": {}, "tool_sha256": "0000"}'
    Check ((Get-PuppetPackHint $man $tool) -match 'without the tunic colours') 'a pack with no tunics made: the tunic line first'
    Write-File $man '{"actors": [], "sound": true, "tunics": {"Red": ["x"]}, "tool_sha256": "0000"}'
    Check ((Get-PuppetPackHint $man $tool) -match 'another version of the pack tool') 'another tool hash: run make-puppet-pack again'
    # An incomplete pack (pack-retry: a tool stopped half way, antivirus): a file the manifest names is gone.
    Write-File (Join-Path $ph 'coop_look\gear.bin') 'gear'
    Write-File $man "{`"actors`": [], `"sound`": true, `"tunics`": {`"Red`": [`"x`"]}, `"look_files`": {`"gear`": `"coop_look/gear.bin`", `"red`": `"coop_look/red.bin`"}, `"tool_sha256`": `"$hash`"}"
    $h = Get-PuppetPackHint $man $tool
    Check ($h -match 'incomplete \(coop_look/red\.bin is missing\)') "a look file gone: incomplete, named [$h]"
    Check ((Test-KitPuppetPackStale $man $tool) -and (Test-KitPuppetPackStale $man (Join-Path $ph 'no-tool'))) 'a look file gone: stale, with or without the tool (the start makes it again)'
    Write-File (Join-Path $ph 'coop_look\red.bin') 'red'
    Check ($null -eq (Get-PuppetPackHint $man $tool)) 'every look file there: nothing said'
    Write-File $man "{`"actors`": [], `"sound`": true, `"tunics`": {`"Red`": [`"x`", `"Tex/gone.bntx.zs`"], `"Blue`": [`"y`"]}, `"tool_sha256`": `"$hash`"}"
    $h = Get-PuppetPackHint $man $tool
    Check ($h -match 'incomplete \(romfs/Tex/gone\.bntx\.zs is missing and 1 more\)') "tunic files gone: incomplete, the first named and how many more [$h]"
    Check (@(Get-KitPuppetPackGaps $man ([System.IO.File]::ReadAllText($man) | ConvertFrom-Json)).Count -eq 2) 'pack gaps: two tunic files gone'
    Write-File $man '{ not json'
    Check ((Get-PuppetPackHint $man $tool) -match 'could not be read') 'a broken coop_puppet.json: a line, no throw'
    Check ((Get-PuppetPackHint (Join-Path $ph 'none.json') $tool) -match 'could not be read') 'a missing coop_puppet.json: a line, no throw'
    # A Hookshot-only kit has no make-puppet-pack.cmd: play.ps1 points the hints at the launcher.
    $packRemake = $script:KitPackRemake
    $script:KitPackRemake = 'Start "Start Squadala.bat" again: it makes the pack again.'
    Write-File $man '{"actors": [], "sound": true}'
    $h = Get-PuppetPackHint $man $tool
    Check ($h -match 'Start Squadala\.bat' -and $h -notmatch 'make-puppet-pack') "a Hookshot-only kit: the pack hint names the launcher [$h]"
    $script:KitPackRemake = $packRemake
    # Your own Link in your colour (#202): the nearest preset, as coop_colors.cpp TunicOf.
    Check ((Get-KitTunic 'red') -eq 'red') 'tunic: red'
    Check ((Get-KitTunic ' Blue ') -eq 'blue') 'tunic: a name, any case'
    Check ((Get-KitTunic '#FF3333') -eq 'red') 'tunic: #RRGGBB nearest'
    Check ((Get-KitTunic 'A0A0A0') -eq 'white') 'tunic: gray is white, as the emulator says'
    Check ((Get-KitTunic 'gray') -eq 'white') 'tunic: the name gray too'
    Check ((Get-KitTunic 'green') -eq 'green') 'tunic: green'
    Check ((Get-KitTunic 'blurple') -eq '') 'tunic: not a colour'
    Check ((Get-KitTunic '') -eq '') 'tunic: none chosen'
    $sp = Join-Path $ph 'self-pack'
    New-Item -ItemType Directory -Force -Path (Join-Path $sp 'coop_self') | Out-Null
    Write-File (Join-Path $sp 'coop_self\red.bntx.zs') 'RED'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
    $tile = Join-Path $sp 'romfs\Tex\Angin_Tile_A_Alb.bntx.zs'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red') 'self tunic: red put in'
    Check ((Test-Path -LiteralPath $tile) -and [System.IO.File]::ReadAllText($tile) -eq 'RED') 'self tunic: the red tile over the game''s'
    Check ((Set-KitSelfTunic $sp 'blue') -eq '') 'self tunic: no blue in the pack: none'
    Check (-not (Test-Path -LiteralPath $tile)) 'self tunic: and the old copy taken away'
    [void](Set-KitSelfTunic $sp 'red')
    Check ((Set-KitSelfTunic $sp 'green') -eq '' -and -not (Test-Path -LiteralPath $tile)) 'self tunic: green: the game''s own tile'
    [void](Set-KitSelfTunic $sp 'red')
    Check ((Set-KitSelfTunic $sp '') -eq '' -and -not (Test-Path -LiteralPath $tile)) 'self tunic: no colour: the game''s own tile'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
    Check ((Set-KitSelfTunic $sp 'red') -eq '' -and -not (Test-Path -LiteralPath $tile)) 'self tunic: no green puppet in the pack: none (green players would wear yours)'
    Write-File (Join-Path $sp 'coop_puppet.json') '{ not json'
    Check ((Set-KitSelfTunic $sp 'red') -eq '') 'self tunic: a broken coop_puppet.json: none, no throw'
    Check ((Set-KitSelfTunic (Join-Path $ph 'no-pack') 'red') -eq '') 'self tunic: no pack: none, no throw'
    # the dark tile (the child's tunic body, #202): copied with the main one, taken away with it
    Write-File (Join-Path $sp 'coop_self\red.dark.bntx.zs') 'REDDARK'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}, "self_dark_tiles": {"red": "coop_self/red.dark.bntx.zs"}}'
    $dark = Join-Path $sp 'romfs\Tex\Angin_Tile_A_Dark_Alb.bntx.zs'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and (Test-Path -LiteralPath $dark) -and [System.IO.File]::ReadAllText($dark) -eq 'REDDARK') 'self tunic: the red dark tile over the game''s'
    Check ((Set-KitSelfTunic $sp 'green') -eq '' -and -not (Test-Path -LiteralPath $dark) -and -not (Test-Path -LiteralPath $tile)) 'self tunic: green: the game''s own dark tile too'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
    Write-File $dark 'STALE'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and -not (Test-Path -LiteralPath $dark)) 'self tunic: an older pack without dark tiles: no stale dark copy'
    # self_files (#202 adult): every tunic texture of your colour, all taken away on a change
    Write-File (Join-Path $sp 'coop_self\red.vest.bntx.zs') 'REDVEST'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_files": {"red": {"Tex/Angin_Tile_A_Alb.bntx.zs": "coop_self/red.bntx.zs", "Tex/Angin_Tile_A_Dark_Alb.bntx.zs": "coop_self/red.dark.bntx.zs", "Tex/Mt_Vest_Player_Adult_Alb.bntx.zs": "coop_self/red.vest.bntx.zs", "../evil.bntx.zs": "coop_self/red.vest.bntx.zs"}}}'
    $vest = Join-Path $sp 'romfs\Tex\Mt_Vest_Player_Adult_Alb.bntx.zs'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and [System.IO.File]::ReadAllText($vest) -eq 'REDVEST' -and [System.IO.File]::ReadAllText($dark) -eq 'REDDARK' -and [System.IO.File]::ReadAllText($tile) -eq 'RED') 'self tunic: self_files: tile, dark tile and vest put in'
    Check (-not (Test-Path -LiteralPath (Join-Path $sp 'evil.bntx.zs'))) 'self tunic: self_files: a path outside Tex/ is not written'
    Check ((Set-KitSelfTunic $sp 'green') -eq '' -and -not (Test-Path -LiteralPath $vest) -and -not (Test-Path -LiteralPath $dark) -and -not (Test-Path -LiteralPath $tile)) 'self tunic: self_files: green takes every copy away'
    # the adult's model copy (bigrun5): from the pack's romfs over Model/Player_Adult
    New-Item -ItemType Directory -Force -Path (Join-Path $sp 'romfs\Model') | Out-Null
    Write-File (Join-Path $sp 'romfs\Model\CoopTunic_Adult_Red.Player_Adult.bfres.zs') 'REDMODEL'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_files": {"red": {"Tex/Angin_Tile_A_Alb.bntx.zs": "coop_self/red.bntx.zs", "Model/Player_Adult.Player_Adult.bfres.zs": "romfs/Model/CoopTunic_Adult_Red.Player_Adult.bfres.zs", "Model/Other.bfres.zs": "coop_self/red.bntx.zs"}}}'
    $model = Join-Path $sp 'romfs\Model\Player_Adult.Player_Adult.bfres.zs'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and [System.IO.File]::ReadAllText($model) -eq 'REDMODEL') 'self tunic: self_files: the adult model copy put in'
    Check (-not (Test-Path -LiteralPath (Join-Path $sp 'romfs\Model\Other.bfres.zs'))) 'self tunic: self_files: no other model path written'
    Check ((Set-KitSelfTunic $sp '') -eq '' -and -not (Test-Path -LiteralPath $model) -and (Test-Path -LiteralPath (Join-Path $sp 'romfs\Model\CoopTunic_Adult_Red.Player_Adult.bfres.zs'))) 'self tunic: self_files: no colour takes the model copy away, the puppet''s stays'
    # the child's model copy too (colour-agree), and a copy of it left from a start in a colour
    Write-File (Join-Path $sp 'romfs\Model\CoopTunic_Child_Red.Player_Child.bfres.zs') 'REDCHILD'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_files": {"red": {"Tex/Angin_Tile_A_Alb.bntx.zs": "coop_self/red.bntx.zs", "Model/Player_Child.Player_Child.bfres.zs": "romfs/Model/CoopTunic_Child_Red.Player_Child.bfres.zs"}}}'
    $childModel = Join-Path $sp 'romfs\Model\Player_Child.Player_Child.bfres.zs'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and [System.IO.File]::ReadAllText($childModel) -eq 'REDCHILD') 'self tunic: self_files: the child model copy put in'
    Check ((Set-KitSelfTunic $sp 'green') -eq '' -and -not (Test-Path -LiteralPath $childModel)) 'self tunic: green takes the child model copy away'
    [void](Set-KitSelfTunic $sp 'red')
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and -not (Test-Path -LiteralPath $childModel)) 'self tunic: an older pack: no stale child model copy'
    # colour-agree packs: your own S textures and model copies, no game-name tile in self_files
    Write-File (Join-Path $sp 'coop_self\red.Angin_Tile_S_Alb.bntx.zs') 'REDS'
    Write-File (Join-Path $sp 'coop_self\red.Player_Child.bfres.zs') 'REDCHILDS'
    Write-File (Join-Path $sp 'coop_self\black.Angin_Tile_S_Alb.bntx.zs') 'BLACKS'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_files": {"red": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/red.Angin_Tile_S_Alb.bntx.zs", "Model/Player_Child.Player_Child.bfres.zs": "coop_self/red.Player_Child.bfres.zs"}, "black": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/black.Angin_Tile_S_Alb.bntx.zs"}}}'
    $sTile = Join-Path $sp 'romfs\Tex\Angin_Tile_S_Alb.bntx.zs'
    Check ((Set-KitSelfTunic $sp 'black') -eq 'black' -and [System.IO.File]::ReadAllText($sTile) -eq 'BLACKS' -and -not (Test-Path -LiteralPath $childModel) -and -not (Test-Path -LiteralPath $tile)) 'self tunic: S set: black put in, no game-name tile'
    Check ((Set-KitSelfTunic $sp 'pink') -eq '' -and -not (Test-Path -LiteralPath $sTile)) 'self tunic: S set: a colour the pack lacks takes the stale black away'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and [System.IO.File]::ReadAllText($sTile) -eq 'REDS' -and [System.IO.File]::ReadAllText($childModel) -eq 'REDCHILDS') 'self tunic: S set: red tile and own child model'
    Check ((Set-KitSelfTunic $sp '') -eq '' -and -not (Test-Path -LiteralPath $sTile) -and -not (Test-Path -LiteralPath $childModel)) 'self tunic: S set: no colour takes them away'
    # npc-colour: a pack with green's own copies: green and no colour wear them (the game's tunic textures
    # are on Mido's and Saria's models, so your own Link never wears the game's own)
    Write-File (Join-Path $sp 'coop_self\green.Angin_Tile_S_Alb.bntx.zs') 'GREENS'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_files": {"red": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/red.Angin_Tile_S_Alb.bntx.zs", "Model/Player_Child.Player_Child.bfres.zs": "coop_self/red.Player_Child.bfres.zs"}, "green": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/green.Angin_Tile_S_Alb.bntx.zs"}}}'
    foreach ($c in 'green', '', 'nope') {
        [void](Set-KitSelfTunic $sp 'red')
        Check ((Set-KitSelfTunic $sp $c) -eq 'green' -and [System.IO.File]::ReadAllText($sTile) -eq 'GREENS' -and -not (Test-Path -LiteralPath $childModel)) "self tunic: green's own copies for '$c'"
    }
    # -Painted (Longshot): the module paints your exact colour on green's files, so a pack with green's own
    # files and coop_look/green.bin gets green's whatever the colour; the emulator (no -Painted) your colour's
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_files": {"red": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/red.Angin_Tile_S_Alb.bntx.zs", "Model/Player_Child.Player_Child.bfres.zs": "coop_self/red.Player_Child.bfres.zs"}, "green": {"Tex/Angin_Tile_S_Alb.bntx.zs": "coop_self/green.Angin_Tile_S_Alb.bntx.zs"}}, "look_files": {"green": "coop_look/green.bin"}}'
    Check ((Set-KitSelfTunic $sp 'red' -Painted) -eq 'red' -and [System.IO.File]::ReadAllText($sTile) -eq 'REDS') 'self tunic: -Painted, no coop_look/green.bin on disk: your colour''s files'
    New-Item -ItemType Directory -Force -Path (Join-Path $sp 'coop_look') | Out-Null
    Write-File (Join-Path $sp 'coop_look\green.bin') 'LOOK'
    foreach ($c in 'red', '#FF3333', '', 'green') {
        [void](Set-KitSelfTunic $sp 'red')
        Check ((Set-KitSelfTunic $sp $c -Painted) -eq 'green' -and [System.IO.File]::ReadAllText($sTile) -eq 'GREENS' -and -not (Test-Path -LiteralPath $childModel)) "self tunic: -Painted: green's files for '$c'"
    }
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and [System.IO.File]::ReadAllText($sTile) -eq 'REDS') 'self tunic: no -Painted (the emulator): your colour''s files'
    $oldColor = $env:LICHII_COOP_COLOR; $oldTunics = $env:LICHII_COOP_TUNICS; $oldSelf = $env:LICHII_COOP_SELF_TUNIC
    try {
        $env:LICHII_COOP_COLOR = 'red'; $env:LICHII_COOP_TUNICS = $null
        $said = @(Use-KitPuppetPack (Join-Path $sp 'coop_puppet.json') (Join-Path $ph 'no-tool') @('MISSING') -Painted 6>&1)
        Check ($said[-1] -eq $true -and $env:LICHII_COOP_SELF_TUNIC -eq 'green' -and [System.IO.File]::ReadAllText($sTile) -eq 'GREENS' -and (($said -join '|') -match 'painted on in the game')) "pack step -Painted: green's files, your colour painted [$($said -join '|')]"
    } finally { $env:LICHII_COOP_COLOR = $oldColor; $env:LICHII_COOP_TUNICS = $oldTunics; $env:LICHII_COOP_SELF_TUNIC = $oldSelf }
    Remove-Item -LiteralPath (Join-Path $sp 'coop_look') -Recurse -Force
    # a pack made again by an older tool (self_tiles only): the newer copies do not stay
    [void](Set-KitSelfTunic $sp 'red')
    Write-File $vest 'STALEVEST'
    Write-File (Join-Path $sp 'romfs\Tex\Mt_Angin_Acs_Player_Adult_Alb.bntx.zs') 'STALEACS'
    Write-File (Join-Path $sp 'coop_puppet.json') '{"tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
    Check ((Set-KitSelfTunic $sp 'red') -eq 'red' -and -not (Test-Path -LiteralPath $model) -and -not (Test-Path -LiteralPath $vest) -and -not (Test-Path -LiteralPath (Join-Path $sp 'romfs\Tex\Mt_Angin_Acs_Player_Adult_Alb.bntx.zs')) -and [System.IO.File]::ReadAllText($tile) -eq 'RED') 'self tunic: an older pack: no stale model, vest or acs copy'
    # Use-KitPuppetPack: one pack step for the emulator and Hookshot (play.ps1, G5)
    $oldEnv = @{}; foreach ($n in 'LICHII_COOP_COLOR', 'LICHII_COOP_TUNICS', 'LICHII_COOP_SELF_TUNIC', 'LICHII_COOP_PUPPET') { $oldEnv[$n] = [Environment]::GetEnvironmentVariable($n) }
    try {
        Write-File (Join-Path $sp 'coop_puppet.json') '{"sound": true, "tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
        $env:LICHII_COOP_COLOR = 'red'; $env:LICHII_COOP_TUNICS = $null; $env:LICHII_COOP_SELF_TUNIC = $null; $env:LICHII_COOP_PUPPET = '1'
        $said = @(Use-KitPuppetPack (Join-Path $sp 'coop_puppet.json') (Join-Path $ph 'no-tool') @('MISSING') 6>&1)
        Check ($said[-1] -eq $true -and $env:LICHII_COOP_SELF_TUNIC -eq 'red' -and [System.IO.File]::ReadAllText($tile) -eq 'RED') "pack step: found, red: LICHII_COOP_SELF_TUNIC=red and the tile in [$($said -join '|')]"
        Check ((($said -join '|') -match 'Puppet pack: found\.') -and (($said -join '|') -match 'Your Link: the red tunic') -and $env:LICHII_COOP_PUPPET -eq '1') 'pack step: found: the lines said, puppets stay on'
        $env:LICHII_COOP_TUNICS = '0'
        $said = @(Use-KitPuppetPack (Join-Path $sp 'coop_puppet.json') (Join-Path $ph 'no-tool') @('MISSING') 6>&1)
        Check (-not $env:LICHII_COOP_SELF_TUNIC -and -not (Test-Path -LiteralPath $tile)) 'pack step: LICHII_COOP_TUNICS=0: no own colour, the game''s tile'
        $env:LICHII_COOP_TUNICS = $null
        Write-File (Join-Path $sp 'coop_puppet.json') '{"sound": true, "tunics": {}}'
        $said = @(Use-KitPuppetPack (Join-Path $sp 'coop_puppet.json') (Join-Path $ph 'no-tool') @('MISSING') 6>&1)
        Check ((($said -join '|') -match 'without the tunic colours') -and (($said -join '|') -match 'Your Link stays green') -and -not $env:LICHII_COOP_SELF_TUNIC) "pack step: an old pack: the hint and 'stays green' [$($said -join '|')]"
        # Set-KitDefaultColor: no colour chosen = the colour of the likely player number, own Link included
        $env:LICHII_COOP_COLOR = $null
        $said = @(Set-KitDefaultColor 'host' '127.0.0.1' 6>&1)
        Check ($said[-1] -eq 'blue' -and $env:LICHII_COOP_COLOR -eq 'blue' -and (($said -join '|') -match 'player 1')) "default colour: the host is blue [$($said -join '|')]"
        Write-File (Join-Path $sp 'coop_puppet.json') '{"sound": true, "tunics": {"blue": ["x"], "green": ["y"]}, "self_tiles": {"blue": "coop_self/blue.bntx.zs"}}'
        Write-File (Join-Path $sp 'coop_self\blue.bntx.zs') 'BLUE'
        [void](Use-KitPuppetPack (Join-Path $sp 'coop_puppet.json') (Join-Path $ph 'no-tool') @('MISSING') 6>&1)
        Check ($env:LICHII_COOP_SELF_TUNIC -eq 'blue' -and [System.IO.File]::ReadAllText($tile) -eq 'BLUE') 'default colour: the host''s own Link wears the blue tile'
        $env:LICHII_COOP_COLOR = $null
        Check ((Set-KitDefaultColor 'join' '203.0.113.7:27510' 6>$null) -eq 'red' -and $env:LICHII_COOP_COLOR -eq 'red') 'default colour: a joiner is red'
        $env:LICHII_COOP_COLOR = $null
        Check ((Set-KitDefaultColor 'join' '127.0.0.1' 6>$null) -eq 'red') 'default colour: a relay join (127.0.0.1) is no host: red'
        Check ((Set-KitDefaultColor 'join' '10.0.0.2' 6>$null) -eq '' -and $env:LICHII_COOP_COLOR -eq 'red') 'default colour: one already set is kept'
        $env:LICHII_COOP_COLOR = 'purple'
        Check ((Set-KitDefaultColor 'host' '' 6>$null) -eq '' -and $env:LICHII_COOP_COLOR -eq 'purple') 'default colour: a chosen colour is kept'
        $env:LICHII_COOP_COLOR = $null; $env:LICHII_COOP_TUNICS = 'off'
        Check ((Set-KitDefaultColor 'host' '' 6>$null) -eq '' -and -not $env:LICHII_COOP_COLOR) 'default colour: LICHII_COOP_TUNICS=off: none'
        $env:LICHII_COOP_TUNICS = $null
        Check ((Set-KitDefaultColor '' '' 6>$null) -eq '' -and -not $env:LICHII_COOP_COLOR) 'default colour: alone (no server): none'
        $env:LICHII_COOP_SELF_TUNIC = 'red'
        $said = @(Use-KitPuppetPack (Join-Path $ph 'no-pack\coop_puppet.json') (Join-Path $ph 'no-tool') @('MISSING 1', 'MISSING 2') 6>&1)
        Check ($said[-1] -eq $false -and (($said -join '|') -match 'MISSING 1\|MISSING 2') -and -not $env:LICHII_COOP_PUPPET -and -not $env:LICHII_COOP_SELF_TUNIC) "pack step: no pack: the given lines, puppets and own colour off [$($said -join '|')]"
    } finally {
        foreach ($n in $oldEnv.Keys) { [Environment]::SetEnvironmentVariable($n, $oldEnv[$n]) }
    }
} finally {
    Remove-Item -LiteralPath $ph -Recurse -Force -ErrorAction SilentlyContinue
}
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-launch-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$oldApi = $env:OOT_COOP_UPDATE_API
try {
    # The fake newer release: a kit zip, its checksum, and the release list.
    $src = Join-Path $tmp 'src'
    New-Kit (Join-Path $src 'oot-coop-v0.1.0-beta4') 'v0.1.0-beta4' 'new'
    $assets = Join-Path $tmp 'assets'
    New-Item -ItemType Directory -Force -Path $assets | Out-Null
    $zip = Join-Path $assets 'oot-coop-v0.1.0-beta4.zip'
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $src 'oot-coop-v0.1.0-beta4'
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  oot-coop-v0.1.0-beta4.zip`n")
    [System.IO.File]::WriteAllText("$assets\bad.sha256", ('0' * 64) + "`n")
    function Write-List([string]$Path, [string]$Sum) {
        $json = ConvertTo-Json -Depth 5 @(@{ tag_name = 'v0.1.0-beta4'; draft = $false; html_url = 'local'; assets = @(
            @{ name = 'oot-coop-v0.1.0-beta4.zip'; browser_download_url = $zip; size = (Get-Item $zip).Length },
            @{ name = 'oot-coop-v0.1.0-beta4.zip.sha256'; browser_download_url = $Sum; size = 80 }) })
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
    $kit = Join-Path $tmp 'kit yes (1)'; New-Kit $kit 'v0.1.0-beta3' 'old'
    Write-File "$kit\emulator\user\keys\prod.keys" 'my keys'
    Start-Play $kit 'y' $good
    Check ($code -eq 0) "asked, yes: play ran ($(($out | Select-Object -Last 1) -join ''))"
    Check (($out -join ' ') -match 'Co-op v0\.1\.0-beta4 is out\. Update now\?') 'the question names the new version'
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate') 'the old play.ps1 hands over to the new one (and does not start the game itself)'
    # (a flake here once had no trace: what the update said, when it did not hand over)
    if (((Get-Played $kit) -join '|') -ne 'new NoUpdate') { Write-Host "     the update said: $(($out | Select-Object -Last 15) -join ' | ')" }
    Check ((Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'v0\.1\.0-beta4') 'the kit is the new version'
    Check ((Read-File "$kit\emulator\lichii.exe") -match 'coop: loaded v0\.1\.0-beta4') 'the emulator is the new one'
    Check ((Read-File "$kit\emulator\user\keys\prod.keys") -eq 'my keys') 'the keys are kept'
    Test-Started $kit 'asked, yes'

    # No key at all (an unattended start): yes.
    $kit = Join-Path $tmp 'kit default'; New-Kit $kit 'v0.1.0-beta3' 'old'
    Start-Play $kit '' $good
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate') 'no answer: updated and the new kit started'

    # Declined: the old kit starts as it is.
    $kit = Join-Path $tmp 'kit no'; New-Kit $kit 'v0.1.0-beta3' 'old'
    Start-Play $kit 'n' $good
    Check (((Get-Played $kit) -join '|') -eq 'old') "declined: only the old play.ps1 ran [$((Get-Played $kit) -join '|')]"
    Check ((Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'v0\.1\.0-beta3') 'declined: the kit is unchanged'
    Test-Started $kit 'declined'

    # auto_update = yes: no question.
    $kit = Join-Path $tmp 'kit auto yes'; New-Kit $kit 'v0.1.0-beta3' 'old'
    $o = "$kit\coop-options.ini"; [System.IO.File]::WriteAllText($o, ([System.IO.File]::ReadAllText($o) -replace 'auto_update = ask', 'auto_update = yes'))
    Start-Play $kit 'n' $good
    Check (($out -join ' ') -notmatch 'Update now\?' -and ($out -join ' ') -match 'auto_update = yes') 'auto_update = yes: no question'
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate') 'auto_update = yes: updated and the new kit started'
    Check ([System.IO.File]::ReadAllText($o) -match '(?m)^auto_update = yes') 'auto_update = yes: the player''s choice is kept'

    # auto_update = no: only says so.
    $kit = Join-Path $tmp 'kit auto no'; New-Kit $kit 'v0.1.0-beta3' 'old'
    $o = "$kit\coop-options.ini"; [System.IO.File]::WriteAllText($o, ([System.IO.File]::ReadAllText($o) -replace 'auto_update = ask', 'auto_update = no'))
    Start-Play $kit 'y' $good
    Check (($out -join ' ') -match 'run Update\.bat' -and ($out -join ' ') -notmatch 'Update now\?') 'auto_update = no: says one is out, no question'
    Check (((Get-Played $kit) -join '|') -eq 'old' -and (Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'beta3') 'auto_update = no: the old kit starts unchanged'

    # Nothing newer.
    $kit = Join-Path $tmp 'kit newest'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Start-Play $kit 'y' $good
    Check (($out -join ' ') -notmatch 'is out' -and ((Get-Played $kit) -join '|') -eq 'old') 'nothing newer: no question, the kit starts'
    Test-Started $kit 'nothing newer'

    # No network (the list cannot be read): starts as now, quickly.
    $kit = Join-Path $tmp 'kit offline'; New-Kit $kit 'v0.1.0-beta3' 'old'
    Start-Play $kit 'y' (Join-Path $tmp 'no-such-list.json')
    Check (($out -join ' ') -notmatch 'is out' -and ((Get-Played $kit) -join '|') -eq 'old') 'no network: no question, the kit starts'
    Check ($seconds -lt 15) ("no network: started in {0:N1} s" -f $seconds)
    Test-Started $kit 'no network'

    # A damaged download: the update fails, nothing changes, the old kit starts.
    $kit = Join-Path $tmp 'kit damaged'; New-Kit $kit 'v0.1.0-beta3' 'old'
    Start-Play $kit 'y' $damaged
    Check (($out -join ' ') -match 'damaged' -and ($out -join ' ') -match 'this version starts as it is') 'a failed update says so'
    Check (((Get-Played $kit) -join '|') -eq 'old' -and (Get-Content "$kit\VERSION.txt" -TotalCount 1) -match 'beta3') 'a failed update: the old kit starts unchanged'
    Test-Started $kit 'failed update'

    # An update that did not finish (update-in-progress.txt): asked again, and done.
    $kit = Join-Path $tmp 'kit unfinished'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Write-File "$kit\update-in-progress.txt" 'Update.bat was updating this kit'
    Start-Play $kit 'y' $good
    Check (($out -join ' ') -match 'did not finish') 'an unfinished update is named'
    Check (((Get-Played $kit) -join '|') -eq 'new NoUpdate' -and -not (Test-Path "$kit\update-in-progress.txt")) 'an unfinished update is done, then the new kit starts'

    # EDEN_NX2_GPL from coop-options.ini reaches the emulator and the AMD rule leaves it.
    $kit = Join-Path $tmp 'kit gpl'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Add-Content -LiteralPath "$kit\coop-options.ini" -Value 'EDEN_NX2_GPL=1'
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Check (($out -join ' ') -match 'Option: EDEN_NX2_GPL=1' -and ($out -join ' ') -notmatch 'AMD GPU found') 'EDEN_NX2_GPL in coop-options.ini is passed and wins over the AMD rule'
    Test-Started $kit 'EDEN_NX2_GPL option'

    # A puppet pack made before sound: one line asks for make-puppet-pack, and the game starts.
    $kit = Join-Path $tmp 'kit pack'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Write-File "$kit\emulator\user\load\0400486024DEC000\coop-puppet\coop_puppet.json" '{"actors": []}'
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Check (($out -join ' ') -match 'Puppet pack: found\. Puppet pack: made without the other players'' sounds\. Run make-puppet-pack\.cmd again') "an old puppet pack: the sound line [$(($out | Select-String 'Puppet pack') -join ' / ')]"
    Test-Started $kit 'old puppet pack'
    # A game mod installed (issue 136): play stops before the server or the game starts, and
    # names the folder; turned off in the game's Add-Ons, the game starts.
    $kit = Join-Path $tmp 'kit mod'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Write-File "$kit\emulator\user\load\0400486024DEC000\coop-puppet\coop_puppet.json" '{"actors": [], "sound": true}'
    Write-File "$kit\emulator\user\load\0400486024DEC000\Hyrule-Cheat-Menu\exefs\subsdk8" 'x'
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Check (($out -join ' ') -match 'Press any key') 'a mod: play stops with an error (play.cmd pauses so the player can read it)'
    Check (($out -join ' ') -match 'Code and cheat mods are not supported in co-op' -and ($out -join ' ') -match 'Hyrule-Cheat-Menu') "a mod: the message names it [$(($out | Select-String 'mods|Hyrule') -join ' / ')]"
    Check (($out -join ' ') -notmatch 'Emulator started') 'a mod: the game does not start'
    Write-File "$kit\emulator\user\config\qt-config.ini" "[DisabledAddOns]`r`nsize=1`r`n1\title_id=288309953924349952`r`n1\disabled\size=1`r`n1\disabled\1\d=Hyrule-Cheat-Menu`r`n"
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Test-Started $kit 'a mod turned off in Add-Ons'
    # A copied kit whose qt-config.ini still names the first kit's folders: the check reads this
    # kit's own (after Repair-KitPaths), not the other's.
    $first = Join-Path $tmp 'kit first'; New-Kit $first 'v0.1.0-beta4' 'old'
    Write-File "$first\emulator\user\load\0400486024DEC000\Hyrule-Cheat-Menu\exefs\subsdk8" 'x'
    $firstUser = "$first\emulator\user".Replace('\', '/')
    $kit = Join-Path $tmp 'kit copy clean'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Write-File "$kit\emulator\user\config\qt-config.ini" "[Data%20Storage]`r`nload_directory=$firstUser/load/`r`nsdmc_directory=$firstUser/sdmc/`r`n"
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Check (($out -join ' ') -notmatch 'Code and cheat mods are not supported') 'a clean copy of a kit with a mod: not stopped by the other kit''s mod'
    Test-Started $kit 'a clean copied kit'
    $kit = Join-Path $tmp 'kit copy mod'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Write-File "$kit\emulator\user\load\0400486024DEC000\Own-Mod\exefs\main.ips" 'PATCH'
    $cleanUser = "$tmp\kit copy clean\emulator\user".Replace('\', '/')
    Write-File "$kit\emulator\user\config\qt-config.ini" "[Data%20Storage]`r`nload_directory=$cleanUser/load/`r`n"
    Start-Play $kit '' (Join-Path $tmp 'no-such-releases.json')
    Check (($out -join ' ') -match 'Own-Mod' -and ($out -join ' ') -notmatch 'Emulator started') 'a copied kit with its own mod: stopped, naming it'
    # Join.bat: what the host pasted is made plain (spaces, :port, a trailing dot), a typo is
    # asked again, and the address is kept for next time.
    $kit = Join-Path $tmp 'kit join'; New-Kit $kit 'v0.1.0-beta4' 'join'
    $env:OOT_COOP_UPDATE_API = (Join-Path $tmp 'no-such-releases.json')
    $in = Join-Path $tmp 'join.txt'
    [System.IO.File]::WriteAllText($in, "26.1.2`r`n  26.1.2.3:27511. `r`nSaria`r`nhunter 2`r`n hunter2 `r`n")
    $out = cmd /c "`"$kit\Join.bat`" < `"$in`"" 2>&1
    $ini = Read-File "$kit\emulator\user\config\coop.ini"
    Check (($out -join ' ') -match "'26\.1\.2' is not an address") 'Join.bat: a typo is asked again'
    Check ($ini -match '(?m)^server = 26\.1\.2\.3\s*$' -and $ini -match '(?m)^port = 27511\s*$') "Join.bat: ' 26.1.2.3:27511. ' is saved as server 26.1.2.3, port 27511"
    Check (($out -join ' ') -match 'Type the join password the host sent you' -and $ini -match '(?m)^password = hunter2\s*$') "Join.bat: a password with a space is asked again, then saved in coop.ini [$(($ini -split "`n") -match 'password' -join '|')]"
    $summary = @($out | Where-Object { "$_" -match '^Co-op: server' }) -join ' '
    Check ($summary -match 'join password set' -and $summary -notmatch 'hunter2') "Join.bat: the summary says 'join password set', not the password [$summary]"
    [System.IO.File]::WriteAllText($in, "`r`n`r`n")
    $out = cmd /c "`"$kit\Join.bat`" < `"$in`"" 2>&1
    Check (($out -join ' ') -match 'server 26\.1\.2\.3, port 27511' -and ($out -join ' ') -match 'Emulator started') 'Join.bat: Enter keeps the last address'
    Check ((Read-File "$kit\emulator\user\config\coop.ini") -match '(?m)^password = hunter2\s*$') 'Join.bat: Enter keeps the saved password'
    Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    # Every session has a join password: '-' (once "none") is asked again.
    [System.IO.File]::WriteAllText($in, "`r`n-`r`nzelda42`r`n")
    $out = cmd /c "`"$kit\Join.bat`" < `"$in`"" 2>&1
    $ini = Read-File "$kit\emulator\user\config\coop.ini"
    Check ($ini -match '(?m)^password = zelda42\s*$' -and ($out -join ' ') -match 'Type the join password' -and ($out -join ' ') -notmatch 'join password none') "Join.bat: '-' is no password any more, asked again [$(($ini -split "`n") -match 'password' -join '|')]"
    # play.ps1 -Hookshot (G5): the same pack step as the emulator: your own tunic in Hookshot's
    # mods\contents pack before the game starts, LICHII_COOP_SELF_TUNIC for the module, the hints.
    $kit = Join-Path $tmp 'kit hookshot'; New-Kit $kit 'v0.1.0-beta4' 'hs'
    $hsGame = Join-Path $kit 'hookshot\game'
    New-Item -ItemType Directory -Force -Path $hsGame | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $hsGame 'longshot.exe'), [System.IO.File]::ReadAllBytes((Join-Path $env:SystemRoot 'System32\rundll32.exe')))
    Write-File (Join-Path $hsGame 'hookshot_coop.dll') 'module'
    $hsPack = Join-Path $hsGame 'mods\contents\0400486024DEC000\coop-puppet'
    New-Item -ItemType Directory -Force -Path (Join-Path $hsPack 'coop_self') | Out-Null
    Write-File (Join-Path $hsPack 'coop_self\red.bntx.zs') 'RED'
    Write-File (Join-Path $hsPack 'coop_puppet.json') '{"sound": true, "tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
    Add-Content -LiteralPath (Join-Path $kit 'coop-options.ini') -Value "`r`ncolor = red"
    # A saved join password (Enter keeps it): the join goes on whether play.ps1 asks for one or insists on one.
    # Its join always asks (every session has a password): Enter, from a file as the Join.bat tests do.
    $enter = Join-Path $tmp 'enter.txt'; [System.IO.File]::WriteAllText($enter, "`r`n")
    Write-File (Join-Path $hsGame 'coop.ini') "server = 127.0.0.1`r`nport = 27510`r`nname = Saria`r`npassword = hunter2`r`n"
    $env:OOT_COOP_UPDATE_API = Join-Path $tmp 'no-such-releases.json'
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" join -Server 127.0.0.1 -Name Saria -Hookshot -NoUpdate < `"$enter`"" 2>&1) -join '|'
    Check ($out -match 'Puppet pack: found\.' -and $out -match 'Your Link: the red tunic' -and $out -match 'Game started') "play.ps1 -Hookshot: the pack found and your red tunic said [$out]"
    Check ([System.IO.File]::ReadAllText((Join-Path $hsPack 'romfs\Tex\Angin_Tile_A_Alb.bntx.zs')) -eq 'RED') 'play.ps1 -Hookshot: the red tile in the Hookshot pack before the game starts'
    Remove-Item -LiteralPath $hsPack -Recurse -Force
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" join -Server 127.0.0.1 -Name Saria -Hookshot -NoUpdate < `"$enter`"" 2>&1) -join '|'
    Check ($out -match 'Puppet pack: not in the co-op game' -and $out -match 'Game started') "play.ps1 -Hookshot: no pack: says so, still starts [$out]"
    # Play-OoT-Coop.bat (play.ps1 choose -Hookshot): no question and no session, so the title screen's Host
    # and Join work (a server in coop.ini makes them answer "Co-op is on already in this game"). After a
    # host or join run its server and password lines go; the name and the last invite code stay.
    $stopHs = { Get-Process hookshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 400 }
    & $stopHs
    Write-File (Join-Path $hsGame 'coop.ini') "server = 127.0.0.1`r`nport = 27510`r`nname = Saria`r`npassword = hunter2`r`nlast_invite = ABCDE-FGHJK`r`n"
    $env:LICHII_COOP_SERVER = '127.0.0.1:27510'
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" choose -Hookshot -NoUpdate < nul" 2>&1) -join '|'
    Remove-Item Env:LICHII_COOP_SERVER -ErrorAction SilentlyContinue
    $idle = Read-File (Join-Path $hsGame 'coop.ini')
    Check ($out -match 'Game started' -and $out -match 'Host or Join in the game''s menu, first tab Multiplayer' -and $out -notmatch 'H or J' -and $out -notmatch 'Invite code') "Play-OoT-Coop: starts the game with no question [$out]"
    Check ($idle -notmatch '(?m)^\s*server\s*=' -and $idle -notmatch '(?m)^\s*password\s*=') "Play-OoT-Coop: no server and no join password left in coop.ini after a host run [$idle]"
    Check ($idle -match '(?m)^name = Saria\r?$' -and $idle -match '(?m)^last_invite = ABCDE-FGHJK\r?$' -and $idle -match '(?m)^port = 27510\r?$') 'Play-OoT-Coop: the name, the port and the last invite code stay'
    Check (-not (Test-Path "$kit\server-args.txt") -and -not (Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) })) 'Play-OoT-Coop: no server is started'
    & $stopHs
    Remove-Item -LiteralPath (Join-Path $hsGame 'coop.ini') -Force
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" choose -Hookshot -NoUpdate < nul" 2>&1) -join '|'
    $idle = Read-File (Join-Path $hsGame 'coop.ini')
    Check ($out -match 'Game started' -and $idle -notmatch '(?m)^\s*server\s*=') "Play-OoT-Coop: a first start (no coop.ini) writes one with no server [$out]"
    # Start Squadala.bat (play.ps1 choose -Hookshot, the release kit's start) reads coop-options.ini too, so
    # your colour's tunic files are put in before the game starts (v0.11.0: this start never read it and put
    # the game's green back: "the puppet pack's tunic files are green, your colour is orange").
    & $stopHs
    New-Item -ItemType Directory -Force -Path (Join-Path $hsPack 'coop_self') | Out-Null
    Write-File (Join-Path $hsPack 'coop_self\red.bntx.zs') 'RED'
    Write-File (Join-Path $hsPack 'coop_puppet.json') '{"sound": true, "tunics": {"red": ["x"], "green": ["y"]}, "self_tiles": {"red": "coop_self/red.bntx.zs"}}'
    Remove-Item Env:LICHII_COOP_COLOR -ErrorAction SilentlyContinue
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" choose -Hookshot -NoUpdate < nul" 2>&1) -join '|'
    $tile = Join-Path $hsPack 'romfs\Tex\Angin_Tile_A_Alb.bntx.zs'
    Check ($out -match 'Option: LICHII_COOP_COLOR=' -and $out -match 'Your Link: the red tunic' -and $out -match 'Game started') "Play-OoT-Coop: coop-options.ini's colour is read before the pack step [$out]"
    Check ((Test-Path -LiteralPath $tile) -and [System.IO.File]::ReadAllText($tile) -eq 'RED') 'Play-OoT-Coop: the red tile in the pack before the game starts'
    Remove-Item -LiteralPath $hsPack -Recurse -Force
    # The autotest's kit mode (tools/autotest/hookshot.py --kit-a): OOT_COOP_TEST_KIT gets the game's process id, and
    # the test server stays in the variables (written to no file: coop.ini still has no server).
    & $stopHs
    $pidFile = Join-Path $tmp 'kit-pid.txt'
    $env:OOT_COOP_TEST_KIT = $pidFile; $env:LICHII_COOP_SERVER = '127.0.0.1:27520'
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" choose -Hookshot -NoUpdate < nul" 2>&1) -join '|'
    Remove-Item Env:OOT_COOP_TEST_KIT, Env:LICHII_COOP_SERVER -ErrorAction SilentlyContinue
    $gamePid = if (Test-Path -LiteralPath $pidFile) { "$(Get-Content -LiteralPath $pidFile -TotalCount 1)".Trim() } else { '' }
    Check ($out -match 'Game started' -and $gamePid -match '^\d+$') "kit autotest: the game's process id in OOT_COOP_TEST_KIT's file [$gamePid] [$out]"
    Check ((Read-File (Join-Path $hsGame 'coop.ini')) -notmatch '(?m)^\s*server\s*=') 'kit autotest: coop.ini still has no server'
    Get-Process hookshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    # A built kit's coop.ini.template and coop-ini.ps1 keep @PORT@ (make_kit.ps1 copies with
    # kit_text.ps1), and play.ps1 writes the port each join needs: 27512 (the join helper) for an
    # invite code, the given port for host:port.
    $kit = Join-Path $tmp 'kit port'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Check ((Read-File "$kit\scripts\coop.ini.template") -match '(?m)^port = @PORT@\r?$') 'kit: coop.ini.template keeps @PORT@'
    Check ((Read-File "$kit\scripts\coop-ini.ps1") -match "'@PORT@'") 'kit: coop-ini.ps1 looks for @PORT@'
    Check ((Read-File "$kit\scripts\play.ps1") -notmatch '@PORT@') 'kit: play.ps1 has the default port filled in'
    # start-join.cmd stub: notes what it was started with (no join helper, no network).
    Write-File "$kit\start-join.cmd" "@echo %*> `"%~dp0join-args.txt`"`r`n@exit /b 0`r`n"
    $env:OOT_COOP_UPDATE_API = Join-Path $tmp 'no-such-releases.json'
    $out = cmd /c "`"$kit\play.cmd`" -Server CHFSA-6JFPP-5AATB-UCVDW-EXFYG-ZH2J3-KSGWK-S2GA -Name Saria < nul" 2>&1
    $ini = Read-File "$kit\emulator\user\config\coop.ini"
    Check ($ini -match '(?m)^server = 127\.0\.0\.1\s*$' -and $ini -match '(?m)^port = 27512\s*$') "invite code: coop.ini is 127.0.0.1 port 27512, the join helper [$(($ini -split "`n") -match '^(server|port)' -join '|')]"
    Check ($ini -match '(?m)^password = ABCDEFGHJKLMNPQRSTUV\s*$') 'invite code: the password from the code is in coop.ini'
    $deadline = (Get-Date).AddSeconds(5)
    while (-not (Test-Path "$kit\join-args.txt") -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    Check ((Read-File "$kit\join-args.txt") -match '203\.0\.113\.5:27510.*-GamePort 27512') "invite code: the join helper is started for the host, game port 27512 [$((Read-File "$kit\join-args.txt").Trim())]"
    Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    $out = cmd /c "`"$kit\play.cmd`" -Server 26.1.2.3:27600 -Name Saria < nul" 2>&1
    $ini = Read-File "$kit\emulator\user\config\coop.ini"
    Check ($ini -match '(?m)^server = 26\.1\.2\.3\s*$' -and $ini -match '(?m)^port = 27600\s*$') "host:port: coop.ini has the given port 27600 [$(($ini -split "`n") -match '^(server|port)' -join '|')]"
    Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    # The host closed the game while the server runs (a player: "the host hookshot batch doesnt work
    # since the server is already open"): Host.bat and Host-Hookshot.bat join the running session
    # again with the password start-server.ps1 saved; no new password, the server is left alone.
    # The fake server: a program that only waits, as this kit's server\coop_server.exe.
    $kit = Join-Path $tmp 'kit rejoin'; New-Kit $kit 'v0.1.0-beta4' 'old'
    Copy-KitFile (Join-Path $kitSrc 'Host-Hookshot.bat') (Join-Path $kit 'Host-Hookshot.bat')
    New-Item -ItemType Directory -Force -Path (Join-Path $kit 'server') | Out-Null
    $fakeServer = Join-Path $kit 'server\coop_server.exe'
    Add-Type -TypeDefinition 'public static class CoopFakeServer { public static void Main() { System.Threading.Thread.Sleep(600000); } }' -OutputAssembly $fakeServer -OutputType ConsoleApplication
    Write-File "$kit\start-server.cmd" "@echo %*> `"%~dp0server-args.txt`"`r`n@exit /b 0`r`n"
    Write-File "$kit\emulator\user\config\coop.ini" "server = 26.1.2.3`r`nport = 27600`r`nname = Saria`r`npassword = afriendspassword`r`n"
    $hsGame = Join-Path $kit 'hookshot\game'
    New-Item -ItemType Directory -Force -Path $hsGame | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $hsGame 'longshot.exe'), [System.IO.File]::ReadAllBytes((Join-Path $env:SystemRoot 'System32\rundll32.exe')))
    Write-File (Join-Path $hsGame 'hookshot_coop.dll') 'module'
    Write-File (Join-Path $hsGame 'coop.ini') "server = 127.0.0.1`r`nport = 27510`r`nname = Saria`r`n"
    $env:OOT_COOP_UPDATE_API = Join-Path $tmp 'no-such-releases.json'
    [void](Write-KitHostSession $kit 27633 'SessionPassword123')
    $server = Start-Process -FilePath $fakeServer -ArgumentList @('--port', '27633', '--password-env', 'OOT_COOP_SERVER_PASSWORD') -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    try {
        foreach ($bat in 'Host.bat', 'Host-Hookshot.bat') {
            $iniFile = if ($bat -eq 'Host.bat') { "$kit\emulator\user\config\coop.ini" } else { Join-Path $hsGame 'coop.ini' }
            $out = (cmd /c "`"$kit\$bat`" -NoUpdate < nul" 2>&1) -join '|'
            $ini = Read-File $iniFile
            Check ($out -match 'joining that session again as the host' -and $out -match '(Emulator|Game) started') "${bat}, the kit's server running: says it joins the running session again, and the game starts [$out]"
            Check ($ini -match '(?m)^password = SessionPassword123\s*$' -and $ini -match '(?m)^server = 127\.0\.0\.1\s*$' -and $ini -match '(?m)^port = 27633\s*$') "${bat}, the kit's server running: coop.ini has the saved password and port [$(($ini -split "`n") -match '^(server|port|password)' -join '|')]"
            Check ($out -notmatch 'new random one' -and $out -notmatch 'Close' -and $out -notmatch 'SessionPassword123') "${bat}, the kit's server running: no new password, no refusal, the password is not printed"
            Start-Sleep -Milliseconds 500
            Check (-not $server.HasExited -and -not (Test-Path "$kit\server-args.txt")) "${bat}, the kit's server running: the server is left alone and not started again"
            Get-Process lichii, hookshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 300
        }
        # The saved session cannot be the running server's (written for another port; no file at
        # all): refused with what to do, coop.ini untouched, nothing started.
        [void](Write-KitHostSession $kit 27999 'OtherPassword456')
        $before = Read-File "$kit\emulator\user\config\coop.ini"
        $out = (cmd /c "`"$kit\Host.bat`" -NoUpdate < nul" 2>&1) -join '|'
        Check ($out -match 'Close that window, then start Host\.bat again' -and $out -notmatch 'Emulator started' -and (Read-File "$kit\emulator\user\config\coop.ini") -eq $before) "Host.bat, a saved session for another port: refused, coop.ini untouched [$out]"
        Remove-Item -LiteralPath (Get-KitHostSessionFile $kit) -Force
        $out = (cmd /c "`"$kit\Host-Hookshot.bat`" -NoUpdate < nul" 2>&1) -join '|'
        Check ($out -match 'Close that window, then start Host-Hookshot\.bat again' -and $out -notmatch 'Game started' -and -not (Test-Path "$kit\server-args.txt")) "Host-Hookshot.bat, no saved session: refused, names Host-Hookshot.bat [$out]"
        # server_password set in coop-options.ini: as before, that password and the running server.
        Add-Content -LiteralPath "$kit\coop-options.ini" -Value "`r`nserver_password = hunter2"
        $out = (cmd /c "`"$kit\Host.bat`" -NoUpdate < nul" 2>&1) -join '|'
        Check ($out -match 'The server from this kit is already running' -and $out -match 'Emulator started' -and (Read-File "$kit\emulator\user\config\coop.ini") -match '(?m)^password = hunter2\s*$') "Host.bat, server_password set and the server running: as before [$out]"
    } finally {
        if (-not $server.HasExited) { $server | Stop-Process -Force -ErrorAction SilentlyContinue }
        Get-Process lichii, hookshot -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 300
    }
    # The saved session (kit-paths.ps1): which running server it can belong to.
    $sk = Join-Path $tmp 'kit session'; New-Item -ItemType Directory -Force -Path $sk | Out-Null
    $now = Get-Date
    Check ($null -eq (Read-KitHostSession $sk @([pscustomobject]@{ StartTime = $now; CommandLine = '' }))) 'host session: no file -> none'
    $text = Write-KitHostSession $sk 27510 'pw1'
    Check ($text -match '(?m)^port = 27510\r?$' -and (Test-Path (Join-Path $sk 'logs\host-session.txt'))) 'host session: written to logs\host-session.txt (not a .log: collect-logs leaves it)'
    $s = Read-KitHostSession $sk @([pscustomobject]@{ StartTime = $now.AddSeconds(3); CommandLine = '"C:\k\server\coop_server.exe" --port 27510 --password-env X' })
    Check ($s -and $s.Port -eq 27510 -and $s.Password -eq 'pw1') 'host session: a server on that port started after the file -> its port and password'
    Check ($null -ne (Read-KitHostSession $sk @([pscustomobject]@{ StartTime = $now.AddSeconds(3); CommandLine = '' }))) 'host session: command line not known -> the start time decides'
    Check ($null -eq (Read-KitHostSession $sk @([pscustomobject]@{ StartTime = $now.AddMinutes(-5); CommandLine = 'coop_server.exe --port 27510' }))) 'host session: a server older than the file -> none'
    Check ($null -eq (Read-KitHostSession $sk @([pscustomobject]@{ StartTime = $now.AddSeconds(3); CommandLine = 'coop_server.exe --port 27511' }))) 'host session: a server on another port -> none'
    Check ($null -eq (Read-KitHostSession $sk @([pscustomobject]@{ StartTime = $now.AddSeconds(3); CommandLine = 'coop_server.exe --join 1.2.3.4:27510 --game-port 27512' }))) 'host session: a join helper -> none'
    Check ($null -eq (Read-KitHostSession $sk @())) 'host session: no server running -> none'
    Write-File (Join-Path $sk 'logs\host-session.txt') "started = x`r`nport = 27510`r`npassword = `r`n"
    Check ($null -eq (Read-KitHostSession $sk @([pscustomobject]@{ StartTime = $now.AddSeconds(3); CommandLine = '' }))) 'host session: a damaged file -> none'
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

# --- Hookshot: what the kit's game needs before it starts, and the port check-hosting checks ---
$hk = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-hs-need-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    Write-File (Join-Path $hk 'hookshot\PIN') "runtime=Longshot`nrepo=https://git.magicbean.dev/MagicBeanDev/Longshot`ncommit=e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8`n"
    Write-File (Join-Path $hk 'hookshot\TOOLS.txt') "release=v3.0.0-rc.2`nlock_sha256=b3`n"
    Write-File (Join-Path $hk 'hookshot\patches\0001-a.patch') "one`n"
    Write-File (Join-Path $hk 'hookshot\hookshot_coop.dll') 'MODULE 1'
    Check ((Get-KitHookshotNeed $hk) -eq 'game') 'hookshot need: no game built yet -> game'
    $g = Join-Path $hk 'hookshot\game'
    Write-File (Join-Path $g 'longshot.exe') 'exe'
    Write-File (Join-Path $g 'coop-built-from.txt') (Get-KitHookshotStamp $hk)
    Write-File (Join-Path $g 'hookshot_coop.dll') 'MODULE 1'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: no puppet pack -> module (the pack is made)'
    $pack = Join-Path $g 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json'
    Write-File $pack '{"sound": true, "tunics": {"red": ["x"]}, "look_files": {"gear": "coop_look/gear.bin"}}'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: an incomplete puppet pack (gear.bin gone) -> module (the pack is made again; pack-retry)'
    Write-File (Join-Path (Split-Path -Parent $pack) 'coop_look\gear.bin') 'gear'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: a tunic file gone -> module'
    Write-File (Join-Path (Split-Path -Parent $pack) 'romfs\x') 'tunic'
    Check ((Get-KitHookshotNeed $hk) -eq '') 'hookshot need: the game as the kit wants it -> nothing'
    Write-File (Join-Path $hk 'hookshot\hookshot_coop.dll') 'MODULE 2'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: a new module -> module (seconds)'
    Write-File (Join-Path $g 'hookshot_coop.dll') 'MODULE 2'
    Write-File (Join-Path $hk 'hookshot\patches\0002-b.patch') "two`n"
    Check ((Get-KitHookshotNeed $hk) -eq 'patches') 'hookshot need: a patch changed, the same pin -> patches'
    Write-File (Join-Path $g 'coop-built-from.txt') (Get-KitHookshotStamp $hk)
    Write-File (Join-Path $hk 'hookshot\PIN') "repo=x`ncommit=0000000000000000000000000000000000000000`n"
    Check ((Get-KitHookshotNeed $hk) -eq 'pin') 'hookshot need: another pin -> pin (a full build)'
    Write-File (Join-Path $hk 'hookshot\PIN') "repo=x`ncommit=e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8`n"
    Write-File $pack '{"sound": false}'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: a pack the kit would make again -> module'
    Write-File $pack '{"sound": true, "tunics": {"red": ["x"]}}'
    Check ((Get-KitHookshotNeed $hk) -eq '') 'hookshot need: up to date again'
    # the randomizer the kit brings (hookshot\OOTRR.txt): fetched into the game's tools\ootrr by the module step
    $osha = 'a' * 64
    $offlineWas = $env:OOT_COOP_OFFLINE
    $env:OOT_COOP_OFFLINE = '0'   # (online, with no probe)
    Write-File (Join-Path $hk 'hookshot\OOTRR.txt') "# pin`nrelease=v0.5.0`nurl=https://x`nwindows=OOTRR-0.5.0-windows-x64.zip`nwindows_sha256=$osha`n"
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: the kit''s OOTRR not in the game yet -> module'
    # no internet (kit-offline): only the OOTRR download is left -> 'offline' (play.ps1 says one line, no module step on
    # every start); a zip already downloaded with the pinned SHA-256 still goes in; other stale work is still done
    $env:OOT_COOP_OFFLINE = '1'
    Check ((Test-KitOffline) -and -not (Test-KitOotrrZipReady $hk)) 'offline: OOT_COOP_OFFLINE=1 says offline; no OOTRR zip here'
    Check ((Get-KitHookshotNeed $hk) -eq 'offline') 'hookshot need offline: only the OOTRR download left -> offline (no module step)'
    $ozipBytes = [System.Text.Encoding]::ASCII.GetBytes('zip')
    $ozip = Join-Path $hk 'hookshot\downloads\OOTRR-0.5.0-windows-x64.zip'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ozip) | Out-Null
    [System.IO.File]::WriteAllBytes($ozip, $ozipBytes)
    Check ((Get-KitHookshotNeed $hk) -eq 'offline') 'hookshot need offline: a zip whose SHA-256 differs is not used -> offline'
    $zsha = (Get-FileHash -LiteralPath $ozip -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-File (Join-Path $hk 'hookshot\OOTRR.txt') "# pin`nrelease=v0.5.0`nurl=https://x`nwindows=OOTRR-0.5.0-windows-x64.zip`nwindows_sha256=$zsha`n"
    Check ((Test-KitOotrrZipReady $hk) -and (Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need offline: the zip here matches its SHA-256 -> module (no network needed)'
    Remove-Item -LiteralPath $ozip -Force
    Write-File (Join-Path $hk 'hookshot\OOTRR.txt') "# pin`nrelease=v0.5.0`nurl=https://x`nwindows=OOTRR-0.5.0-windows-x64.zip`nwindows_sha256=$osha`n"
    $gmod = Join-Path $g 'hookshot_coop.dll'
    $gmodWas = [System.IO.File]::ReadAllBytes($gmod)
    [System.IO.File]::WriteAllBytes($gmod, [byte[]](1, 2, 3))
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need offline: a new module is still put in -> module'
    [System.IO.File]::WriteAllBytes($gmod, $gmodWas)
    # behind a proxy (review-kit-offline): never offline, no probe, so the download is tried as before
    $viaProxy = New-Object System.Net.WebProxy('http://proxy.example:8080')
    $direct = New-Object System.Net.WebProxy
    $bypassed = New-Object System.Net.WebProxy('http://proxy.example:8080', $false, [string[]]@('github\.com'))
    Check ((Test-KitProxied $viaProxy) -and -not (Test-KitProxied $direct) -and -not (Test-KitProxied $bypassed)) 'proxy: github.com through a proxy yes; no proxy or github.com bypassed no'
    $env:OOT_COOP_OFFLINE = ''
    Check (-not (& { function Test-KitProxied { $true }; Test-KitOffline })) 'offline: behind a proxy -> online, no probe'
    $env:OOT_COOP_OFFLINE = '0'
    Write-File (Join-Path $g 'tools\ootrr\ootrr.exe') 'MZ'
    Write-File (Join-Path $g 'tools\ootrr\coop-ootrr.txt') "v0.4.2 $osha`n"
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: another OOTRR release in the game -> module'
    Write-File (Join-Path $g 'tools\ootrr\coop-ootrr.txt') "v0.5.0 $osha`n"
    Check ((Get-KitHookshotNeed $hk) -eq '') 'hookshot need: the kit''s OOTRR in the game -> nothing'
    Check (-not (Test-KitOotrrStale (Join-Path $hk 'no-kit') $g)) 'OOTRR stale: a kit without OOTRR.txt brings none'
    Check ((Test-KitOotrrStale $hk (Join-Path $hk 'no-game'))) 'OOTRR stale: a game without tools\ootrr -> yes'
    # Another OOTRR release (0.5.0 -> 0.6.0): its seeds are other worlds and the old seed files do not build with it,
    # so the built seeds go (the game makes them again); another mod, a seed folder not built (no coop-seed.txt) and
    # the control folder stay.
    $ng = Join-Path $hk 'new-ootrr-game'
    Write-File (Join-Path $ng 'mods\randomizer-AB\coop-seed.txt') "AB`n"
    Write-File (Join-Path $ng 'mods\randomizer-AB-keysanity\coop-seed.txt') "AB`n"
    Write-File (Join-Path $ng 'mods\randomizer-half\romfs\x.bin') 'x'
    Write-File (Join-Path $ng 'mods\oot-remake-randomizer\randomizer-AB.ootrr') '{}'
    Write-File (Join-Path $ng 'mods\oot-remake-randomizer\randomizer-AB.spoiler.txt') 'chests'
    Write-File (Join-Path $ng 'mods\oot-remake-randomizer\settings.txt') 'on=1'
    Write-File (Join-Path $ng 'mods\hd-textures\romfs\y.bin') 'y'
    $gone = @(Remove-KitRandoSeeds $ng 'v0.6.0')
    Check ($gone.Count -eq 2 -and ($gone -join ' ') -match 'randomizer-AB removed for OOTRR v0\.6\.0' -and
           -not (Test-Path -LiteralPath (Join-Path $ng 'mods\randomizer-AB')) -and
           -not (Test-Path -LiteralPath (Join-Path $ng 'mods\randomizer-AB-keysanity')) -and
           -not (Test-Path -LiteralPath (Join-Path $ng 'mods\oot-remake-randomizer\randomizer-AB.ootrr')) -and
           -not (Test-Path -LiteralPath (Join-Path $ng 'mods\oot-remake-randomizer\randomizer-AB.spoiler.txt'))) 'new OOTRR: built seeds, their seed files and kept spoiler logs removed'
    Check ((Test-Path -LiteralPath (Join-Path $ng 'mods\randomizer-half\romfs\x.bin')) -and
           (Test-Path -LiteralPath (Join-Path $ng 'mods\oot-remake-randomizer\settings.txt')) -and
           (Test-Path -LiteralPath (Join-Path $ng 'mods\hd-textures\romfs\y.bin'))) 'new OOTRR: other mods, the control folder and an unbuilt folder stay'
    Check (@(Remove-KitRandoSeeds (Join-Path $hk 'no-such-game') 'v0.6.0').Count -eq 0) 'new OOTRR: no mods folder -> nothing'
    # Install-KitOotrr (player report 2026-11-19, Velvet: no tools\ootrr\ootrr.exe on every start after Update.bat, the
    # randomizer broken; the old step deleted tools\ootrr before unpacking, so any failure after that left none).
    # All or nothing: unpacked into tools\ootrr.new, tried (Test-KitOotrrRuns), then swapped; a failure keeps the OOTRR
    # that was there and writes why into tools\ootrr-setup.txt, which the game's chat shows (coop_rando OotrrMissingText).
    $ik = Join-Path $hk 'ootrr-install'
    $ig = Join-Path $ik 'game'
    $iDl = Join-Path $ik 'hookshot\downloads'
    $New_OotrrZip = {
        param([string]$Name, [string]$Release, [string]$ExeFrom, [string]$ExeText)
        $src = Join-Path $ik "src-$Release"
        $top = Join-Path $src "OOTRR-$Release-windows-x64"
        New-Item -ItemType Directory -Force -Path $top, $iDl | Out-Null
        if ($ExeFrom) { Copy-Item -LiteralPath $ExeFrom -Destination (Join-Path $top 'ootrr.exe') } else { Write-File (Join-Path $top 'ootrr.exe') $ExeText }
        Write-File (Join-Path $top 'LICENSE') 'GPL'
        $zip = Join-Path $iDl $Name
        & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $src "OOTRR-$Release-windows-x64"
        $sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-File (Join-Path $ik 'hookshot\OOTRR.txt') "# pin`nrelease=$Release`nurl=https://example.invalid/ootrr`nwindows=$Name`nwindows_sha256=$sha`n"
        return $sha
    }
    # (stand-ins for ootrr.exe, compiled here: 'shuffle 1 1' ends with 0 as OOTRR's does; the bad one starts and fails)
    $fakeGood = Join-Path $ik 'fake-good.exe'; $fakeBad = Join-Path $ik 'fake-bad.exe'
    New-Item -ItemType Directory -Force -Path $ik | Out-Null
    Add-Type -OutputType ConsoleApplication -OutputAssembly $fakeGood -TypeDefinition 'public static class P { public static int Main(string[] a) { return (a.Length == 3 && a[0] == "shuffle") ? 0 : 2; } }'
    Add-Type -OutputType ConsoleApplication -OutputAssembly $fakeBad -TypeDefinition 'public static class Q { public static int Main(string[] a) { return 3; } }'
    $goodSha = & $New_OotrrZip 'OOTRR-good.zip' 'v0.5.0' $fakeGood ''
    $r = Install-KitOotrr $ik $ig
    Check ($r.Ok -and -not $r.Why -and (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr\ootrr.exe')) -and
           (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr\coop-ootrr.txt') -TotalCount 1).Trim() -eq "v0.5.0 $goodSha" -and
           (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr-setup.txt') -TotalCount 1).Trim() -eq 'ok v0.5.0' -and
           -not (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr.new')) -and -not (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr.old'))) "OOTRR install: unpacked, tried, in place, stamped, ok in ootrr-setup.txt [$($r.Lines -join ' | ')]"
    $r = Install-KitOotrr $ik $ig
    Check ($r.Ok -and ($r.Lines -join ' ') -match 'there already') 'OOTRR install: the pinned one there already -> nothing done'
    # A release whose ootrr.exe Windows will not start (an antivirus's block looks the same): the old one stays.
    [void](& $New_OotrrZip 'OOTRR-bad.zip' 'v0.6.0' '' 'not a program')
    $r = Install-KitOotrr $ik $ig
    Check (-not $r.Ok -and $r.Why -match 'did not let ootrr.exe run' -and $r.Had -eq 'v0.5.0' -and
           (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr\coop-ootrr.txt') -TotalCount 1).Trim() -eq "v0.5.0 $goodSha" -and
           (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr-setup.txt') -TotalCount 1) -match '^failed v0\.6\.0: Windows did not let ootrr\.exe run' -and
           ($r.Lines -join ' ') -match 'is kept' -and ($r.Lines -join ' ') -match 'Protection history' -and
           -not (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr.new'))) "OOTRR install: a program that does not run keeps the old OOTRR and says why [$($r.Lines -join ' | ')]"
    Check (Test-KitOotrrStale $ik $ig) 'OOTRR install: after a failure the kit still counts it stale (the next start and Update.bat try again)'
    Check ((Test-KitOotrrRuns (Join-Path $ik 'nothing.exe')) -match 'not there') 'OOTRR runs: a missing program says so'
    # The game (or its ootrr.exe) holds the folder: the swap fails and the old folder is put back.
    # review-0116-j N1: a program that starts but fails its test (exit 3) is not swapped in either.
    [void](& $New_OotrrZip 'OOTRR-fails.zip' 'v0.6.5' $fakeBad '')
    $r = Install-KitOotrr $ik $ig
    Check (-not $r.Ok -and $r.Why -match 'failed a 1-second test \(exit 3\)' -and
           (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr\coop-ootrr.txt') -TotalCount 1).Trim() -eq "v0.5.0 $goodSha" -and
           -not (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr.new'))) "OOTRR install: a program that exits with an error keeps the old OOTRR [$($r.Lines -join ' | ')]"
    $goodSha = & $New_OotrrZip 'OOTRR-good7.zip' 'v0.7.0' $fakeGood ''
    $hold = [System.IO.File]::Open((Join-Path $ig 'tools\ootrr\LICENSE'), 'Open', 'Read', 'None')
    try { $r = Install-KitOotrr $ik $ig } finally { $hold.Close() }
    Check (-not $r.Ok -and $r.Why -match 'could not be put in place' -and (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr\ootrr.exe')) -and
           (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr\coop-ootrr.txt') -TotalCount 1) -match '^v0\.5\.0 ') "OOTRR install: a held folder keeps the old OOTRR in place [$($r.Lines -join ' | ')]"
    # A swap stopped between its two moves (power cut): tools\ootrr.old and no tools\ootrr -> put back, then updated.
    Move-Item -LiteralPath (Join-Path $ig 'tools\ootrr') -Destination (Join-Path $ig 'tools\ootrr.old')
    $r = Install-KitOotrr $ik $ig
    Check ($r.Ok -and $r.Had -eq 'v0.5.0' -and (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr\coop-ootrr.txt') -TotalCount 1).Trim() -eq "v0.7.0 $goodSha" -and
           -not (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr.old'))) "OOTRR install: a stopped swap is put back first, then the new release goes in [$($r.Lines -join ' | ')]"
    # Offline with no zip here: no try, the old one kept, said.
    $offWas = $env:OOT_COOP_OFFLINE; $env:OOT_COOP_OFFLINE = '1'
    Write-File (Join-Path $ik 'hookshot\OOTRR.txt') "# pin`nrelease=v0.8.0`nurl=https://example.invalid/ootrr`nwindows=OOTRR-none.zip`nwindows_sha256=$('0' * 64)`n"
    $r = Install-KitOotrr $ik $ig
    $env:OOT_COOP_OFFLINE = $offWas
    Check (-not $r.Ok -and $r.Why -eq 'offline' -and (Test-Path -LiteralPath (Join-Path $ig 'tools\ootrr\ootrr.exe')) -and
           (Get-Content -LiteralPath (Join-Path $ig 'tools\ootrr-setup.txt') -TotalCount 1) -match '^failed v0\.8\.0: this PC had no internet') 'OOTRR install: offline -> no try, the old one kept, said'
    Check ((Install-KitOotrr (Join-Path $ik 'no-kit') $ig).Ok) 'OOTRR install: a kit without OOTRR.txt brings none (fine)'
    Remove-Item -LiteralPath $ik -Recurse -Force -ErrorAction SilentlyContinue
    # A randomizer seed built before the puppet pack was made again: its size table lacks the pack's entries and the
    # game aborts while it loads (lane1, 2026-11-14), so it is built again, or removed when it cannot be.
    $rg = Join-Path $hk 'rando-game'
    $rst = 'romfs\System\Resource\T.rsizetable.zs'
    Write-File (Join-Path $rg "mods\randomizer-AB\$rst") 'seed'
    Write-File (Join-Path $rg 'mods\randomizer-AB\coop-seed.txt') "AB`n"
    Write-File (Join-Path $rg "mods\_off\$rst") 'off'
    Write-File (Join-Path $rg "mods\contents\0400486024DEC000\coop-puppet\$rst") 'pack'
    (Get-Item (Join-Path $rg "mods\randomizer-AB\$rst")).LastWriteTimeUtc = [datetime]::UtcNow.AddHours(-1)
    Check ((@(Get-KitOtherSizeTables (Join-Path $rg 'mods')) | ForEach-Object { $_.Directory.Parent.Parent.Parent.Name }) -join ',' -eq 'coop-puppet') 'rando seeds: the other mods'' size tables are the pack''s (deep), not a seed''s or an off folder''s'
    Check ((@(Get-KitStaleRandoSeeds $rg) -join ',') -eq 'randomizer-AB') 'rando seeds: a seed older than the puppet pack is stale'
    $lines = @(Update-KitRandoSeeds $rg)
    Check (-not (Test-Path -LiteralPath (Join-Path $rg 'mods\randomizer-AB')) -and $lines.Count -eq 1 -and $lines[0] -match 'removed') 'rando seeds: no OOTRR to build it again -> the seed is removed, said once'
    Check (@(Get-KitStaleRandoSeeds $rg).Count -eq 0) 'rando seeds: nothing stale after'
    # Built again with OOTRR (a fake: writes the seed as `ootrr build` does): the game's own files OOTRR does not
    # write stay in the seed, so the players still find spoiler.txt and settings.txt (player report 0.11.3).
    Write-File (Join-Path $rg "mods\randomizer-AB\$rst") 'seed'
    Write-File (Join-Path $rg 'mods\randomizer-AB\coop-seed.txt') "AB`n"
    Write-File (Join-Path $rg 'mods\randomizer-AB\coop-rando.txt') "default`n"
    Write-File (Join-Path $rg 'mods\randomizer-AB\spoiler.txt') 'chests'
    Write-File (Join-Path $rg 'mods\randomizer-AB\settings.txt') "OOTRR1-XZLA`n"
    Write-File (Join-Path $rg 'mods\oot-remake-randomizer\randomizer-AB.ootrr') '{}'
    Write-File (Join-Path $rg 'romfs\System\x.bin') 'x'
    (Get-Item (Join-Path $rg "mods\randomizer-AB\$rst")).LastWriteTimeUtc = [datetime]::UtcNow.AddHours(-1)
    $fake = Join-Path $hk 'fake-ootrr.ps1'
    Write-File $fake @'
# build <seed file> <romfs> <load dir> <name> --host hookshot
$out = Join-Path $args[3] $args[4]
New-Item -ItemType Directory -Force -Path (Join-Path $out 'romfs\System\Resource') | Out-Null
Set-Content -LiteralPath (Join-Path $out 'romfs\System\Resource\T.rsizetable.zs') -Value 'new'
Set-Content -LiteralPath (Join-Path $out 'coop-seed.txt') -Value 'AB'
exit 0
'@
    $lines = @(Update-KitRandoSeeds $rg $fake)
    $ab = Join-Path $rg 'mods\randomizer-AB'
    Check ($lines.Count -eq 1 -and $lines[0] -match 'built again' -and
           (Get-Content -Raw -LiteralPath (Join-Path $ab $rst)).Trim() -eq 'new') "rando seeds: built again with OOTRR [$($lines -join ' | ')]"
    Check ((Get-Content -Raw -LiteralPath (Join-Path $ab 'spoiler.txt')) -eq 'chests' -and
           (Get-Content -Raw -LiteralPath (Join-Path $ab 'settings.txt')).Trim() -eq 'OOTRR1-XZLA' -and
           (Get-Content -Raw -LiteralPath (Join-Path $ab 'coop-rando.txt')).Trim() -eq 'default') 'rando seeds: built again, spoiler.txt, settings.txt and the preset kept'
    $env:OOT_COOP_OFFLINE = $offlineWas
    Check ((Test-KitPuppetPackStale (Join-Path $hk 'none\coop_puppet.json') (Join-Path $hk 'no-tool')) -and
        -not (Test-KitPuppetPackStale $pack (Join-Path $hk 'no-tool'))) 'pack stale: a missing pack yes, a current one no'
    # the game somewhere else (hs.py, -Out): game-folder.txt, and a builder's bin\ layout
    $other = Join-Path $hk 'elsewhere'
    Write-File (Join-Path $hk 'hookshot\game-folder.txt') "$other`n"
    Check ((Get-KitHookshotGame $hk) -eq $other -and (Get-KitHookshotNeed $hk) -eq 'moved') 'hookshot need: game-folder.txt is followed; its game gone -> moved'
    Write-File (Join-Path $other 'bin\longshot.exe') 'exe'
    Check ((Get-KitHookshotExe $other) -eq (Join-Path $other 'bin\longshot.exe')) 'hookshot exe: bin\ first'
    # kit-copy (a 0.10.1 player: a Windows copy of a built kit said "the co-op game of another kit is running"):
    # a copied or moved kit uses its OWN hookshot\game, never the other kit's, whatever game-folder.txt says
    $cp = "$hk - Copy"
    try {
        Copy-Item -LiteralPath $hk -Destination $cp -Recurse
        Write-File (Join-Path $hk 'hookshot\game-folder.txt') "$g`n"            # 0.10.1: the full path to its own game
        Write-File (Join-Path $cp 'hookshot\game-folder.txt') "$g`n"            # ... and the copy keeps that path
        $cg = Join-Path $cp 'hookshot\game'
        Check ((Get-KitHookshotGame $hk) -eq $g) 'kit-copy: a 0.10.1 kit naming its own game by full path keeps it'
        Check ((Get-KitHookshotGame $cp) -eq $cg -and (Get-KitHookshotNeed $cp) -eq '') "kit-copy: copied built kit -> its own game, ready [$(Get-KitHookshotGame $cp)]"
        Write-File (Join-Path $cp 'hookshot\game-folder.txt') "hookshot\game`n"
        Check ((Get-KitHookshotGame $cp) -eq $cg) 'kit-copy: a relative game-folder.txt is the kit''s own folder'
        Remove-Item -LiteralPath $cg -Recurse -Force
        Write-File (Join-Path $cp 'hookshot\game-folder.txt') "$g`n"
        Check ((Get-KitHookshotGame $cp) -eq $cg -and (Get-KitHookshotNeed $cp) -eq 'game') "kit-copy: copied unbuilt kit -> its own (missing) game: built, not shared [$(Get-KitHookshotNeed $cp)]"
        Remove-Item -LiteralPath (Join-Path $cp 'hookshot\game-folder.txt')
        Check ((Get-KitHookshotNeed $cp) -eq 'game') 'kit-copy: copied before the first build (no game-folder.txt) -> game'
        Write-File (Join-Path $cp 'hookshot\game-folder.txt') "$(Join-Path $hk 'gone\hookshot\game')`n"
        Check ((Get-KitHookshotNeed $cp) -eq 'game') 'kit-copy: a moved kit whose old folder is gone -> its own game, not moved'
        Write-File (Join-Path $cp 'hookshot\game-folder.txt') "$other`n"
        Check ((Get-KitHookshotGame $cp) -eq $other) 'kit-copy: -Out (a game elsewhere, not a kit''s hookshot\game) is still followed'
        # the copy of a built game: same stamp -> found, another stamp or no game -> not, never itself
        $want = Get-KitHookshotStamp $cp
        Check ((Find-KitHookshotGameToCopy $cp $want @($hk, $cp)) -eq (Join-Path ([System.IO.Path]::GetFullPath($hk)) 'hookshot\game')) 'kit-copy: a built game of another kit, same stamp -> copied, not built'
        Check (-not (Find-KitHookshotGameToCopy $cp "$want-x" @($hk))) 'kit-copy: another kit''s game of another stamp -> not copied'
        Check (-not (Find-KitHookshotGameToCopy $hk (Get-KitHookshotStamp $hk) @($hk))) 'kit-copy: a kit never copies from itself'
        Check (-not (Find-KitHookshotGameToCopy $cp $want @($cp, (Join-Path $hk 'nothing')))) 'kit-copy: no other built kit -> not copied'
        # NVIDIA DLSS (#234): another kit's game is copied only with DLSS as this kit chose (NVIDIA's files only after its own yes)
        Check (-not (Find-KitHookshotGameToCopy $cp $want @($hk) -Dlss $true)) 'dlss: a kit with DLSS on does not copy a game without it'
        $hkNgx = Join-Path $hk 'hookshot\game\ngx'
        Write-File (Join-Path $hkNgx 'nvngx_dlss.dll') 'NVIDIA'
        try {
            Check ((Test-KitHookshotDlss (Join-Path $hk 'hookshot\game')) -and -not (Test-KitHookshotDlss $cg)) 'dlss: a game with ngx\nvngx_dlss.dll has DLSS, one without has not'
            Check (-not (Find-KitHookshotGameToCopy $cp $want @($hk))) 'dlss: a kit without DLSS never copies a game with NVIDIA''s files'
            Check ((Find-KitHookshotGameToCopy $cp $want @($hk) -Dlss $true) -eq (Join-Path ([System.IO.Path]::GetFullPath($hk)) 'hookshot\game')) 'dlss: a kit with DLSS on copies a game with it'
        } finally { Remove-Item -LiteralPath $hkNgx -Recurse -Force -ErrorAction SilentlyContinue }
        # the license link follows the pinned tools' scripts\ngx_sdk.py; without them, NVIDIA's repository
        $ngxTools = Join-Path $cp 'ngx-tools'
        Write-File (Join-Path $ngxTools 'scripts\ngx_sdk.py') "COMMIT = `"$('ab' * 20)`"   # (v1)`n"
        Check ((Get-KitNgxLicenseUrl $ngxTools) -eq "https://github.com/NVIDIA/DLSS/blob/$('ab' * 20)/LICENSE.txt") 'dlss: the license link is the pinned ngx_sdk.py''s commit'
        Check ((Get-KitNgxLicenseUrl (Join-Path $cp 'no-tools')) -eq 'https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt') 'dlss: no tools -> the license in NVIDIA''s repository'
    } finally {
        Remove-Item -LiteralPath $cp -Recurse -Force -ErrorAction SilentlyContinue
        Write-File (Join-Path $hk 'hookshot\game-folder.txt') "$other`n"
    }
    # collect-logs: a kit-built game (bin\longshot.exe beside image.bin) writes its log in the folder above bin\
    Check (@(Get-KitHookshotLogDirs (Join-Path $other 'bin\longshot.exe')).Count -eq 1) 'hookshot log dirs: bin\ alone without image.bin beside it'
    Write-File (Join-Path $other 'image.bin') 'img'
    $dirs = @(Get-KitHookshotLogDirs (Join-Path $other 'bin\longshot.exe'))
    Check ($dirs.Count -eq 2 -and $dirs[0] -eq $other -and $dirs[1] -eq (Join-Path $other 'bin')) 'hookshot log dirs: the game folder first, then bin\'
    Remove-Item -LiteralPath (Join-Path $other 'image.bin')
    Check (@(Get-KitHookshotLogDirs '').Count -eq 0) 'hookshot log dirs: no exe, none'
    # Longshot rc.4 writes its log into logs\ of the game folder, rc.3 (0.11.1's games) beside the exe: both found
    $lf = Join-Path ([System.IO.Path]::GetTempPath()) "coop-logfiles-$PID"
    try {
        New-Item -ItemType Directory -Force (Join-Path $lf 'logs') | Out-Null
        Check (@(Get-KitHookshotLogFiles $lf).Count -eq 0) 'hookshot log files: none yet'
        Write-File (Join-Path $lf 'logs\longshot_log.txt') 'rc4'
        $got = @(Get-KitHookshotLogFiles $lf)
        Check ($got.Count -eq 1 -and $got[0].Sub -eq 'logs' -and $got[0].File.Name -eq 'longshot_log.txt') 'hookshot log files: rc.4 logs\longshot_log.txt'
        Write-File (Join-Path $lf 'longshot_log.txt') 'rc3'
        Write-File (Join-Path $lf 'hookshot_log.txt') 'old'
        Write-File (Join-Path $lf 'logs\other.txt') 'x'
        $got = @(Get-KitHookshotLogFiles $lf)
        Check ($got.Count -eq 3 -and @($got | Where-Object { $_.Sub -eq '' }).Count -eq 2) 'hookshot log files: rc.3 beside the exe too, other files left out'
        Check (@(Get-KitHookshotLogFiles '').Count -eq 0) 'hookshot log files: no folder, none'
    } finally {
        Remove-Item -LiteralPath $lf -Recurse -Force -ErrorAction SilentlyContinue
    }
    # collect-logs finds the game more ways than the start (player report: no longshot_log.txt in the zip)
    $lk = Join-Path ([System.IO.Path]::GetTempPath()) "coop-logexes-$PID"
    try {
        $otherGame = Join-Path $lk 'other-kit\hookshot\game'
        $runGame = Join-Path $lk 'elsewhere'
        Write-File (Join-Path $lk 'kit\hookshot\game\longshot.exe') 'exe'
        Write-File (Join-Path $otherGame 'longshot.exe') 'exe'
        Write-File (Join-Path $runGame 'longshot.exe') 'exe'
        Write-File (Join-Path $lk 'kit\hookshot\game-folder.txt') "$otherGame`n"
        $kitExe = Join-Path $lk 'kit\hookshot\game\longshot.exe'
        $e = @(Get-KitLogGameExes (Join-Path $lk 'kit') @((Join-Path $runGame 'longshot.exe'), $kitExe, (Join-Path $lk 'gone\longshot.exe')))
        Check ($e.Count -eq 3 -and $e[0] -eq $kitExe -and $e[1] -eq (Join-Path $otherGame 'longshot.exe') -and $e[2] -eq (Join-Path $runGame 'longshot.exe')) "log game exes: the kit's own game, then the one game-folder.txt names, then a running one; each once, none missing [$($e -join ', ')]"
        Remove-Item -LiteralPath (Join-Path $lk 'kit\hookshot\game-folder.txt')
        Check (@(Get-KitLogGameExes (Join-Path $lk 'kit') @()).Count -eq 1) 'log game exes: no game-folder.txt, nothing running -> the kit''s own game'
    } finally { Remove-Item -LiteralPath $lk -Recurse -Force -ErrorAction SilentlyContinue }
    # the puppet pack's game file gone (player report: names but no Links, the only hint in longshot_log.txt)
    $pt = (Get-KitPackSourceText 'D:\Games\oot.nup') -join "`n"
    Check ($pt -match 'D:\\Games\\oot\.nup' -and $pt -match 'game-files\.txt' -and $pt -match 'names only' -and $pt -match 'Every start tries again') "pack source text: names the missing file, game-files.txt and what happens [$pt]"
    $pt = (Get-KitPackSourceText '') -join "`n"
    Check ($pt -match 'game-files\.txt is missing' -and $pt -match '\.nup') "pack source text: no game-files.txt said plainly [$pt]"
    # pack-retry: the .nup moved (Downloads, Desktop, beside the kit): the start finds it before it asks.
    $nf = Join-Path $hk 'nup-find'
    $isGame = { param($p) [System.IO.File]::ReadAllText($p) -eq 'GAME' }
    Write-File (Join-Path $nf 'Downloads\sub\OoT.nup') 'GAME'
    Write-File (Join-Path $nf 'Downloads\manual.nup') 'NOT'
    Write-File (Join-Path $nf 'Desktop\deep\er\far\Other.nup') 'GAME'
    $places = @((Join-Path $nf 'gone'), (Join-Path $nf 'Downloads'), (Join-Path $nf 'Desktop'))
    Check ((Find-KitGameNup (Join-Path $nf 'gone\OoT.nup') $hk $isGame $places) -eq (Join-Path $nf 'Downloads\sub\OoT.nup')) 'nup find: moved into a Downloads subfolder: found; not the game file and too deep left out'
    Write-File (Join-Path $nf 'Desktop\Renamed.nup') 'GAME'
    Check ((Find-KitGameNup (Join-Path $nf 'gone\OoT.nup') $hk $isGame $places) -eq (Join-Path $nf 'Downloads\sub\OoT.nup')) 'nup find: two game files: the one with the remembered name'
    Check ((Find-KitGameNup '' $hk $isGame $places) -eq '') 'nup find: two game files and no name to go by: none (asked)'
    Remove-Item -LiteralPath (Join-Path $nf 'Downloads\sub\OoT.nup')
    Check ((Find-KitGameNup '' $hk $isGame $places) -eq (Join-Path $nf 'Desktop\Renamed.nup')) 'nup find: one game file, renamed: found'
    Check ((Find-KitGameNup '' $hk { param($p) throw 'tool error' } $places) -eq '') 'nup find: the check throws: none, no throw'
    Check ((Find-KitGameNup '' $hk $isGame @((Join-Path $nf 'nothing'))) -eq '') 'nup find: nothing anywhere: none'
    # The drag prompt is asked once, not at every start (review): a new game file or a new pack tool asks again.
    $askTool = Join-Path $nf 'tool'
    Write-File (Join-Path $askTool 'build.py') 'v1'
    $note = Join-Path $nf 'pack-asked.txt'
    $key = Get-KitPackAskKey 'D:\gone\OoT.nup' $askTool
    Check (-not (Test-KitPackAsked $note $key)) 'pack ask: never asked: asks'
    Write-File $note "$key`n"
    Check (Test-KitPackAsked $note (Get-KitPackAskKey 'D:\gone\OoT.nup' $askTool)) 'pack ask: asked about this file and tool: not again'
    Check (-not (Test-KitPackAsked $note (Get-KitPackAskKey 'E:\other.nup' $askTool))) 'pack ask: another game file named: asks again'
    Check (-not (Test-KitPackAsked $note (Get-KitPackAskKey '' $askTool))) 'pack ask: no game file named any more: asks again'
    Write-File (Join-Path $askTool 'build.py') 'v2'
    Check (-not (Test-KitPackAsked $note (Get-KitPackAskKey 'D:\gone\OoT.nup' $askTool))) 'pack ask: a new pack tool (an update): asks again'
    $hb = [System.IO.File]::ReadAllText((Join-Path $kitSrc 'scripts\hookshot-build.ps1'))
    Check ($hb.Contains('$askedBefore = -not $PackOnly -and (Test-KitPackAsked $askNote $askKey)')) 'pack ask: make-puppet-pack.cmd (-PackOnly) always asks'
    # hs-missing (a tester read "No co-op Hookshot game" as "couldn't find Hookshot"): antivirus took the module
    Remove-Item -LiteralPath (Join-Path $hk 'hookshot\hookshot_coop.dll')
    Check ((Get-KitHookshotNeed $hk) -eq 'quarantine') 'hookshot need: the module in neither the kit nor the game -> quarantine (no build)'
    Rename-Item -LiteralPath (Join-Path $hk 'hookshot\PIN') 'PIN.off'
    Check ((Get-KitHookshotNeed $hk) -ne 'quarantine') 'hookshot need: a kit made without the Hookshot part (no PIN) -> not quarantine'
    Rename-Item -LiteralPath (Join-Path $hk 'hookshot\PIN.off') 'PIN'
    Write-File (Join-Path $other 'bin\hookshot_coop.dll') 'MODULE 2'
    Check ((Get-KitHookshotNeed $hk) -ne 'quarantine') 'hookshot need: the kit''s module gone, the game''s there -> the game still starts'
    Remove-Item -LiteralPath (Join-Path $other 'bin\hookshot_coop.dll')
    Write-File (Join-Path $hk 'hookshot\hookshot_coop.dll') 'MODULE 2'
    foreach ($n in 'game', 'moved', 'pin', 'patches', 'module', 'quarantine') { Check ([bool](Get-KitHookshotNeedText $n)) "hookshot need text: $n" }
    foreach ($n in 'game', 'moved', 'pin') { Check ([bool](Get-KitHookshotNeedQuestion $n)) "hookshot need question: $n asks first" }
    foreach ($n in 'patches', 'module', 'quarantine', '') { Check (-not (Get-KitHookshotNeedQuestion $n)) "hookshot need question: none for '$n'" }
    Check ((Get-KitHookshotNeedText 'game') -match "isn't set up yet" -and (Get-KitHookshotNeedText 'game') -notmatch 'No co-op' -and (Get-KitHookshotNeedText 'game') -match '^Playing on the emulator\? Use Join\.bat') 'hookshot need text: game says first to use Join.bat on the emulator, then not set up yet'
    Check ((Get-KitHookshotNeedText 'moved' 'D:\Old\game') -match 'D:\\Old\\game' -and (Get-KitHookshotNeedText 'moved') -match '-Out') 'hookshot need text: moved names the folder and -Out'
    Check ((Get-KitHookshotNeedText 'quarantine') -match 'Antivirus' -and (Get-KitHookshotNeedText 'quarantine') -match 'Exclusions') 'hookshot need text: quarantine says antivirus and the exclusion steps'
    Check ((Get-KitHookshotNeedText 'pin') -match 'about 15 minutes') 'hookshot need text: a pin move says about 15 minutes'
    Check (-not (Get-KitHookshotNeedText '')) 'hookshot need text: nothing to say when nothing is needed'
    $g1 = Get-KitHookshotNeedText 'game' '' $true; $m1 = Get-KitHookshotNeedText 'moved' 'D:\Old\game' $true
    Check ($g1 -match "isn't set up yet" -and $g1 -notmatch 'emulator|Join\.bat') "hookshot need text, Hookshot-only kit: game names no emulator [$g1]"
    Check ($m1 -match 'D:\\Old\\game' -and $m1 -notmatch 'Build-Hookshot|-Out') "hookshot need text, Hookshot-only kit: moved names no Build-Hookshot-Coop.bat [$m1]"

    # check-hosting's port (Revirie's bug: Host-Hookshot.bat writes the Hookshot game's coop.ini, not the emulator's)
    Remove-Item -LiteralPath (Join-Path $hk 'hookshot\game-folder.txt')
    $emuIni = Join-Path $hk 'emulator\user\config\coop.ini'
    $hsIni = Join-Path $g 'coop.ini'
    Check ((Get-KitHostPort $hk @() 27510) -eq 27510) 'host port: no coop.ini -> the default'
    Write-File $emuIni "server = 127.0.0.1`nport = 27510`n"
    (Get-Item $emuIni).LastWriteTimeUtc = [datetime]::UtcNow.AddHours(-1)
    Write-File $hsIni "server = 127.0.0.1`nport = 27600`n"
    Check ((Get-KitHostPort $hk @() 27510) -eq 27600) 'host port: the coop.ini written last (Hookshot) wins'
    Check ((Get-KitHostPort $hk @(27510) 27510) -eq 27510) 'host port: the port the running server listens on wins'
    Check ((Get-KitHostPort $hk @(27999) 27510) -eq 27999) 'host port: a running server on a port no coop.ini names: that port'
    Check ((Get-KitHostPort $hk @(27510, 27600) 27510) -in 27510, 27600) 'host port: two listening: one of the kit''s'
} finally {
    Remove-Item -LiteralPath $hk -Recurse -Force -ErrorAction SilentlyContinue
}
# --- "Start Squadala.bat" (start.ps1, start-checks.ps1): the one entry point of the Hookshot-only kit and each
# thing that could go wrong before the game starts. play.ps1 is a stub here: it writes what it was given.
. (Join-Path $kitSrc 'scripts\start-checks.ps1')
# (a short name: the kits made under it are checked against start-checks.ps1's 80-letter limit, and the TEMP of GitHub's
# runners, C:\Users\runneradmin\AppData\Local\Temp\, is 40 letters already; "oot-start-test-<8>\kits\O'Brien's kit" made 82)
$sk = Join-Path ([System.IO.Path]::GetTempPath()) ('ost-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
try {
    # Still in the zip: Windows' temp copy of an open zip, a folder named like the zip, anywhere under TEMP.
    Check (Test-KitInZip 'C:\Users\x\AppData\Local\Temp\Temp1_oot-coop-v1.zip\oot-coop-v1' '') 'in zip: Temp1_<zip> folder'
    Check (Test-KitInZip 'C:\Downloads\oot-coop-v1.zip\oot-coop-v1' '') 'in zip: a folder named .zip'
    Check (Test-KitInZip 'C:\Users\x\AppData\Local\Temp\abc\oot-coop' 'C:\Users\x\AppData\Local\Temp') 'in zip: under TEMP'
    Check (-not (Test-KitInZip 'C:\OoT-Coop' 'C:\Users\x\AppData\Local\Temp')) 'not in zip: C:\OoT-Coop'
    # Places the game build cannot use; spaces are fine.
    Check ((Get-KitPathProblem 'C:\Users\x\OneDrive\Desktop\oot-coop' @('C:\Users\x\OneDrive')) -eq 'onedrive') 'path: inside OneDrive'
    Check ((Get-KitPathProblem 'C:\Users\x\OneDrive - Contoso\oot-coop' @()) -eq 'onedrive') 'path: a business OneDrive folder'
    Check ((Get-KitPathProblem "C:\Spiele\Zelda $([char]0x00DC)berall" @()) -eq 'unicode') 'path: a letter outside ASCII'
    Check ((Get-KitPathProblem "C:\Users\x\$([char]0x5C71)$([char]0x7530)\oot-coop" @()) -eq 'unicode') 'path: a user folder in Japanese'
    Check ((Get-KitPathProblem ('C:\' + ('a' * 90)) @()) -eq 'long') 'path: too long'
    Check ((Get-KitPathProblem 'C:\My Games\OoT Co-op' @()) -eq '') 'path: spaces are fine'
    foreach ($p in 'onedrive', 'unicode', 'long', 'readonly') { Check ([bool](Get-KitPathProblemText $p)) "path: '$p' has a plain-words text" }
    # The folder a kit is copied to: free, else the next name.
    $mt = Join-Path $sk 'move'
    Check ((Get-KitMoveTarget @("$mt\OoT-Coop")) -eq "$mt\OoT-Coop") 'move target: a free folder'
    Write-File "$mt\OoT-Coop\x.txt" 'taken'
    Check ((Get-KitMoveTarget @("$mt\OoT-Coop")) -eq "$mt\OoT-Coop-2") 'move target: taken -> the next name'
    # No admin rights: a folder this user cannot write is found (an ACL deny of this user's write).
    $ro = Join-Path $sk 'readonly'
    New-Item -ItemType Directory -Force -Path $ro | Out-Null
    Check (Test-KitWritable $ro) 'writable: a normal folder'
    & icacls.exe $ro /deny "$($env:USERNAME):(OI)(CI)(W,AD)" | Out-Null
    try { Check (-not (Test-KitWritable $ro)) 'writable: a folder this user may not write is said' }
    finally { & icacls.exe $ro /remove:d "$env:USERNAME" | Out-Null }
    # Missing kit files (MANIFEST.txt): an antivirus (.exe/.dll) or a broken unzip.
    $mk = Join-Path $sk 'manifest'
    Write-File "$mk\MANIFEST.txt" "# Files`r`nMANIFEST.txt`r`nhookshot\hookshot_coop.dll`r`ndocs\SETUP.md`r`n"
    Write-File "$mk\docs\SETUP.md" 'x'
    $miss = @(Get-KitMissingFiles $mk)
    Check ($miss.Count -eq 1 -and $miss[0] -eq 'hookshot\hookshot_coop.dll') "missing: the module is found missing [$miss]"
    Check (((Get-KitMissingFilesText $miss) -join ' ') -match 'antivirus') 'missing: a .dll -> the antivirus text'
    Check (((Get-KitMissingFilesText @('docs\SETUP.md')) -join ' ') -match 'unzip') 'missing: another file -> unzip again'
    # The Visual C++ runtime: in the kit's hookshot\ or in Windows.
    $rk = Join-Path $sk 'runtime'
    New-Item -ItemType Directory -Force -Path "$rk\sys", "$rk\kit\hookshot" | Out-Null
    Check (@(Get-KitMissingRuntime "$rk\kit" "$rk\sys").Count -eq 3) 'runtime: none -> all three named'
    foreach ($d in 'vcruntime140.dll', 'vcruntime140_1.dll') { Write-File "$rk\kit\hookshot\$d" 'x' }
    Write-File "$rk\sys\msvcp140.dll" 'x'
    Check (@(Get-KitMissingRuntime "$rk\kit" "$rk\sys").Count -eq 0) 'runtime: in the kit and in Windows -> fine'
    # A built game an antivirus took files from: said, not built again.
    $qg = Join-Path $sk 'qgame'
    Write-File "$qg\coop-built-from.txt" 'stamp'
    Check ((Get-KitGameQuarantined $qg) -eq 'longshot.exe') 'quarantine: the game exe gone'
    Write-File "$qg\bin\longshot.exe" 'x'
    Check ((Get-KitGameQuarantined $qg) -eq 'hookshot_coop.dll') 'quarantine: the module gone'
    Write-File "$qg\bin\hookshot_coop.dll" 'x'
    Check ((Get-KitGameQuarantined $qg) -eq '') 'quarantine: all there'
    Check ((Get-KitGameQuarantined (Join-Path $sk 'nogame')) -eq '') 'quarantine: no game built yet is not one'
    # Other kits on this PC: listed, and the game file of an older one reused.
    $list = Join-Path $sk 'appdata\kits.txt'
    $old = Join-Path $sk 'old kit'; $new = Join-Path $sk 'new kit'
    Write-File "$old\VERSION.txt" "kit: oot-coop-v1`r`n"; Write-File "$new\VERSION.txt" "kit: oot-coop-v2`r`n"
    $nup = Join-Path $sk 'game.nup'; Write-File $nup 'x'
    Write-File "$old\hookshot\game-files.txt" "$nup`n"
    Check (@(Register-KitFolder $list $old).Count -eq 0) 'kits: the first kit has no others'
    $others = @(Register-KitFolder $list $new)
    Check ($others.Count -eq 1 -and $others[0] -eq $old) 'kits: the second sees the first'
    Check ((Get-KitName $old) -eq 'oot-coop-v1') 'kits: the name from VERSION.txt'
    Check ((Copy-KitGameFilesFrom $new $others) -eq $nup -and (Get-Content "$new\hookshot\game-files.txt") -eq $nup) 'kits: the new kit takes the old one''s game file (no question)'
    Check ((Copy-KitGameFilesFrom $new $others) -eq '') 'kits: a kit with its own game file keeps it'

    # Whole starts of a Hookshot-only kit, with a stub play.ps1.
    function New-StartKit([string]$Dir) {
        Copy-KitFile (Join-Path $kitSrc 'Start Squadala.bat') (Join-Path $Dir 'Start Squadala.bat')
        foreach ($f in 'start.ps1', 'start-checks.ps1', 'kit-paths.ps1', 'windows-checks.ps1') { Copy-KitFile (Join-Path $kitSrc "scripts\$f") (Join-Path $Dir "scripts\$f") }
        Write-File "$Dir\scripts\play.ps1" ("param([string]`$Mode, [switch]`$Hookshot, [switch]`$NoUpdate)`r`n" +
            "[System.IO.File]::WriteAllText((Join-Path (Split-Path -Parent `$PSScriptRoot) 'played.txt'), `"`$Mode `$Hookshot `$NoUpdate `$env:OOT_COOP_KIT`")`r`n" +
            "if (`$env:OOT_TEST_PLAY_MS) { Start-Sleep -Milliseconds ([int]`$env:OOT_TEST_PLAY_MS) }`r`nexit 0`r`n")
        foreach ($d in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') { Write-File "$Dir\hookshot\$d" 'x' }
        Write-File "$Dir\VERSION.txt" "kit: oot-coop-v2`r`n"
        Write-File "$Dir\MANIFEST.txt" "# Files`r`nMANIFEST.txt`r`nVERSION.txt`r`nscripts\start.ps1`r`nhookshot\vcruntime140.dll`r`n"
    }
    $tmpFor = Join-Path $sk 'tmp'
    New-Item -ItemType Directory -Force -Path $tmpFor, "$sk\desk", "$sk\appdata" | Out-Null
    function Set-StartEnv([bool]$On) {
        if ($On) {
            $script:keepEnv = @($env:TEMP, $env:TMP, $env:LOCALAPPDATA)
            $env:TEMP = $tmpFor; $env:TMP = $tmpFor; $env:LOCALAPPDATA = "$sk\appdata"; $env:OOT_COOP_DESKTOP = "$sk\desk"
        } else {
            $env:TEMP = $script:keepEnv[0]; $env:TMP = $script:keepEnv[1]; $env:LOCALAPPDATA = $script:keepEnv[2]
            Remove-Item env:OOT_COOP_DESKTOP -ErrorAction SilentlyContinue
        }
    }
    function Start-Kit([string]$Kit, [string]$Answer = '', [string]$Bat = 'Start Squadala.bat') {
        $in = Join-Path $sk 'answer.txt'
        [System.IO.File]::WriteAllText($in, "$Answer`r`n")
        Set-StartEnv $true
        try { $o = (cmd /c "call `"$Kit\$Bat`" -NoUpdate < `"$in`"" 2>&1) -join '|'; $c = $LASTEXITCODE }
        finally { Set-StartEnv $false }
        return @($c, $o)
    }
    $k = Join-Path $sk 'kits\OoT Co-op'
    New-StartKit $k
    $r = Start-Kit $k
    Check ($r[0] -eq 0 -and (Read-File "$k\played.txt") -eq "choose True True $k") "start: a good kit hands over to play.ps1 choose -Hookshot with the kit named [$($r[1])] [$(Read-File "$k\played.txt")]"
    # (kit-friction review) a ' in the kit's path: the Windows checks get the folder from a variable, never in their code
    $ka = Join-Path $sk "kits\O'Brien's kit"
    New-StartKit $ka
    $r = Start-Kit $ka
    Check ($r[0] -eq 0 -and (Read-File "$ka\played.txt") -eq "choose True True $ka" -and (Test-Path -LiteralPath "$ka\logs\windows-checks.txt") -and $r[1] -notmatch 'Unexpected token') "start: a kit path with an apostrophe starts, the Windows checks ran [$($r[1])]"
    Remove-Item -LiteralPath $ka -Recurse -Force -ErrorAction SilentlyContinue
    # A desktop shortcut to another kit is moved to the kit started last; its name up to v0.11.2 ("Play OoT Co-op",
    # to "Start OoT Co-op.bat") becomes "Play Squadala".
    $sh = New-Object -ComObject WScript.Shell
    $l = $sh.CreateShortcut("$sk\desk\Play OoT Co-op.lnk"); $l.TargetPath = "$old\Start OoT Co-op.bat"; $l.Save()
    [void](Start-Kit $k)
    Check (-not (Test-Path -LiteralPath "$sk\desk\Play OoT Co-op.lnk") -and (Test-Path -LiteralPath "$sk\desk\Play Squadala.lnk")) 'start: the old desktop shortcut "Play OoT Co-op" is now "Play Squadala"'
    Check ($sh.CreateShortcut("$sk\desk\Play Squadala.lnk").TargetPath -ieq "$k\Start Squadala.bat") 'start: the desktop shortcut follows the kit started last'
    $l = $sh.CreateShortcut("$sk\desk\Play Squadala.lnk"); $l.TargetPath = "$old\Start Squadala.bat"; $l.Save()
    [void](Start-Kit $k)
    Check ($sh.CreateShortcut("$sk\desk\Play Squadala.lnk").TargetPath -ieq "$k\Start Squadala.bat") 'start: "Play Squadala" follows the kit started last too'
    Remove-Item -LiteralPath "$sk\desk\Play Squadala.lnk" -Force
    # Started from inside the zip (only the .bat unpacked to a temp folder): the .bat itself says so.
    $z = Join-Path $sk 'Temp1_oot-coop.zip'
    Copy-KitFile (Join-Path $kitSrc 'Start Squadala.bat') (Join-Path $z 'Start Squadala.bat')
    $r = Start-Kit $z
    Check ($r[0] -ne 0 -and $r[1] -match 'still inside its zip' -and $r[1] -match 'Extract All') "start: only the .bat (from an open zip): says unzip first [$($r[1])]"
    # The whole kit run from Windows' temp copy of the zip.
    $zk = Join-Path $tmpFor 'Temp1_oot-coop.zip\oot-coop'
    New-StartKit $zk
    $r = Start-Kit $zk
    Check ($r[0] -ne 0 -and $r[1] -match 'still inside its zip' -and -not (Test-Path "$zk\played.txt")) "start: the kit in TEMP: says unzip first, no game [$($r[1])]"
    # An antivirus took a kit file.
    Remove-Item -LiteralPath "$k\hookshot\vcruntime140.dll", "$k\played.txt"
    $r = Start-Kit $k
    Check ($r[0] -ne 0 -and $r[1] -match 'antivirus' -and $r[1] -match 'vcruntime140\.dll' -and -not (Test-Path "$k\played.txt")) "start: a kit file quarantined: says so, no game [$($r[1])]"
    Write-File "$k\hookshot\vcruntime140.dll" 'x'
    # No Visual C++ runtime anywhere (not in the kit; a test cannot take Windows' own away, so only when Windows has none).
    # The game an antivirus emptied: said, not built again.
    Write-File "$k\hookshot\game\coop-built-from.txt" 'stamp'
    $r = Start-Kit $k
    Check ($r[0] -ne 0 -and $r[1] -match 'antivirus' -and $r[1] -match 'longshot\.exe' -and $r[1] -match 'not built again') "start: the game's exe quarantined: says so [$($r[1])]"
    Remove-Item -LiteralPath "$k\hookshot\game" -Recurse
    # A second double-click while the first start is busy.
    Remove-Item -LiteralPath "$k\played.txt" -ErrorAction SilentlyContinue
    $env:OOT_TEST_PLAY_MS = '6000'
    Set-StartEnv $true
    try { $first = Start-Process -FilePath $ps -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$k\scripts\start.ps1`"", '-NoUpdate') }
    finally { Set-StartEnv $false }
    try {
        for ($i = 0; $i -lt 100 -and -not (Test-Path "$k\played.txt"); $i++) { Start-Sleep -Milliseconds 100 }
        $r = Start-Kit $k
        Check ($r[0] -ne 0 -and $r[1] -match 'already starting in another window') "start: a double start is refused while the first runs [$($r[1])]"
    } finally { $first.WaitForExit(15000) | Out-Null; Remove-Item env:OOT_TEST_PLAY_MS }
    $r = Start-Kit $k
    Check ($r[0] -eq 0) "start: after the first window ends, a start works again [$($r[1])]"
    # A folder the build cannot use (letters outside ASCII), no game yet: asked once; no -> what to do, no game.
    $uk = Join-Path $sk "kits\Zelda $([char]0x00DC)ber"
    New-StartKit $uk
    $r = Start-Kit $uk 'n'
    Check ($r[0] -ne 0 -and $r[1] -match 'letters the game build cannot handle' -and $r[1] -match 'yourself' -and -not (Test-Path "$uk\played.txt")) "start: a unicode path, answer no: says where to move it [$($r[1])]"
    # A game built there already: no question (it works there).
    Write-File "$uk\hookshot\game\bin\longshot.exe" 'x'; Write-File "$uk\hookshot\game\bin\hookshot_coop.dll" 'x'
    $r = Start-Kit $uk
    Check ($r[0] -eq 0 -and (Test-Path "$uk\played.txt")) "start: a unicode path with the game built: starts [$($r[1])]"
} finally {
    Remove-Item -LiteralPath $sk -Recurse -Force -ErrorAction SilentlyContinue
}
# The one entry point: one command line (an update may replace it while it runs); the kit README is ten lines at most.
$startLine = @(Get-Content (Join-Path $kitSrc 'Start Squadala.bat') | Where-Object { $_ -and $_ -notmatch '^(@echo off|rem )' })
Check ($startLine.Count -eq 1 -and $startLine[0] -match 'scripts\\start\.ps1" %\*' -and $startLine[0] -match '& exit /b 0$') 'Start Squadala.bat: start.ps1, one line'
Check (-not (Test-Path -LiteralPath (Join-Path $kitSrc 'Start OoT Co-op.bat'))) 'one start file: no Start OoT Co-op.bat (its old name, up to v0.11.3) in the kit'
Check (@(Get-Content (Join-Path $kitSrc 'README.txt')).Count -le 10) 'README.txt: ten lines at most'
# --- hs-missing: Host-/Join-Hookshot.bat with no co-op Hookshot game say why in plain words and offer the build ---
$hm = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-hs-missing-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    New-Kit $hm 'v0.1.0-beta4' 'hm'
    Write-File (Join-Path $hm 'hookshot\PIN') "repo=https://github.com/NotRexed/Hookshot`ncommit=4aee23659b9436dab2cd7be1ac07333c06246b7c`n"
    Write-File (Join-Path $hm 'hookshot\TOOLS.txt') "release=v2.2.0-rc.1`nsetup_sha256=b3`n"
    Write-File (Join-Path $hm 'hookshot\hookshot_coop.dll') 'MODULE'
    $no = Join-Path $hm 'no.txt'; [System.IO.File]::WriteAllText($no, "n`r`n")
    $runHs = { param($Mode) (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$hm\scripts\play.ps1`" $Mode -Server 127.0.0.1 -Name Saria -Hookshot -NoUpdate < `"$no`"" 2>&1) -join '|' }
    $built = { Test-Path -LiteralPath (Join-Path $hm 'hookshot\tools') }
    # never built: plain words, the offer, n = no build, a pointer to Build-Hookshot-Coop.bat
    $out = & $runHs 'join'
    Check ($out -match 'Playing on the emulator\? Use Join\.bat \(or Host\.bat\).*\|Hookshot isn''t set up yet' -and $out -match 'Set it up now\? \(then the game starts; closing this window cancels\) \[Y/n\]' -and $out -match 'Not now\. Build-Hookshot-Coop\.bat' -and $out -notmatch 'Game started' -and -not (& $built)) "Join-Hookshot, no game: says not set up yet, offers the build, n builds nothing [$out]"
    $out = & $runHs 'host'
    Check ($out -match "Hookshot isn't set up yet" -and $out -match 'Set it up now\?' -and $out -notmatch 'Game started') "Host-Hookshot, no game: the same offer [$out]"
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$hm\scripts\play.ps1`" choose -Hookshot -NoUpdate < `"$no`"" 2>&1) -join '|'
    Check ($out -match "Hookshot isn't set up yet" -and $out -match 'Set it up now\?' -and $out -notmatch 'Game started') "Play-OoT-Coop, no game: the same offer [$out]"
    # the built game moved or deleted: names where it was and -Out
    $gone = Join-Path $hm 'moved away\game'
    Write-File (Join-Path $hm 'hookshot\game-folder.txt') "$gone`n"
    $out = & $runHs 'join'
    Check ($out -match 'not in .*moved away.game any more \(moved or deleted\)' -and $out -match 'Build-Hookshot-Coop\.bat -Out' -and $out -match 'Build it again now\?' -and -not (& $built)) "Join-Hookshot, the game moved: says so, -Out, offers a new build [$out]"
    # built for an older pin: the rebuild offer
    $g = Join-Path $hm 'hookshot\game'
    Write-File (Join-Path $hm 'hookshot\game-folder.txt') "$g`n"
    Write-File (Join-Path $g 'longshot.exe') 'exe'
    Write-File (Join-Path $g 'hookshot_coop.dll') 'MODULE'
    Write-File (Join-Path $g 'coop-built-from.txt') "release=v2.1.0`ncommit=0000000000000000000000000000000000000000`npatches="
    $out = & $runHs 'join'
    Check ($out -match 'built for another game version' -and $out -match 'Update it now\?' -and $out -notmatch 'Game started' -and -not (& $built)) "Join-Hookshot, an older pin: the rebuild offer [$out]"
    # the module taken (antivirus): no build offered, the restore and exclusion steps
    Remove-Item -LiteralPath (Join-Path $g 'hookshot_coop.dll'), (Join-Path $hm 'hookshot\hookshot_coop.dll')
    $out = & $runHs 'join'
    Check ($out -match 'hookshot_coop\.dll is missing' -and $out -match 'Antivirus' -and $out -match 'Protection history' -and $out -match 'Exclusions' -and $out -notmatch '\[Y/n\]' -and $out -notmatch 'Game started' -and -not (& $built)) "Join-Hookshot, the module quarantined: antivirus steps, no build [$out]"
    # the game's copy gone after it was put in (a kit without a pin to build: the last check before the start)
    Remove-Item -LiteralPath (Join-Path $hm 'hookshot\PIN')
    Write-File (Join-Path $hm 'hookshot\hookshot_coop.dll') 'MODULE'
    $out = & $runHs 'join'
    Check ($out -match 'hookshot_coop\.dll is missing' -and $out -match 'Antivirus' -and $out -notmatch 'No co-op Hookshot game' -and $out -notmatch 'Game started') "Join-Hookshot, the game's module gone: antivirus steps, not 'No co-op Hookshot game' [$out]"
} finally {
    Remove-Item -LiteralPath $hm -Recurse -Force -ErrorAction SilentlyContinue
}
# The new launcher is one command line too (the update may replace it while it runs).
$last = @(Get-Content (Join-Path $kitSrc 'Play-OoT-Coop.bat') | Where-Object { $_ -and $_ -notmatch '^(@echo off|rem )' })
Check ($last.Count -eq 1 -and $last[0] -match 'play\.ps1" choose -Hookshot' -and $last[0] -match '& exit /b$') 'Play-OoT-Coop.bat: play.ps1 choose -Hookshot, one line'

# tools-move (a 0.10.1 player: "Access to the path '...\hookshot\tools.new' is denied" on the first build): a file
# in the folder still held (antivirus, Explorer, a python.exe just ended) -> tried again, then robocopy /MOVE
$mv = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-move-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    Write-File "$mv\tools.new\python\python.exe" 'stub'
    Write-File "$mv\tools.new\nx2\make_game.py" 'x'
    $script:held = [System.IO.File]::Open("$mv\tools.new\python\python.exe", 'Open', 'Read', 'Read')
    $script:waits = 0
    $why = Move-KitFolderRetry "$mv\tools.new" "$mv\tools" 10 100 { $script:waits++; if ($script:waits -eq 2) { $script:held.Dispose() } }
    Check ($why -eq '' -and $script:waits -ge 2 -and $script:waits -lt 20 -and (Test-Path "$mv\tools\nx2\make_game.py") -and -not (Test-Path "$mv\tools.new")) "tools-move: a file held for a moment -> tried again, then moved [$why, waits $script:waits]"
    Remove-Item -LiteralPath "$mv\tools" -Recurse -Force
    Write-File "$mv\tools.new\python\python.exe" 'stub'
    Write-File "$mv\tools.new\nx2\make_game.py" 'x'
    $script:held = [System.IO.File]::Open("$mv\tools.new\python\python.exe", 'Open', 'Read', 'Read')
    try {
        $why = Move-KitFolderRetry "$mv\tools.new" "$mv\tools" 1 100
        Check ($why -eq '' -and (Test-Path "$mv\tools\nx2\make_game.py") -and (Read-File "$mv\tools\python\python.exe") -eq 'stub') "tools-move: held the whole time -> robocopy /MOVE copies it all [$why]"
        $why = Remove-KitFolderRetry "$mv\tools.new" 1 100
        Check ([bool]$why) "tools-move: a folder that stays held -> why, not a crash [$why]"
        # (kit-friction: who holds it, by name. Here this test's own PowerShell holds python.exe; Restart Manager names it)
        $who = @(Get-KitFolderHolders "$mv\tools.new")
        Check ($who.Count -ge 1 -and ($who -join ',') -match "process $PID\)") "tools-move: the holder is named (Restart Manager) [$($who -join ', ')]"
        $txt = Get-KitHeldFolderText "$mv\tools.new" $why
        Check ($txt -match "Something still has a file in it open: .*process $PID\)") "tools-move: the message names the holder [$txt]"
    } finally { $script:held.Dispose() }
    Check ((Remove-KitFolderRetry "$mv\tools.new" 5 100) -eq '' -and -not (Test-Path "$mv\tools.new")) 'tools-move: let go -> removed'
    Check ((Remove-KitFolderRetry "$mv\none" 1 100) -eq '') 'tools-move: nothing to remove -> fine'
    $txt = Get-KitHeldFolderText 'D:\coop\hookshot\tools.new' 'Access denied'
    Check ($txt -match 'tools\.new' -and $txt -match 'antivirus' -and $txt -match 'Exclusions' -and $txt -match 'Explorer') "tools-move: the message names the folder, antivirus, Explorer and the exclusion [$txt]"
    $txt = Get-KitHeldFolderText 'D:\coop\hookshot\tools.new' 'Access denied' @('MsMpEng.exe (process 4)') 'GoogleDrive'
    Check ($txt -match 'open: MsMpEng\.exe \(process 4\)\.' -and $txt -match 'cloud-sync folder \(GoogleDrive\)' -and $txt -match 'move the kit out of it') "tools-move: in a sync folder: says so, and to move the kit [$txt]"
    Check ((Get-KitSyncRoot ([System.IO.Path]::GetTempPath())) -eq '') 'tools-move: the temp folder is in no sync folder'
} finally {
    Remove-Item -LiteralPath $mv -Recurse -Force -ErrorAction SilentlyContinue
}
# kit-copy, the build itself: a second kit (here a 0.10.1 copy naming the first kit's game) copies the first kit's
# built game into its own hookshot\game instead of building (no game files, no network: the tools are stubbed ready),
# without the first kit's coop.ini and coop-options.ini, and notes its game folder relative to the kit
$kc = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-kitcopy-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    $commit = 'e7ed2f7a6526cd6b841b86678b3ac96eb6e5b1c8'
    foreach ($k in "$kc\kit-1", "$kc\kit-1 - Copy") {
        Copy-Item -LiteralPath (Join-Path $kitSrc 'scripts') -Destination "$k\scripts" -Recurse
        Copy-Item -LiteralPath (Join-Path $kitSrc 'coop.ini.template') -Destination "$k\scripts\coop.ini.template"   # (as make_kit.ps1 lays it out)
        Write-File "$k\hookshot\PIN" "runtime=Longshot`nrepo=https://git.magicbean.dev/MagicBeanDev/Longshot`ncommit=$commit`n"
        Write-File "$k\hookshot\TOOLS.txt" "release=v3.0.0-rc.3`nlock_sha256=b3`nsource_commit=$commit`nsource_sha256=aa`n"
        Write-File "$k\hookshot\patches\0001-a.patch" "one`n"
        Write-File "$k\hookshot\hookshot_coop.dll" 'MODULE'
        Write-File "$k\Start Squadala.bat" '@echo off'
        Write-File "$k\VERSION.txt" "kit: oot-coop-test`n"
    }
    $k1 = "$kc\kit-1"; $k2 = "$kc\kit-1 - Copy"
    $hash = (Get-FileHash -LiteralPath "$k1\hookshot\patches\0001-a.patch" -Algorithm SHA256).Hash.ToLowerInvariant()
    $stampKc = "release=v3.0.0-rc.3`ncommit=$commit`npatches=$hash"
    Check ((Get-KitHookshotStamp $k1) -eq $stampKc) 'kit-copy build: the build''s stamp is Get-KitHookshotStamp''s'
    foreach ($k in $k1, $k2) { Write-File "$k\hookshot\tools\coop-tools.txt" $stampKc; Write-File "$k\hookshot\tools\python\python.exe" 'stub' }
    Write-File "$k1\hookshot\game\bin\longshot.exe" 'exe'
    Write-File "$k1\hookshot\game\coop-built-from.txt" $stampKc
    Write-File "$k1\hookshot\game\bin\coop.ini" "name = Saria`n"
    Write-File "$k1\hookshot\game\bin\coop-options.ini" "LICHII_COOP_PAD=1`n"
    Write-File "$k1\hookshot\game\bin\hookshot_log.txt" 'kit 1 log'
    Write-File "$k1\hookshot\game-folder.txt" "$k1\hookshot\game`n"
    Write-File "$k2\hookshot\game-folder.txt" "$k1\hookshot\game`n"   # what a Windows copy of a 0.10.1 kit has
    Write-File "$k2\coop-options.ini" "LICHII_COOP_PAD=2`n"
    $keepKc = @($env:LOCALAPPDATA, $env:OOT_COOP_NO_SHORTCUT)
    $env:LOCALAPPDATA = "$kc\appdata"; $env:OOT_COOP_NO_SHORTCUT = '1'; $env:OOT_COOP_NO_LONGSHOT_ASK = '1'   # (kit-friction: no "Built Longshot yourself?" here)
    $ErrorActionPreference = 'Continue'
    try { $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File "$k2\scripts\hookshot-build.ps1" 2>&1) -join '|'; $code = $LASTEXITCODE }
    finally { $env:LOCALAPPDATA = $keepKc[0]; $env:OOT_COOP_NO_SHORTCUT = $keepKc[1]; Remove-Item env:OOT_COOP_NO_LONGSHOT_ASK -ErrorAction SilentlyContinue; $ErrorActionPreference = 'Stop' }
    $b2 = "$k2\hookshot\game\bin"
    Check ($code -eq 0 -and $out -match 'copying the one another kit on this PC built' -and $out -match 'starts with that kit''s saves' -and $out -notmatch 'Building the game' -and (Test-Path "$b2\longshot.exe")) "kit-copy build: the copy gets its own game, copied not built [$code $out]"
    Check ((Read-File "$k2\hookshot\game-folder.txt").Trim() -eq 'hookshot\game') "kit-copy build: game-folder.txt is relative [$(Read-File "$k2\hookshot\game-folder.txt")]"
    Check ((Read-File "$b2\coop.ini") -notmatch 'Saria' -and (Read-File "$b2\coop-options.ini") -match 'PAD=2' -and -not (Test-Path "$b2\hookshot_log.txt")) 'kit-copy build: its own coop.ini and coop-options.ini, no log of the other kit'
    Check ((Get-KitHookshotGame $k2) -eq "$k2\hookshot\game" -and (Get-KitHookshotGame $k1) -eq "$k1\hookshot\game") 'kit-copy build: two kits, two games'
    # an unzipped-twice kit (no game-folder.txt, found through kits.txt) copies the same way
    Write-File "$kc\appdata\OoT-Coop\kits.txt" "$k1`n"
    $k3 = "$kc\kit-2"
    Copy-Item -LiteralPath $k2 -Destination $k3 -Recurse
    Remove-Item -LiteralPath "$k3\hookshot\game", "$k3\hookshot\game-folder.txt" -Recurse -Force
    $env:LOCALAPPDATA = "$kc\appdata"; $env:OOT_COOP_NO_SHORTCUT = '1'; $env:OOT_COOP_NO_LONGSHOT_ASK = '1'
    $ErrorActionPreference = 'Continue'
    try { $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File "$k3\scripts\hookshot-build.ps1" 2>&1) -join '|'; $code = $LASTEXITCODE }
    finally { $env:LOCALAPPDATA = $keepKc[0]; $env:OOT_COOP_NO_SHORTCUT = $keepKc[1]; Remove-Item env:OOT_COOP_NO_LONGSHOT_ASK -ErrorAction SilentlyContinue; $ErrorActionPreference = 'Stop' }
    Check ($code -eq 0 -and $out -match 'copying the one another kit' -and (Test-Path "$k3\hookshot\game\bin\longshot.exe")) "kit-copy build: a kit unzipped twice copies the first one's game too [$code $out]"
    # (kit-friction) the same with a Longshot of the player's own given: a nightly, so its saves only
    $k4 = "$kc\kit-3"
    Copy-Item -LiteralPath $k2 -Destination $k4 -Recurse
    Remove-Item -LiteralPath "$k4\hookshot\game", "$k4\hookshot\game-folder.txt" -Recurse -Force
    $own = "$kc\My Longshot"
    Write-File "$own\bin\image.bin" 'image'
    Write-File "$own\longshot_version.txt" "version=nightly-2026-11-12`n"
    Write-File "$own\bin\user\save\5a3c9e1f0b7d24680000000000000001\slot_00\progress.sav" 'MINE'
    Write-File "$k1\hookshot\game\bin\image.bin" 'image'
    $env:LOCALAPPDATA = "$kc\appdata"; $env:OOT_COOP_NO_SHORTCUT = '1'
    $ErrorActionPreference = 'Continue'
    try { $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File "$k4\scripts\hookshot-build.ps1" -Longshot $own 2>&1) -join '|'; $code = $LASTEXITCODE }
    finally { $env:LOCALAPPDATA = $keepKc[0]; $env:OOT_COOP_NO_SHORTCUT = $keepKc[1]; $ErrorActionPreference = 'Stop' }
    Check ($code -eq 0 -and $out -match 'Found Longshot nightly-2026-11-12' -and $out -match 'Co-op is built on v3\.0\.0-rc\.3, so it is built fresh' -and
        (Read-File "$k4\hookshot\game\bin\user\save\5a3c9e1f0b7d24680000000000000001\slot_00\progress.sav") -eq 'MINE' -and $out -match 'Copied your Longshot save slot\(s\) slot_00') "longshot build: a nightly is said to be another version, its save is copied in [$code $out]"
    # revert-b: a kit gone back to a version on another Longshot commit (its game was built for the newer one): the
    # build of this version in the PC's game cache (OOT_COOP_HS_CACHE, hs.py's layout) is copied over the game instead
    # of a 15 to 45 minute build; the player's saves, settings, coop.ini and the romfs link stay as they were
    $k5 = "$kc\kit-reverted"
    Copy-Item -LiteralPath $k2 -Destination $k5 -Recurse
    Remove-Item -LiteralPath "$k5\hookshot\game", "$k5\hookshot\game-folder.txt" -Recurse -Force
    $older = 'c' * 40
    Write-File "$k5\hookshot\PIN" "runtime=Longshot`nrepo=https://git.magicbean.dev/MagicBeanDev/Longshot`ncommit=$older`n"
    Write-File "$k5\hookshot\TOOLS.txt" "release=v3.0.0-rc.3`nlock_sha256=b3`nsource_commit=$older`nsource_sha256=aa`n"
    $stampOld = "release=v3.0.0-rc.3`ncommit=$older`npatches=$hash"
    Write-File "$k5\hookshot\tools\coop-tools.txt" $stampOld
    $g5 = "$k5\hookshot\game"
    Write-File "$g5\longshot.exe" 'NEWER BUILD'
    Write-File "$g5\coop-built-from.txt" $stampKc
    Write-File "$g5\save\slot_00\progress.sav" 'MY SAVE'
    Write-File "$g5\coop.ini" "name = Saria`n"
    Write-File "$g5\longshot_options.ini" 'my graphics'
    Write-File "$g5\romfs\Actor\x.bin" 'my game files'
    Write-File "$k5\hookshot\game-folder.txt" "hookshot\game`n"
    $cache = "$kc\hs-cache"
    Write-File "$cache\0123456789abcdef0123\stamp.txt" ($stampOld.Replace("`n", "`r`n") + "`r`n")
    Write-File "$cache\0123456789abcdef0123\game\longshot.exe" 'OLDER BUILD'
    Write-File "$cache\0123456789abcdef0123\game\coop-built-from.txt" $stampOld
    Write-File "$cache\0123456789abcdef0123\game\longshot_options.ini" 'cache graphics'
    Write-File "$cache\0123456789abcdef0123\game\save\slot_00\progress.sav" 'NOT MINE'
    Write-File "$cache\fedcba9876543210fedc\stamp.txt" $stampKc
    Write-File "$cache\fedcba9876543210fedc\game\longshot.exe" 'NEWER BUILD'
    Check ((Find-KitHookshotCachedGame $stampOld $cache) -eq "$cache\0123456789abcdef0123\game") 'revert game: the cache entry of a stamp is found (its CRLF stamp.txt too)'
    Check (-not (Find-KitHookshotCachedGame "$stampOld-x" $cache) -and -not (Find-KitHookshotCachedGame $stampOld "$kc\no-cache")) 'revert game: another stamp, or no cache -> none'
    # NVIDIA DLSS (#234): a kit that opted in takes only a cached game built with it, and one that did not only one without
    Check (-not (Find-KitHookshotCachedGame $stampOld $cache -Dlss $true)) 'dlss: a kit with DLSS on does not take a cached game without it'
    Write-File "$cache\0123456789abcdef0123\game\ngx\nvngx_dlss.dll" 'NVIDIA'
    Check ((Find-KitHookshotCachedGame $stampOld $cache -Dlss $true) -eq "$cache\0123456789abcdef0123\game" -and
        -not (Find-KitHookshotCachedGame $stampOld $cache)) 'dlss: a cached game with DLSS goes only to a kit with DLSS on'
    Remove-Item -LiteralPath "$cache\0123456789abcdef0123\game\ngx" -Recurse -Force
    # (this kit's game had DLSS once; the kit has no dlss.txt: the copy without DLSS takes NVIDIA's runtime away too)
    Write-File "$g5\ngx\nvngx_dlss.dll" 'NVIDIA'
    $env:LOCALAPPDATA = "$kc\appdata"; $env:OOT_COOP_NO_SHORTCUT = '1'; $env:OOT_COOP_NO_LONGSHOT_ASK = '1'; $env:OOT_COOP_HS_CACHE = $cache
    $ErrorActionPreference = 'Continue'
    try { $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File "$k5\scripts\hookshot-build.ps1" 2>&1) -join '|'; $code = $LASTEXITCODE }
    finally { $env:LOCALAPPDATA = $keepKc[0]; $env:OOT_COOP_NO_SHORTCUT = $keepKc[1]; Remove-Item env:OOT_COOP_NO_LONGSHOT_ASK, env:OOT_COOP_HS_CACHE -ErrorAction SilentlyContinue; $ErrorActionPreference = 'Stop' }
    Check ($code -eq 0 -and $out -match 'from the game cache on this PC' -and $out -notmatch 'Building the game' -and (Read-File "$g5\longshot.exe") -eq 'OLDER BUILD') "revert game: the cached build is copied in, not built [$code $out]"
    Check ((Read-File "$g5\coop-built-from.txt") -eq $stampOld) 'revert game: the game''s stamp is this version''s'
    Check (-not (Test-Path -LiteralPath "$g5\ngx")) 'dlss: a copy without DLSS into a kit without it leaves no NVIDIA runtime behind'
    Check ((Read-File "$g5\save\slot_00\progress.sav") -eq 'MY SAVE' -and (Read-File "$g5\longshot_options.ini") -eq 'my graphics' -and
        (Read-File "$g5\romfs\Actor\x.bin") -eq 'my game files' -and (Read-File "$g5\coop.ini") -match 'Saria') 'revert game: saves, settings, coop.ini and the game files stay the player''s'
} finally {
    Remove-Item -LiteralPath $kc -Recurse -Force -ErrorAction SilentlyContinue
}

# pack-retry: hookshot-build.ps1 -PackOnly (make-puppet-pack.cmd on a Longshot kit, and the pack step of every start):
# a .nup that moved is found, a pack tool error is said, no game is said. Needs a real Python for the kit's tools\python
# (TEST_PYTHON, else python on PATH): a junction to its folder; skipped without one.
$pyExe = if ($env:TEST_PYTHON) { $env:TEST_PYTHON } else { (Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1).Source }
$pyHome = ''
# (the Windows Store stub, WindowsApps\python.exe, is found and exists but only prints "Python was not found" to
# stderr, which this script's Stop preference turns into an error: no real Python, so the cases are skipped)
if ($pyExe -and $pyExe -notmatch '\\WindowsApps\\' -and (Test-Path -LiteralPath $pyExe)) {
    try { $pyHome = "$(& $pyExe -I -c 'import sys; print(sys.base_prefix)' 2>$null)".Trim() } catch { $pyHome = '' }
}
if ($pyHome -and (Test-Path -LiteralPath (Join-Path $pyHome 'python.exe'))) {
    $pk = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-packonly-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    try {
        $k = "$pk\kit"
        Copy-Item -LiteralPath (Join-Path $kitSrc 'scripts') -Destination "$k\scripts" -Recurse
        $commit = 'a' * 40
        Write-File "$k\hookshot\PIN" "runtime=Longshot`nrepo=https://git.magicbean.dev/MagicBeanDev/Longshot`ncommit=$commit`n"
        Write-File "$k\hookshot\TOOLS.txt" "release=v3.0.0-rc.3`nlock_sha256=b3`nsource_commit=$commit`nsource_sha256=aa`n"
        Write-File "$k\hookshot\patches\0001-a.patch" "one`n"
        Write-File "$k\Start Squadala.bat" '@echo off'
        New-Item -ItemType Directory -Force -Path "$k\hookshot\tools" | Out-Null
        New-Item -ItemType Junction -Path "$k\hookshot\tools\python" -Value $pyHome | Out-Null
        # stand-ins: the pack tool writes a manifest (or fails, PACK_FAIL), dumpcheck takes a file that says GAME
        Write-File "$k\tools\puppet_pack\build.py" "import os, sys`nif os.environ.get('PACK_FAIL'): print('BuildError: Tex/x.bntx.zs: broken'); sys.exit(1)`nd = sys.argv[sys.argv.index('--mod-dir') + 1]`nos.makedirs(d, exist_ok=True)`nopen(os.path.join(d, 'coop_puppet.json'), 'w').write('{}')`nopen(os.path.join(d, 'from.txt'), 'w').write(sys.argv[sys.argv.index('--romfs') + 1])`nopen(os.path.join(d, 'args.txt'), 'w').write(' '.join(sys.argv[1:]))`n"
        Write-File "$k\tools\puppet_pack\dumpcheck.py" "import sys`np = sys.argv[sys.argv.index('--romfs') + 1]`nsys.exit(0 if open(p, 'rb').read(5) == b'GAME' else 1)   # (5 bytes: a real .nup in Downloads is not read through)`n"
        $run = {
            $ErrorActionPreference = 'Continue'
            $o = (& powershell -NoProfile -ExecutionPolicy Bypass -File "$k\scripts\hookshot-build.ps1" -PackOnly 2>&1) -join '|'
            return @($LASTEXITCODE, $o)
        }
        $r = & $run
        Check ($r[0] -ne 0 -and $r[1] -match 'not built yet.*Start Squadala\.bat') "pack only: no game yet: said, nothing made [$($r[1])]"
        Write-File "$k\hookshot\game\longshot.exe" 'exe'
        Write-File "$k\hookshot\game-files.txt" "$pk\gone\OoT.nup`n"
        Write-File "$pk\OoT.nup" 'GAME'   # moved beside the kit
        Write-File "$pk\manual.nup" 'NOT'
        $r = & $run
        $man = "$k\hookshot\game\mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json"
        Check ($r[0] -eq 0 -and $r[1] -match 'Found your game file: .*OoT\.nup' -and $r[1] -match 'Making the puppet pack' -and (Test-Path -LiteralPath $man) -and (Read-File (Join-Path (Split-Path -Parent $man) 'from.txt')) -eq "$pk\OoT.nup") "pack only: the .nup moved beside the kit: found, the pack made [$($r[0]) $($r[1])]"
        $packArgs = Read-File (Join-Path (Split-Path -Parent $man) 'args.txt')
        Check ($packArgs -match '--title-rows' -and $packArgs -match '--pause-tab') "pack: the kit's pack command (Get-KitPackArgs) has --title-rows and --pause-tab [$packArgs]"
        Check ((Read-File "$k\hookshot\game-files.txt").Trim() -eq "$pk\OoT.nup") 'pack only: the new place is remembered'
        $env:PACK_FAIL = '1'
        try { $r = & $run } finally { Remove-Item env:PACK_FAIL }
        Check ($r[0] -ne 0 -and $r[1] -match 'BuildError' -and $r[1] -match 'could not be made \(the lines above say why\)') "pack only: a pack tool error: said plainly, exit 1 [$($r[1])]"
    } finally {
        if (Test-Path -LiteralPath "$pk\kit\hookshot\tools\python") { [System.IO.Directory]::Delete("$pk\kit\hookshot\tools\python") }   # (the junction, not the Python behind it)
        Remove-Item -LiteralPath $pk -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Write-Host 'skip pack only cases (no Python: set TEST_PYTHON)'
}

# kit-friction: what Windows itself may block (windows-checks.ps1, run as "Start Squadala.bat" runs it: its text through
# powershell -Command), with stand-ins for Smart App Control, the language mode, the policy, the Code Integrity log and
# controlled folder access. And a Longshot of the player's own: its saves, its downloads, its version.
$wk = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-wc-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$wcVars = 'OOT_COOP_WC_SAC', 'OOT_COOP_WC_MODE', 'OOT_COOP_WC_POLICY', 'OOT_COOP_WC_CI', 'OOT_COOP_WC_MP', 'OOT_COOP_WC_CFA', 'OOT_COOP_WC_WRITE'
function Invoke-Wc([string]$Kit, [hashtable]$Vars = @{}, [switch]$Constrained) {
    foreach ($v in $wcVars) { Remove-Item "env:$v" -ErrorAction SilentlyContinue }
    foreach ($n in $Vars.Keys) { Set-Item "env:$n" $Vars[$n] }
    $pre = if ($Constrained) { "`$ExecutionContext.SessionState.LanguageMode = 'ConstrainedLanguage'; " } else { '' }
    $cmd = "$pre`$k='$Kit\'; iex (Get-Content -Raw -LiteralPath (Join-Path `$k 'scripts\windows-checks.ps1')); exit 0"
    $ErrorActionPreference = 'Continue'
    try { $o = (& powershell -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1) -join '|'; $c = $LASTEXITCODE }
    finally { $ErrorActionPreference = 'Stop'; foreach ($v in $wcVars) { Remove-Item "env:$v" -ErrorAction SilentlyContinue } }
    return @($c, $o)
}
try {
    $wkit = Join-Path $wk 'OoT Co-op'
    Copy-KitFile (Join-Path $kitSrc 'scripts\windows-checks.ps1') "$wkit\scripts\windows-checks.ps1"
    Write-File "$wkit\Start Squadala.bat" '@echo off'
    Write-File "$wkit\scripts\start.ps1" '#'
    Write-File "$wkit\hookshot\game\bin\big.dll" 'built here'
    foreach ($f in "$wkit\Start Squadala.bat", "$wkit\scripts\start.ps1", "$wkit\hookshot\game\bin\big.dll") {
        Set-Content -LiteralPath $f -Stream 'Zone.Identifier' -Value "[ZoneTransfer]`r`nZoneId=3"
    }
    $marked = { param($f) [bool](Get-Item -LiteralPath $f -Stream 'Zone.Identifier' -ErrorAction SilentlyContinue) }
    $r = Invoke-Wc $wkit
    Check ($r[0] -eq 0 -and $r[1] -match "Took Windows' 'downloaded from the internet' mark off" -and -not (& $marked "$wkit\scripts\start.ps1") -and
        -not (& $marked "$wkit\Start Squadala.bat")) "windows checks: the kit's own files lose the internet mark, the start goes on [$($r[1])]"
    Check (& $marked "$wkit\hookshot\game\bin\big.dll") 'windows checks: the built game''s files are not walked'
    $log = Read-File "$wkit\logs\windows-checks.txt"
    Check ($log -match 'internet mark: 2 file' -and $log -match 'language mode: FullLanguage' -and $log -match 'smart app control: off') "windows checks: logs\windows-checks.txt says what was found [$log]"
    $r = Invoke-Wc $wkit
    Check ($r[0] -eq 0 -and $r[1] -notmatch 'mark') "windows checks: unmarked: nothing said [$($r[1])]"
    $r = Invoke-Wc $wkit -Constrained -Vars @{ OOT_COOP_WC_SAC = 'on' }
    Check ($r[0] -eq 1 -and $r[1] -match 'Smart App Control is blocking this kit''s PowerShell scripts' -and $r[1] -match 'Smart App Control settings > Off' -and
        $r[1] -match 'cannot turn Smart App Control back on' -and $r[1] -notmatch 'Exception|error') "windows checks: real Constrained Language mode with Smart App Control on: stops and says why [$($r[1])]"
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_MODE = 'ConstrainedLanguage' }
    Check ($r[0] -eq 1 -and $r[1] -match 'AppLocker or WDAC') "windows checks: locked without Smart App Control: a policy of the PC [$($r[1])]"
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_POLICY = 'AllSigned' }
    Check ($r[0] -eq 1 -and $r[1] -match 'Group Policy .* AllSigned') "windows checks: a Group Policy AllSigned stops the start [$($r[1])]"
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_POLICY = 'RemoteSigned' }
    Check ($r[0] -eq 0) "windows checks: RemoteSigned by policy goes on (the kit's files are unmarked) [$($r[1])]"
    $dev = $wkit.Substring(2)
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_SAC = 'on'; OOT_COOP_WC_CI = "Code Integrity determined that a process (\Device\HarddiskVolume3$dev\hookshot\game\bin\longshot.exe) attempted to load \Device\HarddiskVolume3$dev\hookshot\game\bin\hookshot_coop.dll that did not meet the requirements.|Another (\Device\HarddiskVolume3\Windows\x.dll)" }
    Check ($r[0] -eq 0 -and $r[1] -match 'Smart App Control blocked hookshot\\game\\bin\\hookshot_coop\.dll of this kit\.' -and (Read-File "$wkit\logs\windows-checks.txt") -match 'blocked by code integrity: hookshot\\game\\bin\\hookshot_coop\.dll') "windows checks: the file Smart App Control blocked is named, from the Code Integrity log [$($r[1])]"
    # Defender took a file of the kit's game (Velvet 2026-11-19: tools\ootrr\ootrr.exe gone on every start): named
    # kit-relative in the log and said, with how to allow it; another folder's detection is not this kit's.
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_MP = "file:_\\?\$wkit\hookshot\game\tools\ootrr\ootrr.exe|file:_C:\Elsewhere\x.exe" }   # (\\?\: review-0116-j N2)
    Check ($r[0] -eq 0 -and $r[1] -match 'Microsoft Defender took hookshot\\game\\tools\\ootrr\\ootrr\.exe of this kit' -and $r[1] -match 'Protection history' -and
           (Read-File "$wkit\logs\windows-checks.txt") -match 'defender detections in the kit: hookshot\\game\\tools\\ootrr\\ootrr\.exe' -and
           (Read-File "$wkit\logs\windows-checks.txt") -notmatch 'Elsewhere') "windows checks: a Defender detection in the kit is named and said [$($r[1])]"
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_SAC = 'on' }
    Check ($r[0] -eq 0 -and $r[1] -match 'Smart App Control is on\. It may block') "windows checks: Smart App Control on, nothing blocked yet: said, goes on [$($r[1])]"
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_WRITE = 'Access to the path is denied.'; OOT_COOP_WC_CFA = '1' }
    Check ($r[0] -eq 1 -and $r[1] -match 'refused to let PowerShell write in this kit''s folder \(access denied\)' -and $r[1] -match 'Allow an app through Controlled folder access' -and
        (Read-File "$wkit\logs\windows-checks.txt") -match 'write test in the kit folder: access denied' -and (Read-File "$wkit\logs\windows-checks.txt") -match 'controlled folder access: on') "windows checks: access denied with controlled folder access on: stops, says how to allow it [$($r[1])]"
    $r = Invoke-Wc $wkit -Vars @{ OOT_COOP_WC_WRITE = 'Access to the path is denied.'; OOT_COOP_WC_CFA = '0' }
    Check ($r[0] -eq 1 -and $r[1] -match 'Another antivirus') "windows checks: access denied without it: another antivirus or the folder [$($r[1])]"
    Check ((Read-File "$wkit\logs\windows-checks.txt") -match 'execution policies: .*MachinePolicy=') 'windows checks: every execution policy scope is logged'
    $keepProfile = $env:USERPROFILE
    $env:USERPROFILE = $wk
    Copy-KitFile (Join-Path $kitSrc 'scripts\windows-checks.ps1') "$wk\Documents\kit\scripts\windows-checks.ps1"
    try { $r = Invoke-Wc "$wk\Documents\kit" -Vars @{ OOT_COOP_WC_CFA = '1' } } finally { $env:USERPROFILE = $keepProfile }
    Check ($r[0] -eq 0 -and $r[1] -match 'Controlled folder access .* protects .*Documents') "windows checks: a kit under Documents with controlled folder access on is told to move [$($r[1])]"
    Check ((Read-File "$wk\Documents\kit\logs\windows-checks.txt") -match 'controlled folder access over the kit: Documents' -and (Read-File "$wk\Documents\kit\logs\windows-checks.txt") -notmatch [regex]::Escape($wk)) 'windows checks: the log names the folder, not the user''s path'

    # A Longshot of the player's own (Get-KitLongshot*, Copy-KitLongshot*)
    $ls = Join-Path $wk 'Longshot\v3.0.0-rc.3'
    $lsHome = Join-Path $wk 'LsHome'
    Write-File "$ls\longshot.exe" 'launcher'
    Write-File "$ls\bin\image.bin" 'image'
    Write-File "$ls\longshot_version.txt" "version=v3.0.0-rc.3`nbuilder=$lsHome\versions\v3.0.0-rc.3\LongshotBuilder\LongshotBuilder.exe`n"
    $acct = '5a3c9e1f0b7d24680000000000000001'
    Write-File "$ls\bin\user\save\$acct\slot_00\progress.sav" 'MINE0'
    Write-File "$ls\bin\user\save\$acct\slot_01\progress.sav" 'MINE1'
    Write-File "$ls\bin\user\save\$acct\account.sav" 'ACCT'
    Write-File "$ls\bin\user\save\$acct-coop\slot_00\progress.sav" 'GUEST'
    Write-File "$lsHome\versions\current.txt" "v3.0.0-rc.3`n"
    Write-File "$lsHome\downloads\$('ab' * 32).zip" 'ZIP'
    Check ((Get-KitLongshotImageDir $ls) -eq "$ls\bin") 'longshot: a builder''s game folder: its image is in bin\'
    Check ((Get-KitLongshotImageDir $wk) -eq '') 'longshot: no image.bin: no Longshot game'
    $info = Get-KitLongshotGameInfo $ls
    Check ($info.version -eq 'v3.0.0-rc.3' -and $info.home -eq $lsHome) "longshot: version and builder home from longshot_version.txt [$($info.version) $($info.home)]"
    $homes = @(Get-KitLongshotHomes $info.home "$wk\no-appdata")
    Check ($homes.Count -eq 1 -and $homes[0].version -eq 'v3.0.0-rc.3') 'longshot: the home and its version'
    $zipTo = "$wk\kitdl\llvm-mingw-abababababab.zip"
    Check ((Copy-KitLongshotDownload $homes ('AB' * 32) $zipTo) -eq $lsHome -and (Read-File $zipTo) -eq 'ZIP') 'longshot: a download with the same SHA-256 is copied'
    Check ((Copy-KitLongshotDownload $homes ('cd' * 32) "$wk\kitdl\other.zip") -eq '' -and -not (Test-Path "$wk\kitdl\other.zip")) 'longshot: no zip with that SHA-256: nothing copied'
    $ours = "$wk\coop-game\bin"
    Write-File "$ours\user\save\$acct\slot_01\progress.sav" 'COOP1'
    $said = (Copy-KitLongshotSaves "$ls\bin" $ours) -join '|'
    Check ((Read-File "$ours\user\save\$acct\slot_00\progress.sav") -eq 'MINE0' -and (Read-File "$ours\user\save\$acct\account.sav") -eq 'ACCT') "longshot saves: a slot copied with account.sav [$said]"
    Check ((Read-File "$ours\user\save\$acct\slot_01\progress.sav") -eq 'COOP1' -and $said -match 'left as they are: slot_01') "longshot saves: a slot the co-op game has is never overwritten [$said]"
    Check (-not (Test-Path "$ours\user\save\$acct-coop") -and (Read-File "$ls\bin\user\save\$acct\slot_01\progress.sav") -eq 'MINE1') 'longshot saves: guest folders stay behind, the player''s own saves unchanged'
    $need = Get-KitHookshotNeedText 'game' '' $true
    Check ($need -match 'never used as it is' -and $need -match 'moving one into this folder does nothing' -and $need -match '60 GB' -and $need -match 'Longshot') "need text: says a self-built Longshot is not used, and where the 60 GB goes [$need]"
} finally {
    foreach ($v in $wcVars) { Remove-Item "env:$v" -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $wk -Recurse -Force -ErrorAction SilentlyContinue
}

# kit-mirror (0.11.1): Get-KitDownload tries each URL several times, then curl.exe, then the co-op mirror, and never keeps a
# file whose SHA-256 differs. Invoke-KitFetch is replaced in a child scope by a fake that logs each try and fails as told.
$dl = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-dl-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $dl | Out-Null
try {
    $good = Join-Path $dl 'good.bin'; Write-File $good 'the pinned file'
    $goodSha = (Get-FileHash -LiteralPath $good -Algorithm SHA256).Hash.ToLowerInvariant()
    $feed = 'file:///' + ($dl -replace '\\', '/')
    & {
        $script:tries = @()
        # fail: host -> 'all' (always), 'web' (Invoke-WebRequest only), 'bad' (a wrong file)
        $script:fail = @{}
        function Invoke-KitFetch([string]$Url, [string]$Out, [string]$Method) {
            $h = Get-KitUrlHost $Url
            $script:tries += "$h/$Method"
            $how = $script:fail[$h]
            if ($how -eq 'all' -or ($how -eq 'web' -and $Method -eq 'web')) { throw 'The underlying connection was closed: An unexpected error occurred on a send.' }
            if ($how -eq 'gone') { throw 'curl.exe: curl: (22) The requested URL returned error: 404 (exit 22)' }
            if ($how -eq 'bad') { Write-File $Out 'something else'; return }
            Copy-Item -LiteralPath $good -Destination $Out
        }
        function Reset-Dl { $script:tries = @(); $script:fail = @{}; $script:KitBadHosts = @{}; Remove-Item -LiteralPath "$dl\out.bin" -Force -ErrorAction SilentlyContinue }

        Reset-Dl
        $r = Get-KitDownload @('https://a.example/f') "$dl\out.bin" $goodSha @() 4 0
        Check ($r -eq '' -and ($script:tries -join ',') -eq 'a.example/web' -and (Test-Path "$dl\out.bin")) "download: a working host, one try [$r $($script:tries -join ',')]"
        $r = Get-KitDownload @('https://a.example/f') "$dl\out.bin" $goodSha.ToUpperInvariant() @() 4 0
        Check ($r -eq '' -and $script:tries.Count -eq 1) 'download: a file already there with the right SHA-256 is not fetched again'

        Reset-Dl; $script:fail['a.example'] = 'web'
        $r = Get-KitDownload @('https://a.example/f') "$dl\out.bin" $goodSha @() 4 0
        Check ($r -eq '' -and ($script:tries -join ',') -eq 'a.example/web,a.example/web,a.example/web,a.example/web,a.example/curl') "download: 4 tries, then curl.exe [$($script:tries -join ',')]"

        Reset-Dl; $script:fail['a.example'] = 'all'; $script:fail['b.example'] = 'all'
        $r = Get-KitDownload @('https://a.example/f', 'https://b.example/f') "$dl\out.bin" $goodSha @('https://mirror.example/f') 2 0
        Check ($r -eq '' -and ($script:tries -join ',') -eq 'a.example/web,a.example/web,a.example/curl,b.example/web,b.example/web,b.example/curl,mirror.example/web') "download: every URL, then the mirror [$($script:tries -join ',')]"
        $before = $script:tries.Count
        Remove-Item -LiteralPath "$dl\out.bin" -Force
        $r = Get-KitDownload @('https://a.example/g') "$dl\out.bin" $goodSha @('https://mirror.example/g') 2 0
        Check ($r -eq '' -and ($script:tries[$before..($script:tries.Count - 1)] -join ',') -eq 'a.example/web,mirror.example/web') "download: a host that failed every try gets one try next time [$($script:tries -join ',')]"

        Reset-Dl; $script:fail['a.example'] = 'bad'
        $r = Get-KitDownload @('https://a.example/f') "$dl\out.bin" $goodSha @('https://mirror.example/f') 4 0
        Check ($r -eq '' -and (Read-File "$dl\out.bin") -eq 'the pinned file' -and ($script:tries -join ',') -eq 'a.example/web,mirror.example/web') "download: a wrong file is thrown away and the mirror's taken [$($script:tries -join ',')]"
        Reset-Dl; $script:fail['a.example'] = 'bad'; $script:fail['mirror.example'] = 'bad'
        $r = Get-KitDownload @('https://a.example/f') "$dl\out.bin" $goodSha @('https://mirror.example/f') 4 0
        Check ($r -match 'a\.example: .*SHA-256 differs' -and $r -match 'mirror\.example: .*SHA-256 differs' -and -not (Test-Path "$dl\out.bin") -and -not (Test-Path "$dl\out.bin.part")) "download: wrong files everywhere: nothing kept, each host named [$r]"

        Reset-Dl; $script:fail['a.example'] = 'gone'
        $r = Get-KitDownload @('https://a.example/f') "$dl\out.bin" $goodSha @('https://mirror.example/f') 4 0
        Check ($r -eq '' -and ($script:tries -join ',') -eq 'a.example/web,mirror.example/web' -and -not $script:KitBadHosts['a.example']) "download: a 404 is not asked again, and its host is not marked bad [$($script:tries -join ',')]"
        Check (-not (Test-KitGoneError ([Exception]::new('curl.exe: curl: (35) schannel: failed to receive handshake (exit 35)')))) 'download: a TLS error is no 404'

        Reset-Dl; $script:fail['a.example'] = 'all'; $script:fail['mirror.example'] = 'all'
        $r = Get-KitDownload @('https://a.example/f') "$dl\out.bin" $goodSha @('https://mirror.example/f') 2 0
        Check ($r -match '^a\.example: The underlying connection' -and $r -match '; mirror\.example: ') "download: everything failed: each host and why [$r]"
        $text = Get-KitDownloadFailText 'tools.lock' $r 'https://git.magicbean.dev/x/tools.lock' $true
        Check ($text -match 'co-op mirror on GitHub' -and $text -match 'antivirus' -and $text -match 'VPN or proxy' -and $text -match 'https://git\.magicbean\.dev/x/tools\.lock' -and $text -match 'a\.example') "download: the failure text names the hosts, the mirror, the usual causes and a browser test [$text]"

        # (review-kit-mirror 4) the mirror shares github.com with upstream URLs: a "bad" github.com still gets the mirror's full round
        Reset-Dl; $script:fail['gh.example'] = 'all'
        $r = Get-KitDownload @('https://gh.example/upstream') "$dl\out.bin" $goodSha @('https://gh.example/mirror') 2 0
        Check (($script:tries -join ',') -eq 'gh.example/web,gh.example/web,gh.example/curl,gh.example/web,gh.example/web,gh.example/curl') "download: the mirror gets every try even on a host that failed [$($script:tries -join ',')]"

        Reset-Dl
        Check ((Get-KitDownload @('https://a.example/f') "$dl\out.bin" '' @() 4 0) -match 'no SHA-256' -and $script:tries.Count -eq 0) 'download: nothing is fetched without a SHA-256 to check it against'
    }
    # the real fetchers on file:// (Invoke-WebRequest and curl.exe), and a test-blocked host falling back to the mirror
    $script:KitBadHosts = @{}
    Invoke-KitFetch "$feed/good.bin" "$dl\web.bin" 'web'; Invoke-KitFetch "$feed/good.bin" "$dl\curl.bin" 'curl'
    Check ((Read-File "$dl\web.bin") -eq 'the pinned file' -and (Read-File "$dl\curl.bin") -eq 'the pinned file') 'download: Invoke-WebRequest and curl.exe both fetch'
    # (review-kit-mirror 2) Invoke-WebRequest -OutFile reads [ ] as wildcards: a kit folder named so still gets its file
    $br = Join-Path $dl 'kit [copy] (1)'; New-Item -ItemType Directory -Force -Path $br | Out-Null
    Invoke-KitFetch "$feed/good.bin" (Join-Path $br 'web.bin') 'web'; Invoke-KitFetch "$feed/good.bin" (Join-Path $br 'curl.bin') 'curl'
    Check ((Read-File (Join-Path $br 'web.bin')) -eq 'the pinned file' -and (Read-File (Join-Path $br 'curl.bin')) -eq 'the pinned file') 'download: a folder with [ ] in its name: both fetchers'
    $threw = ''; try { Invoke-KitFetch "$feed/missing.bin" "$dl\m.bin" 'curl' } catch { $threw = $_.Exception.Message }
    Check ($threw -match '^curl\.exe: ') "download: a failed curl.exe throws its own reason [$threw]"
    $env:OOT_COOP_TEST_BLOCK_HOST = 'magicbean\.dev$'
    try { $r = Get-KitDownload @('https://git.magicbean.dev/MagicBeanDev/Longshot/releases/download/x/good.bin') "$dl\blocked.bin" $goodSha @("$feed/good.bin") 2 0 }
    finally { Remove-Item env:OOT_COOP_TEST_BLOCK_HOST -ErrorAction SilentlyContinue }
    Check ($r -eq '' -and (Read-File "$dl\blocked.bin") -eq 'the pinned file') "download: git.magicbean.dev blocked: the file comes from the mirror [$r]"
    $keepTls = [Net.ServicePointManager]::SecurityProtocol
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls11
    Set-KitTls
    $now = [Net.ServicePointManager]::SecurityProtocol
    Check (($now -band [Net.SecurityProtocolType]::Tls12) -and ($now -band [Net.SecurityProtocolType]::Tls11)) "download: Set-KitTls adds Tls12 to what is enabled, takes nothing away [$now]"
    [Net.ServicePointManager]::SecurityProtocol = $keepTls
} finally {
    Remove-Item -LiteralPath $dl -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
