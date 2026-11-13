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
    Write-File $man '{"tool": "tools/puppet_pack", "actors": []}'
    Check ((Get-PuppetPackHint $man $tool) -match 'without the other players'' sounds.*make-puppet-pack\.cmd') 'a pack without "sound": run make-puppet-pack again'
    Write-File $man '{"actors": [], "sound": false}'
    Check ((Get-PuppetPackHint $man $tool) -match 'sounds') 'a --no-sound pack: the same line'
    Write-File $man "{`"actors`": [], `"sound`": true, `"tunics`": {`"Red`": [`"x`"]}, `"tool_sha256`": `"$hash`"}"
    (Get-Item (Join-Path $tool 'b_c.py')).LastWriteTimeUtc = [datetime]::UtcNow.AddHours(10)
    Check ($null -eq (Get-PuppetPackHint $man $tool)) 'the same tool, its files dated hours ahead (a zip from another time zone): nothing said'
    Check ($null -eq (Get-PuppetPackHint $man (Join-Path $ph 'no-tool'))) 'no tool folder: nothing said'
    Write-File $man '{"actors": [], "sound": true, "tunics": {"Red": ["x"]}}'
    Check ($null -eq (Get-PuppetPackHint $man $tool)) 'sound and no tool hash: nothing said'
    Write-File $man '{"actors": [], "sound": true}'
    Check ((Get-PuppetPackHint $man $tool) -match 'without the tunic colours.*green.*make-puppet-pack\.cmd') 'a pack from before the tunics: run make-puppet-pack again (#202)'
    Write-File $man '{"actors": [], "sound": true, "tunics": {}, "tunics_off": true}'
    Check ($null -eq (Get-PuppetPackHint $man $tool)) 'a --tunics none pack: no tunic line'
    Write-File $man '{"actors": [], "sound": true, "tunics": {}, "tool_sha256": "0000"}'
    Check ((Get-PuppetPackHint $man $tool) -match 'without the tunic colours') 'a pack with no tunics made: the tunic line first'
    Write-File $man '{"actors": [], "sound": true, "tunics": {"Red": ["x"]}, "tool_sha256": "0000"}'
    Check ((Get-PuppetPackHint $man $tool) -match 'another version of the pack tool') 'another tool hash: run make-puppet-pack again'
    Write-File $man '{ not json'
    Check ((Get-PuppetPackHint $man $tool) -match 'could not be read') 'a broken coop_puppet.json: a line, no throw'
    Check ((Get-PuppetPackHint (Join-Path $ph 'none.json') $tool) -match 'could not be read') 'a missing coop_puppet.json: a line, no throw'
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
    [System.IO.File]::WriteAllBytes((Join-Path $hsGame 'hookshot.exe'), [System.IO.File]::ReadAllBytes((Join-Path $env:SystemRoot 'System32\rundll32.exe')))
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
    Check ($out -match 'Puppet pack: found\.' -and $out -match 'Your Link: the red tunic' -and $out -match 'Hookshot started') "play.ps1 -Hookshot: the pack found and your red tunic said [$out]"
    Check ([System.IO.File]::ReadAllText((Join-Path $hsPack 'romfs\Tex\Angin_Tile_A_Alb.bntx.zs')) -eq 'RED') 'play.ps1 -Hookshot: the red tile in the Hookshot pack before the game starts'
    Remove-Item -LiteralPath $hsPack -Recurse -Force
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" join -Server 127.0.0.1 -Name Saria -Hookshot -NoUpdate < `"$enter`"" 2>&1) -join '|'
    Check ($out -match 'Puppet pack: not in the Hookshot game' -and $out -match 'Hookshot started') "play.ps1 -Hookshot: no pack: says so, still starts [$out]"
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
    Check ($out -match 'Hookshot started' -and $out -match 'Host or Join on the title screen' -and $out -notmatch 'H or J' -and $out -notmatch 'Invite code') "Play-OoT-Coop: starts the game with no question [$out]"
    Check ($idle -notmatch '(?m)^\s*server\s*=' -and $idle -notmatch '(?m)^\s*password\s*=') "Play-OoT-Coop: no server and no join password left in coop.ini after a host run [$idle]"
    Check ($idle -match '(?m)^name = Saria\r?$' -and $idle -match '(?m)^last_invite = ABCDE-FGHJK\r?$' -and $idle -match '(?m)^port = 27510\r?$') 'Play-OoT-Coop: the name, the port and the last invite code stay'
    Check (-not (Test-Path "$kit\server-args.txt") -and -not (Get-Process coop_server -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($kit, [StringComparison]::OrdinalIgnoreCase) })) 'Play-OoT-Coop: no server is started'
    & $stopHs
    Remove-Item -LiteralPath (Join-Path $hsGame 'coop.ini') -Force
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$kit\scripts\play.ps1`" choose -Hookshot -NoUpdate < nul" 2>&1) -join '|'
    $idle = Read-File (Join-Path $hsGame 'coop.ini')
    Check ($out -match 'Hookshot started' -and $idle -notmatch '(?m)^\s*server\s*=') "Play-OoT-Coop: a first start (no coop.ini) writes one with no server [$out]"
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
    [System.IO.File]::WriteAllBytes((Join-Path $hsGame 'hookshot.exe'), [System.IO.File]::ReadAllBytes((Join-Path $env:SystemRoot 'System32\rundll32.exe')))
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
            Check ($out -match 'joining that session again as the host' -and $out -match '(Emulator|Hookshot) started') "${bat}, the kit's server running: says it joins the running session again, and the game starts [$out]"
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
        Check ($out -match 'Close that window, then start Host-Hookshot\.bat again' -and $out -notmatch 'Hookshot started' -and -not (Test-Path "$kit\server-args.txt")) "Host-Hookshot.bat, no saved session: refused, names Host-Hookshot.bat [$out]"
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
    Write-File (Join-Path $hk 'hookshot\PIN') "repo=https://github.com/NotRexed/Hookshot`ncommit=4aee23659b9436dab2cd7be1ac07333c06246b7c`n"
    Write-File (Join-Path $hk 'hookshot\TOOLS.txt') "release=v2.2.0-rc.1`nsetup_sha256=b3`n"
    Write-File (Join-Path $hk 'hookshot\patches\0001-a.patch') "one`n"
    Write-File (Join-Path $hk 'hookshot\hookshot_coop.dll') 'MODULE 1'
    Check ((Get-KitHookshotNeed $hk) -eq 'game') 'hookshot need: no game built yet -> game'
    $g = Join-Path $hk 'hookshot\game'
    Write-File (Join-Path $g 'hookshot.exe') 'exe'
    Write-File (Join-Path $g 'coop-built-from.txt') (Get-KitHookshotStamp $hk)
    Write-File (Join-Path $g 'hookshot_coop.dll') 'MODULE 1'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: no puppet pack -> module (the pack is made)'
    $pack = Join-Path $g 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json'
    Write-File $pack '{"sound": true, "tunics": {"red": ["x"]}}'
    Check ((Get-KitHookshotNeed $hk) -eq '') 'hookshot need: the game as the kit wants it -> nothing'
    Write-File (Join-Path $hk 'hookshot\hookshot_coop.dll') 'MODULE 2'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: a new module -> module (seconds)'
    Write-File (Join-Path $g 'hookshot_coop.dll') 'MODULE 2'
    Write-File (Join-Path $hk 'hookshot\patches\0002-b.patch') "two`n"
    Check ((Get-KitHookshotNeed $hk) -eq 'patches') 'hookshot need: a patch changed, the same pin -> patches'
    Write-File (Join-Path $g 'coop-built-from.txt') (Get-KitHookshotStamp $hk)
    Write-File (Join-Path $hk 'hookshot\PIN') "repo=x`ncommit=0000000000000000000000000000000000000000`n"
    Check ((Get-KitHookshotNeed $hk) -eq 'pin') 'hookshot need: another pin -> pin (a full build)'
    Write-File (Join-Path $hk 'hookshot\PIN') "repo=x`ncommit=4aee23659b9436dab2cd7be1ac07333c06246b7c`n"
    Write-File $pack '{"sound": false}'
    Check ((Get-KitHookshotNeed $hk) -eq 'module') 'hookshot need: a pack the kit would make again -> module'
    Write-File $pack '{"sound": true, "tunics": {"red": ["x"]}}'
    Check ((Get-KitHookshotNeed $hk) -eq '') 'hookshot need: up to date again'
    Check ((Test-KitPuppetPackStale (Join-Path $hk 'none\coop_puppet.json') (Join-Path $hk 'no-tool')) -and
        -not (Test-KitPuppetPackStale $pack (Join-Path $hk 'no-tool'))) 'pack stale: a missing pack yes, a current one no'
    # the game somewhere else (hs.py, -Out): game-folder.txt, and a builder's bin\ layout
    $other = Join-Path $hk 'elsewhere'
    Write-File (Join-Path $hk 'hookshot\game-folder.txt') "$other`n"
    Check ((Get-KitHookshotGame $hk) -eq $other -and (Get-KitHookshotNeed $hk) -eq 'moved') 'hookshot need: game-folder.txt is followed; its game gone -> moved'
    Write-File (Join-Path $other 'bin\hookshot.exe') 'exe'
    Check ((Get-KitHookshotExe $other) -eq (Join-Path $other 'bin\hookshot.exe')) 'hookshot exe: bin\ first'
    # hs-missing (a tester read "No co-op Hookshot game" as "couldn't find Hookshot"): antivirus took the module
    Remove-Item -LiteralPath (Join-Path $hk 'hookshot\hookshot_coop.dll')
    Check ((Get-KitHookshotNeed $hk) -eq 'quarantine') 'hookshot need: the module in neither the kit nor the game -> quarantine (no build)'
    Write-File (Join-Path $other 'bin\hookshot_coop.dll') 'MODULE 2'
    Check ((Get-KitHookshotNeed $hk) -ne 'quarantine') 'hookshot need: the kit''s module gone, the game''s there -> the game still starts'
    Remove-Item -LiteralPath (Join-Path $other 'bin\hookshot_coop.dll')
    Write-File (Join-Path $hk 'hookshot\hookshot_coop.dll') 'MODULE 2'
    foreach ($n in 'game', 'moved', 'pin', 'patches', 'module', 'quarantine') { Check ([bool](Get-KitHookshotNeedText $n)) "hookshot need text: $n" }
    foreach ($n in 'game', 'moved', 'pin') { Check ([bool](Get-KitHookshotNeedQuestion $n)) "hookshot need question: $n asks first" }
    foreach ($n in 'patches', 'module', 'quarantine', '') { Check (-not (Get-KitHookshotNeedQuestion $n)) "hookshot need question: none for '$n'" }
    Check ((Get-KitHookshotNeedText 'game') -match "isn't set up yet" -and (Get-KitHookshotNeedText 'game') -notmatch 'No co-op') 'hookshot need text: game says it is not set up yet, in plain words'
    Check ((Get-KitHookshotNeedText 'moved' 'D:\Old\game') -match 'D:\\Old\\game' -and (Get-KitHookshotNeedText 'moved') -match '-Out') 'hookshot need text: moved names the folder and -Out'
    Check ((Get-KitHookshotNeedText 'quarantine') -match 'Antivirus' -and (Get-KitHookshotNeedText 'quarantine') -match 'Exclusions') 'hookshot need text: quarantine says antivirus and the exclusion steps'
    Check ((Get-KitHookshotNeedText 'pin') -match 'about 15 minutes') 'hookshot need text: a pin move says about 15 minutes'
    Check (-not (Get-KitHookshotNeedText '')) 'hookshot need text: nothing to say when nothing is needed'

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
    Check ($out -match "Hookshot isn't set up yet" -and $out -match 'Set it up now\? \(then the game starts\) \[Y/n\]' -and $out -match 'Not now\. Build-Hookshot-Coop\.bat' -and $out -notmatch 'Hookshot started' -and -not (& $built)) "Join-Hookshot, no game: says not set up yet, offers the build, n builds nothing [$out]"
    $out = & $runHs 'host'
    Check ($out -match "Hookshot isn't set up yet" -and $out -match 'Set it up now\?' -and $out -notmatch 'Hookshot started') "Host-Hookshot, no game: the same offer [$out]"
    $out = (cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$hm\scripts\play.ps1`" choose -Hookshot -NoUpdate < `"$no`"" 2>&1) -join '|'
    Check ($out -match "Hookshot isn't set up yet" -and $out -match 'Set it up now\?' -and $out -notmatch 'Hookshot started') "Play-OoT-Coop, no game: the same offer [$out]"
    # the built game moved or deleted: names where it was and -Out
    $gone = Join-Path $hm 'moved away\game'
    Write-File (Join-Path $hm 'hookshot\game-folder.txt') "$gone`n"
    $out = & $runHs 'join'
    Check ($out -match 'not in .*moved away.game any more \(moved or deleted\)' -and $out -match 'Build-Hookshot-Coop\.bat -Out' -and $out -match 'Build it again now\?' -and -not (& $built)) "Join-Hookshot, the game moved: says so, -Out, offers a new build [$out]"
    # built for an older pin: the rebuild offer
    $g = Join-Path $hm 'hookshot\game'
    Write-File (Join-Path $hm 'hookshot\game-folder.txt') "$g`n"
    Write-File (Join-Path $g 'hookshot.exe') 'exe'
    Write-File (Join-Path $g 'hookshot_coop.dll') 'MODULE'
    Write-File (Join-Path $g 'coop-built-from.txt') "release=v2.1.0`ncommit=0000000000000000000000000000000000000000`npatches="
    $out = & $runHs 'join'
    Check ($out -match 'built for another Hookshot version' -and $out -match 'Update it now\?' -and $out -notmatch 'Hookshot started' -and -not (& $built)) "Join-Hookshot, an older pin: the rebuild offer [$out]"
    # the module taken (antivirus): no build offered, the restore and exclusion steps
    Remove-Item -LiteralPath (Join-Path $g 'hookshot_coop.dll'), (Join-Path $hm 'hookshot\hookshot_coop.dll')
    $out = & $runHs 'join'
    Check ($out -match 'hookshot_coop\.dll is missing' -and $out -match 'Antivirus' -and $out -match 'Protection history' -and $out -match 'Exclusions' -and $out -notmatch '\[Y/n\]' -and $out -notmatch 'Hookshot started' -and -not (& $built)) "Join-Hookshot, the module quarantined: antivirus steps, no build [$out]"
    # the game's copy gone after it was put in (a kit without a pin to build: the last check before the start)
    Remove-Item -LiteralPath (Join-Path $hm 'hookshot\PIN')
    Write-File (Join-Path $hm 'hookshot\hookshot_coop.dll') 'MODULE'
    $out = & $runHs 'join'
    Check ($out -match 'hookshot_coop\.dll is missing' -and $out -match 'Antivirus' -and $out -notmatch 'No co-op Hookshot game' -and $out -notmatch 'Hookshot started') "Join-Hookshot, the game's module gone: antivirus steps, not 'No co-op Hookshot game' [$out]"
} finally {
    Remove-Item -LiteralPath $hm -Recurse -Force -ErrorAction SilentlyContinue
}
# The new launcher is one command line too (the update may replace it while it runs).
$last = @(Get-Content (Join-Path $kitSrc 'Play-OoT-Coop.bat') | Where-Object { $_ -and $_ -notmatch '^(@echo off|rem )' })
Check ($last.Count -eq 1 -and $last[0] -match 'play\.ps1" choose -Hookshot' -and $last[0] -match '& exit /b$') 'Play-OoT-Coop.bat: play.ps1 choose -Hookshot, one line'

if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
