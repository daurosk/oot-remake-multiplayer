# Tests for tools/release/kit_deps.ps1: a dependency's source folder and licence as the build's CMakeCache.txt says
# (FETCHCONTENT_SOURCE_DIR_<NAME>, FETCHCONTENT_BASE_DIR, else <build>/_deps/<name>-src). Made-up builds in %TEMP%.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\kit_deps_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit_deps.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("kit-deps-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
function New-File([string]$Path, [string]$Text = 'x') {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text)
}
try {
    # 1. FetchContent's own download: <build>/_deps/<name>-src, no cache needed.
    $b1 = Join-Path $root 'b1'
    New-File (Join-Path $b1 '_deps\fmt-src\LICENSE') 'fmt licence'
    New-File (Join-Path $b1 '_deps\enet-src\LICENSE') 'enet licence'
    Check ((Find-KitDepLicense @($b1) 'fmt') -eq (Join-Path $b1 '_deps\fmt-src\LICENSE')) 'fmt in _deps/fmt-src'
    Check ((Find-KitDepLicense @($b1) 'enet') -eq (Join-Path $b1 '_deps\enet-src\LICENSE')) 'enet in _deps/enet-src'

    # 2. An offline configure: sources given from outside (FETCHCONTENT_SOURCE_DIR_*, forward slashes as CMake writes them),
    #    no _deps at all. This is the case that failed (offline-cmake.ps1).
    $b2 = Join-Path $root 'b2'
    $deps = Join-Path $root 'deps'
    New-File (Join-Path $deps 'fmt-src\LICENSE.rst') 'fmt licence'
    New-File (Join-Path $deps 'enet-src\LICENSE') 'enet licence'
    New-File (Join-Path $b2 'CMakeCache.txt') (@(
        '# This is the CMakeCache file.',
        "FETCHCONTENT_FULLY_DISCONNECTED:BOOL=ON",
        "FETCHCONTENT_SOURCE_DIR_FMT:PATH=$((Join-Path $deps 'fmt-src') -replace '\\', '/')",
        "FETCHCONTENT_SOURCE_DIR_ENET:PATH=$((Join-Path $deps 'enet-src') -replace '\\', '/')") -join "`n")
    Check ((Find-KitDepLicense @($b2) 'fmt') -eq (Join-Path $deps 'fmt-src\LICENSE.rst')) 'fmt from FETCHCONTENT_SOURCE_DIR_FMT'
    Check ((Find-KitDepLicense @($b2) 'enet') -eq (Join-Path $deps 'enet-src\LICENSE')) 'enet from FETCHCONTENT_SOURCE_DIR_ENET'

    # 3. FETCHCONTENT_BASE_DIR moved: <base>/<name>-src.
    $b3 = Join-Path $root 'b3'
    $base = Join-Path $root 'base3'
    New-File (Join-Path $base 'fmt-src\LICENSE') 'fmt licence'
    New-File (Join-Path $b3 'CMakeCache.txt') "FETCHCONTENT_BASE_DIR:PATH=$($base -replace '\\', '/')"
    Check ((Find-KitDepLicense @($b3) 'fmt') -eq (Join-Path $base 'fmt-src\LICENSE')) 'fmt under FETCHCONTENT_BASE_DIR'

    # 4. A source dir in the cache that is not there: CMake's own _deps is used.
    $b4 = Join-Path $root 'b4'
    New-File (Join-Path $b4 '_deps\fmt-src\LICENSE') 'fmt licence'
    New-File (Join-Path $b4 'CMakeCache.txt') 'FETCHCONTENT_SOURCE_DIR_FMT:PATH=Z:/nowhere/fmt-src'
    Check ((Find-KitDepLicense @($b4) 'fmt') -eq (Join-Path $b4 '_deps\fmt-src\LICENSE')) 'missing given source: _deps'

    # 5. None anywhere: $null (the kit script then stops with the folder it looked in); the first build with one wins.
    $b5 = Join-Path $root 'b5'
    New-Item -ItemType Directory -Force -Path $b5 | Out-Null
    Check ($null -eq (Find-KitDepLicense @($b5) 'fmt')) 'no licence: null'
    Check ((Find-KitDepLicense @($b5, '', $b2) 'fmt') -eq (Join-Path $deps 'fmt-src\LICENSE.rst')) 'the next build is tried'
    Check ((Get-KitDepSource $b5 'FMT') -eq [System.IO.Path]::GetFullPath((Join-Path $b5 '_deps\fmt-src'))) 'the folder it looked in, lower case'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'kit_deps: all checks passed'
