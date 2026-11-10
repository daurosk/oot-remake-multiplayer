# Tests for the kit's game-mod check (Get-KitMods in tools/release/kit/scripts/kit-paths.ps1,
# issue 136: co-op does not allow game mods).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\mods_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\kit-paths.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Write-File([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text)
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-mods-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    $user = Join-Path $tmp 'emulator\user'
    $game = Join-Path $user 'load\0400486024DEC000'
    New-Item -ItemType Directory -Force -Path $user | Out-Null
    Check (@(Get-KitMods $user).Count -eq 0) 'no load folder: no mods'

    # Co-op's own puppet pack is no mod for this check.
    Write-File (Join-Path $game 'coop-puppet\romfs\Pack\Actor\CoopPuppet_Child.pack.zs') 'x'
    Check (@(Get-KitMods $user).Count -eq 0) 'the coop-puppet pack does not count'
    # An empty folder, or one with only empty parts, is no mod (as in the emulator's Add-Ons).
    New-Item -ItemType Directory -Force -Path (Join-Path $game 'Empty\exefs') | Out-Null
    Check (@(Get-KitMods $user).Count -eq 0) 'an empty mod folder does not count'

    # The Hyrule Cheat Menu's layout: an IPS and a subsdk in exefs.
    Write-File (Join-Path $game 'Hyrule-Cheat-Menu\exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips') 'PATCH'
    Write-File (Join-Path $game 'Hyrule-Cheat-Menu\exefs\subsdk8') 'x'
    $mods = @(Get-KitMods $user)
    Check ($mods.Count -eq 1 -and $mods[0] -like '*Hyrule-Cheat-Menu') "an exefs mod is found: $($mods -join ', ')"
    # Any part counts, in any case: RomFS, cheats.
    Write-File (Join-Path $game 'Textures\RomFS\a.bin') 'x'
    Write-File (Join-Path $game 'Cheats only\cheats\0123456789ABCDEF.txt') '[x]'
    Check (@(Get-KitMods $user).Count -eq 3) 'romfs (any case) and cheats folders count too'

    # Turned off in the game's Add-Ons: not counted. Another game's list does not matter.
    $id = [Convert]::ToUInt64('0400486024DEC000', 16).ToString()
    $ini = "[UI]`r`nfoo=1`r`n[DisabledAddOns]`r`n1\disabled\1\d=Hyrule-Cheat-Menu`r`n1\disabled\1\d\default=false`r`n" +
           "1\disabled\2\d=Textures`r`n1\disabled\size=2`r`n1\title_id=$id`r`n1\title_id\default=false`r`n" +
           "2\disabled\1\d=Cheats only`r`n2\disabled\size=1`r`n2\title_id=12345`r`nsize=2`r`n[Other]`r`n3\disabled\1\d=Cheats only`r`n"
    Write-File (Join-Path $user 'config\qt-config.ini') $ini
    Check ((@(Get-KitDisabledAddons $user) -join ',') -eq 'Hyrule-Cheat-Menu,Textures') 'the Add-Ons list of this game is read'
    $mods = @(Get-KitMods $user)
    Check ($mods.Count -eq 1 -and $mods[0] -like '*Cheats only') "mods turned off in Add-Ons do not count: $($mods -join ', ')"

    # A load folder set elsewhere in qt-config.ini, and the SD card's atmosphere folder.
    $other = Join-Path $tmp 'elsewhere\load'
    Write-File (Join-Path $other '0400486024DEC000\Moved\exefs\main.ips') 'PATCH'
    $sd = Join-Path $tmp 'sd'
    Write-File (Join-Path $sd 'atmosphere\contents\0400486024DEC000\romfs\x.bin') 'x'
    $ini = "[Data%20Storage]`r`nload_directory=$($other.Replace('\', '/'))/`r`nsdmc_directory=$($sd.Replace('\', '/'))/`r`n"
    Write-File (Join-Path $user 'config\qt-config.ini') $ini
    $mods = @(Get-KitMods $user)
    Check ($mods.Count -eq 2) "the load folder from qt-config.ini and the SD card's mod both count: $($mods -join ', ')"
    Check (@($mods | Where-Object { $_ -like '*Moved' }).Count -eq 1) 'the mod in the moved load folder'
    Check (@($mods | Where-Object { $_ -like '*atmosphere*0400486024DEC000' }).Count -eq 1) 'the SD card mod (SDMC)'
    Write-File (Join-Path $user 'config\qt-config.ini') ($ini + "[DisabledAddOns]`r`n1\disabled\1\d=SDMC`r`n1\disabled\size=1`r`n1\title_id=$id`r`nsize=1`r`n")
    Check (@(Get-KitMods $user).Count -eq 1) 'the SD card mod turned off (SDMC) does not count'
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
