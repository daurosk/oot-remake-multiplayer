# Tests for the kit's user-folder check (tools/release/check_paths.ps1).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\paths_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'check_paths.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

# --- What is looked for
$n = Get-UserPathNeedles 'C:\Users\Navi\'
Check (($n -join '|') -eq '\Users\Navi|/Users/Navi') 'the needles drop the drive and the last slash, both slashes'
Check (((Get-UserPathNeedles 'D:/home/navi') -join '|') -eq '\home\navi|/home/navi') 'forward slashes and another drive'
$threw = $false
try { Get-UserPathNeedles 'C:\' | Out-Null } catch { $threw = $true }
Check $threw 'a drive alone is no user folder'

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-paths-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path (Join-Path $tmp 'kit\emulator') | Out-Null
try {
    $kit = Join-Path $tmp 'kit'
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText((Join-Path $kit 'clean.txt'), "kit: oot-coop-v0.1.0-test4`r`nbuilt: 2026-11-10`r`n", $utf8)
    [System.IO.File]::WriteAllText((Join-Path $kit 'VERSION.txt'), "co-op hook from C:\Users\Navi\Desktop\repo`r`n", $utf8)
    [System.IO.File]::WriteAllText((Join-Path $kit 'unix.txt'), "/c/users/navi/build/x.cpp`n", $utf8)
    # A program: the path as UTF-16 among other bytes, and one as one-byte text.
    $bytes = [byte[]](0x4d, 0x5a, 0x00, 0x90) + [System.Text.Encoding]::Unicode.GetBytes('C:\Users\NAVI\src\main.cpp') + [byte[]](0, 0, 0xff)
    [System.IO.File]::WriteAllBytes((Join-Path $kit 'emulator\wide.exe'), $bytes)
    $bytes = [byte[]](0x4d, 0x5a, 0x00) + [System.Text.Encoding]::ASCII.GetBytes('c:/users/navi/fritchii/src/core/core.cpp') + [byte[]](0)
    [System.IO.File]::WriteAllBytes((Join-Path $kit 'emulator\narrow.dll'), $bytes)
    # A trimmed program: only the part below the source root.
    $bytes = [byte[]](0x4d, 0x5a) + [System.Text.Encoding]::ASCII.GetBytes('src\core\core.cpp') + [byte[]](0)
    [System.IO.File]::WriteAllBytes((Join-Path $kit 'emulator\trimmed.exe'), $bytes)
    # A longer name that starts with ours is found too: better too strict than not.
    [System.IO.File]::WriteAllText((Join-Path $kit 'other.txt'), "C:\Users\Navigator\x`r`n", $utf8)

    $hits = @(Find-UserPath $kit (Get-UserPathNeedles 'C:\Users\Navi'))
    $names = @($hits | ForEach-Object { Split-Path -Leaf $_.File } | Sort-Object)
    Check (($names -join ',') -eq 'narrow.dll,other.txt,unix.txt,VERSION.txt,wide.exe') "the files with the folder are found: $($names -join ', ')"
    Check (-not ($names -contains 'clean.txt')) 'a clean text file is not'
    Check (-not ($names -contains 'trimmed.exe')) 'a program with trimmed paths is not'
    Check ((@($hits | Where-Object { (Split-Path -Leaf $_.File) -eq 'wide.exe' })[0].Needle) -eq '\Users\Navi') 'a hit says what it found'
    Check (@(Find-UserPath (Join-Path $kit 'clean.txt') (Get-UserPathNeedles 'C:\Users\Navi')).Count -eq 0) 'one file can be checked alone'
    Check (@(Find-UserPath $kit (Get-UserPathNeedles 'C:\Users\Saria')).Count -eq 0) 'nothing of another user'
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
