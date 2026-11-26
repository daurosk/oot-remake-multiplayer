# The source folder of a dependency CMake's FetchContent gave a build, and its licence text, for the kit scripts
# (make_kit_hookshot.ps1; tools/release/kit_deps.sh is the same for the Linux ones).
# Read from the build's CMakeCache.txt, as CMake itself decides it:
#   1. FETCHCONTENT_SOURCE_DIR_<NAME> (a source given from outside: an offline configure, -DFETCHCONTENT_SOURCE_DIR_FMT=...);
#   2. else <FETCHCONTENT_BASE_DIR>/<name>-src (FetchContent's own download; the base is <build>/_deps unless set).
# So a build configured with FETCHCONTENT_FULLY_DISCONNECTED and the sources elsewhere (coord's offline-cmake.ps1) has no
# _deps/<name>-src, and its licences are still found where the build really read them.

function Get-KitDepSource([string]$BuildDir, [string]$Name) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    $base = Join-Path $BuildDir '_deps'
    if (Test-Path -LiteralPath $cache) {
        $up = $Name.ToUpperInvariant()
        foreach ($line in [System.IO.File]::ReadAllLines($cache)) {
            if ($line -match "^FETCHCONTENT_SOURCE_DIR_${up}:[A-Z]+=(.+)$") {
                $dir = $Matches[1].Trim()
                if ($dir -and (Test-Path -LiteralPath $dir -PathType Container)) { return [System.IO.Path]::GetFullPath($dir) }
            } elseif ($line -match '^FETCHCONTENT_BASE_DIR:[A-Z]+=(.+)$' -and $Matches[1].Trim()) {
                $base = $Matches[1].Trim()
            }
        }
    }
    return [System.IO.Path]::GetFullPath((Join-Path $base "$($Name.ToLowerInvariant())-src"))
}

# The first LICENSE* file of that dependency in the first of $BuildDirs that has one; $null when none has.
function Find-KitDepLicense([string[]]$BuildDirs, [string]$Name) {
    foreach ($b in $BuildDirs) {
        if (-not $b) { continue }
        $src = Get-KitDepSource $b $Name
        $f = Get-ChildItem -LiteralPath $src -Filter 'LICENSE*' -File -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
        if ($f) { return $f.FullName }
    }
    return $null
}
