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

    # Dark's Dynamic FPS mod (Fritchii v5.7 skips it for its built-in, and hides it in Add-Ons).
    Remove-Item -LiteralPath (Join-Path $user 'config\qt-config.ini') -Force
    $u2 = Join-Path $tmp 'dfps\emulator\user'
    $g2 = Join-Path $u2 'load\0400486024DEC000'
    New-Item -ItemType Directory -Force -Path $u2 | Out-Null
    function New-Ips([string]$Path, [int64[]]$Offsets, [int]$Size, [switch]$Ips32) {
        $bytes = New-Object System.Collections.Generic.List[byte]
        $bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes($(if ($Ips32) { 'IPS32' } else { 'PATCH' })))
        foreach ($o in $Offsets) {
            $w = if ($Ips32) { 4 } else { 3 }
            for ($k = $w - 1; $k -ge 0; $k--) { $bytes.Add([byte](($o -shr (8 * $k)) -band 0xff)) }
            $bytes.Add([byte]($Size -shr 8)); $bytes.Add([byte]($Size -band 0xff))
            for ($k = 0; $k -lt $Size; $k++) { $bytes.Add(0x1f) }
        }
        $bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes($(if ($Ips32) { 'EEOF' } else { 'EOF' })))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
        [System.IO.File]::WriteAllBytes($Path, $bytes.ToArray())
    }
    $build = 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348'
    New-Ips (Join-Path $g2 'DynamicFPS\exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips') @(0x6e6dc4, 0x3c46300) 4 -Ips32
    Check (@(Get-KitMods $u2).Count -eq 0) 'Dark''s Dynamic FPS (IPS32 over its call and routine) is no mod: Fritchii uses its built-in'
    $ranges = (Get-KitIpsRanges (Join-Path $g2 'DynamicFPS\exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips'))
    Check ($ranges.Count -eq 2 -and $ranges[0][0] -eq 0x6e6dc4 -and $ranges[1][1] -eq 0x3c46304) 'an IPS file''s write ranges are read'
    New-Ips (Join-Path $g2 'DynFPS3\exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips') @(0x6e6dc4) 4
    Check (@(Get-KitMods $u2).Count -eq 0) 'a plain IPS over the call alone: no mod either'
    $pch = "@nsobid-$build`r`n@flag offset_shift 0x100`r`n@enabled`r`n// the call`r`n006E6CC4 4F80D594`r`n@stop`r`n"
    Write-File (Join-Path $g2 'DynFPStxt\exefs\dynamic.pchtxt') $pch
    Check (@(Get-KitMods $u2).Count -eq 0) 'an IPSwitch Dynamic FPS (offset_shift 0x100): no mod'
    Check (((Get-KitPchtxt (Join-Path $g2 'DynFPStxt\exefs\dynamic.pchtxt')).Ranges[0] -join ',') -eq "$(0x6e6dc4),$(0x6e6dc8)") 'an IPSwitch file''s write ranges are read'
    # Not Dynamic FPS: a patch elsewhere, a disabled IPSwitch patch, a name the emulator does not match.
    New-Ips (Join-Path $g2 'Other\exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips') @(0x10000) 4
    Write-File (Join-Path $g2 'OffTxt\exefs\x.pchtxt') ("@nsobid-$build`r`n@flag offset_shift 0x100`r`n@disabled`r`n006E6CC4 4F80D594`r`n")
    New-Ips (Join-Path $g2 'Lower\exefs\e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348.ips') @(0x6e6dc4) 4
    $mods = @(Get-KitMods $u2 | ForEach-Object { Split-Path -Leaf $_ } | Sort-Object)
    Check (($mods -join ',') -eq 'Lower,OffTxt,Other') "patches elsewhere, disabled or not matched by name stay mods [$($mods -join ',')]"
    # With the built-in turned off in Add-Ons, Fritchii loads Dark's mod: it counts.
    Write-File (Join-Path $u2 'config\qt-config.ini') "[DisabledAddOns]`r`nsize=1`r`n1\title_id=$id`r`n1\disabled\size=1`r`n1\disabled\1\d=Dynamic FPS by Dark (built-in)`r`n"
    Check (@(Get-KitMods $u2).Count -eq 6) 'the built-in turned off: Dark''s mods count'
    Remove-Item -LiteralPath (Join-Path $u2 'config\qt-config.ini') -Force

    # Loose cheat files in the game's load folder ("Cheats" in Add-Ons), turned off by file name.
    Write-File (Join-Path $g2 'cheat_infinite_hearts.txt') '[Hearts]'
    $mods = @(Get-KitMods $u2 | ForEach-Object { Split-Path -Leaf $_ })
    Check ($mods -contains 'cheat_infinite_hearts.txt') 'a loose cheat_* file counts'
    Write-File (Join-Path $u2 'config\qt-config.ini') "[DisabledAddOns]`r`nsize=1`r`n1\title_id=$id`r`n1\disabled\size=1`r`n1\disabled\1\d=cheat_infinite_hearts.txt`r`n"
    Check (-not (@(Get-KitMods $u2 | ForEach-Object { Split-Path -Leaf $_ }) -contains 'cheat_infinite_hearts.txt')) 'a cheat file turned off does not'
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
