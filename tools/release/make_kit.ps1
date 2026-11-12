# Builds a co-op test kit: one folder (and a zip of it) to copy to each test PC.
# It holds the server, a copy of the patched emulator with an empty portable user\ folder,
# Host.bat, Join.bat, play.cmd, start-server.cmd, collect-logs.cmd, check-hosting.cmd, Update.bat
# (make_update_bat.ps1), make-puppet-pack.cmd with tools\puppet_pack, coop-options.ini (feature switches on, debug off), the player
# guides (SETUP.md, PLAY-LAN.md, PLAY-ONLINE.md, KNOWN-ISSUES.md), TESTING.md, the licence
# texts (licenses\, THIRD_PARTY_NOTICES.md), SOURCE.txt (where this build's source is),
# MANIFEST.txt (the kit's own files, for Update.bat) and VERSION.txt.
# It refuses to finish when the build machine's user folder appears in any file of the
# kit (check_paths.ps1): build the emulator with trimmed paths (lichii/README.md).
# No game files, keys or firmware go in, and no puppet pack: each PC adds its own and
# builds the pack from its own romfs (docs/testing.md).
#
#   powershell -ExecutionPolicy Bypass -File tools\release\make_kit.ps1
#
# -EmulatorBin  the patched emulator's bin folder (default ..\fritchii-src\build\coop\bin,
#               beside the main checkout, as lichii/README.md builds it)
# -BuildDir     this repo's CMake build folder with coop_server (default build\)
# -OutRoot      where the kit folder goes (default ..\coop-kit beside the main checkout)
# -Port         the default server port written into the kit (27510)
# -IncludePdb   also copy lichii.pdb (about 60 MB) for crash dumps with symbols
# -NoZip        skip the zip
# -Python       a python.exe that has the zstandard package: its Python (without other
#               packages) and zstandard are copied into the kit's python\ folder, so
#               make-puppet-pack.cmd runs on a PC without Python (about 60 MB more).
#               Without it, each PC needs Python 3 and zstandard of its own.
# -Release      a release name such as v0.1.0-beta1: the kit is named oot-coop-<Release>
#               instead of oot-coop-<date>, and VERSION.txt and SOURCE.txt name it.
# -Recipes      Fritchii's release zip (Fritchii-<version>-Windows-x64.zip) or a folder: every
#               .nx2recipe in it (in the zip, under recipes/) goes to emulator\recipes\, so
#               the first start shows Fritchii's "Preparing shaders" step. A recipe holds no
#               game code (Fritchii's tools/nx2-precompile/README.md); SOURCE.txt names it.
# -HookshotDll  the co-op module for Hookshot (hookshot_coop.dll, this repo's CMake target
#               hookshot_coop; default: found under -BuildDir). With it and hookshot\PIN in the repo,
#               the kit gets Build-Hookshot-Coop.bat and hookshot\ (PIN, patches, TOOLS.txt, the module):
#               each player builds Hookshot from their own game (docs/play-online.md, "On Hookshot").
#               Without them the kit is emulator-only (a warning says so).
param(
    [string]$EmulatorBin = '',
    [string]$BuildDir = '',
    [string]$OutRoot = '',
    [ValidateRange(1, 65535)][int]$Port = 27510,
    [switch]$IncludePdb,
    [switch]$NoZip,
    [string]$Python = '',
    [ValidatePattern('^[A-Za-z0-9._-]*$')][string]$Release = '',
    [string]$Recipes = '',
    [string]$HookshotDll = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
. (Join-Path $here 'kit\scripts\coop-ini.ps1')
. (Join-Path $here 'check_paths.ps1')

function Invoke-Git([string]$Dir) {
    $out = & git.exe -C $Dir @args 2>$null
    if ($LASTEXITCODE -ne 0) { return '' }
    return ($out -join "`n").Trim()
}
function Describe-Commit([string]$Dir) {
    if (-not $Dir) { return 'unknown' }
    if (-not (Test-Path $Dir)) { return 'unknown (that folder is gone)' }
    $sha = Invoke-Git $Dir rev-parse HEAD
    if (-not $sha) { return 'unknown (not a git checkout)' }
    $dirty = if (Invoke-Git $Dir status --porcelain --untracked-files=no) { ' with uncommitted changes' } else { '' }
    $branch = Invoke-Git $Dir rev-parse --abbrev-ref HEAD
    return "$sha ($branch)$dirty"
}
function Read-CMakeCache([string]$Dir, [string]$Key) {
    $cache = Join-Path $Dir 'CMakeCache.txt'
    if (-not (Test-Path $cache)) { return '' }
    $line = Select-String -Path $cache -Pattern "^$Key(:[A-Z]+)?=(.*)$" | Select-Object -First 1
    if ($line) { return $line.Matches[0].Groups[2].Value }
    return ''
}
function Test-BinaryHas([string]$Path, [string]$Text) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    return [System.Text.Encoding]::ASCII.GetString($bytes).Contains($Text)
}
# Copies a text file and gives it Windows line ends (the repo stores LF) with @PORT@ filled in.
function Copy-Text([string]$From, [string]$To) {
    $text = [System.IO.File]::ReadAllText($From).Replace("`r`n", "`n").Replace('@PORT@', "$Port")
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
    [System.IO.File]::WriteAllText($To, $text.Replace("`n", "`r`n"), [System.Text.UTF8Encoding]::new($false))
}

$repo = Invoke-Git $here rev-parse --show-toplevel
if (-not $repo) { throw "tools\release is not inside a git checkout" }
$repo = [System.IO.Path]::GetFullPath($repo)
$commonDir = Invoke-Git $repo rev-parse --path-format=absolute --git-common-dir
$mainCheckout = Split-Path -Parent ([System.IO.Path]::GetFullPath($commonDir))
$besideMain = Split-Path -Parent $mainCheckout
if (-not $EmulatorBin) { $EmulatorBin = Join-Path $besideMain 'fritchii-src\build\coop\bin' }
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build' }
if (-not $OutRoot) { $OutRoot = Join-Path $besideMain 'coop-kit' }
$EmulatorBin = [System.IO.Path]::GetFullPath($EmulatorBin)
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutRoot = [System.IO.Path]::GetFullPath($OutRoot)
if (($OutRoot + '\').StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "The kit must go outside the repo (it holds the emulator): $OutRoot"
}

# The emulator: a co-op build, Release.
$lichii = Join-Path $EmulatorBin 'lichii.exe'
if (-not (Test-Path $lichii)) { throw "No lichii.exe in $EmulatorBin. Build it as lichii/README.md says, or pass -EmulatorBin." }
if (-not (Test-BinaryHas $lichii 'coop: loaded')) { throw "$lichii is not a co-op build (built without -DLICHII_COOP=ON)." }
# Fritchii's own updater would offer a plain Fritchii in place of this co-op build. Kits are
# built with -DENABLE_UPDATE_CHECKER=OFF; Update.bat is the kit's only updater.
foreach ($text in 'Check for Updates', 'update-old') {
    if (Test-BinaryHas $lichii $text) { throw "$lichii has Fritchii's updater ('$text'). Build it with -DENABLE_UPDATE_CHECKER=OFF." }
}
$emuBuild = Split-Path -Parent $EmulatorBin
$emuType = Read-CMakeCache $emuBuild 'CMAKE_BUILD_TYPE'
if ($emuType -and $emuType -ne 'Release') { Write-Warning "The emulator build type is $emuType, not Release." }
$emuSource = Read-CMakeCache $emuBuild 'CMAKE_HOME_DIRECTORY'
if (-not $emuSource) { $emuSource = Invoke-Git $EmulatorBin rev-parse --show-toplevel }
$hookDir = Read-CMakeCache $emuBuild 'LICHII_COOP_DIR'

# The server: Release, so it runs without the debug runtime.
$server = @('server\coop_server.exe', 'server\Release\coop_server.exe') |
    ForEach-Object { Join-Path $BuildDir $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $server) { throw "No coop_server.exe in $BuildDir. Build it: cmake -S . -B build, then cmake --build build --config Release --target coop_server" }
if (Test-BinaryHas $server 'VCRUNTIME140D.dll') { throw "$server is a Debug build; build Release." }
$serverSource = Read-CMakeCache $BuildDir 'CMAKE_HOME_DIRECTORY'

# Fritchii pin.
$pin = @{}
Get-Content (Join-Path $repo 'lichii\PIN') | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $pin[$Matches[1]] = $Matches[2].Trim() } }
if ($emuSource -and (Invoke-Git $emuSource rev-parse HEAD)) {
    & git -C $emuSource merge-base --is-ancestor $pin.commit HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Warning "The emulator source is not based on the pinned $($pin.tag) ($($pin.commit))." }
}

# The Visual C++ runtime, copied beside both programs so a PC without it can run them.
$crt = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    foreach ($vs in & $vswhere -all -products * -property installationPath) {
        $crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($crt) { break }
    }
}
if (-not $crt) { Write-Warning 'Visual C++ runtime folder not found; the kit will need the VC++ 2015-2022 x64 runtime installed on each PC.' }

# Lay out the kit.
$name = if ($Release) { "oot-coop-$Release" } else { 'oot-coop-{0:yyyyMMdd-HHmm}' -f (Get-Date) }
$kit = Join-Path $OutRoot $name
if (Test-Path $kit) { throw "$kit already exists; wait a minute or remove it." }
New-Item -ItemType Directory -Force -Path $kit | Out-Null
Write-Host "Kit: $kit"

$emu = Join-Path $kit 'emulator'
# Fritchii's shader recipes go in first; the emulator copy below adds to the folder.
$recipeLines = @()
if ($Recipes) {
    if (-not (Test-Path -LiteralPath $Recipes)) { throw "-Recipes: $Recipes not found." }
    $recipeDir = Join-Path $emu 'recipes'
    New-Item -ItemType Directory -Force -Path $recipeDir | Out-Null
    if ((Get-Item -LiteralPath $Recipes).PSIsContainer) {
        Get-ChildItem -LiteralPath $Recipes -Filter '*.nx2recipe' -File -Recurse |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName (Join-Path $recipeDir $_.Name) }
    } else {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Recipes).Path)
        try {
            foreach ($entry in $zip.Entries) {
                if ($entry.FullName -match '(^|/)recipes/[^/]+\.nx2recipe$') {
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $recipeDir $entry.Name), $true)
                }
            }
        } finally { $zip.Dispose() }
    }
    foreach ($r in Get-ChildItem -LiteralPath $recipeDir -File) {
        $recipeLines += "  emulator\recipes\$($r.Name)  SHA-256 $((Get-FileHash -LiteralPath $r.FullName -Algorithm SHA256).Hash.ToLower())"
    }
    if (-not $recipeLines) { throw "-Recipes: no .nx2recipe in $Recipes." }
    Write-Host "Shader recipes: $($recipeLines.Count) from $(Split-Path -Leaf $Recipes)"
}
$skip = @('/XD', (Join-Path $EmulatorBin 'user'))
if (-not $IncludePdb) { $skip += @('/XF', '*.pdb') }
& robocopy $EmulatorBin $emu /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP @skip | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying the emulator failed (robocopy $LASTEXITCODE)." }
# Fritchii's "Preparing shaders" step (video_core/nx2_precompile.cpp) only runs with a recipe in
# recipes\ beside the exe. Without it every new place stutters while shaders build, worst on AMD,
# where Fritchii turns shader libraries off and relies on this step (docs/setup.md, "Low frame rate").
if (-not (Get-ChildItem -LiteralPath (Join-Path $emu 'recipes') -File -ErrorAction SilentlyContinue)) {
    Write-Warning "No recipes folder beside lichii.exe: players get no 'Preparing shaders' step, so more stutter (most on AMD). Pass -Recipes with Fritchii's release zip."
}
foreach ($sub in 'config', 'keys', 'log') { New-Item -ItemType Directory -Force -Path (Join-Path $emu "user\$sub") | Out-Null }
Write-CoopIni (Join-Path $here 'kit\coop.ini.template') (Join-Path $emu 'user\config\coop.ini') '' "$Port" ''

$srv = Join-Path $kit 'server'
New-Item -ItemType Directory -Force -Path $srv | Out-Null
Copy-Item $server $srv
if ($crt) {
    foreach ($dir in $srv, $emu) {
        Get-ChildItem $crt.FullName -Filter '*.dll' | Where-Object { -not (Test-Path (Join-Path $dir $_.Name)) } |
            Copy-Item -Destination $dir
    }
}

foreach ($f in 'Host.bat', 'Join.bat', 'start-server.cmd', 'start-join.cmd', 'start-relay.cmd', 'play.cmd', 'collect-logs.cmd', 'make-puppet-pack.cmd', 'check-hosting.cmd', 'coop-options.ini') { Copy-Text (Join-Path $here "kit\$f") (Join-Path $kit $f) }
foreach ($f in 'start-server.ps1', 'play.ps1', 'collect-logs.ps1', 'coop-ini.ps1', 'kit-paths.ps1', 'puppet-pack.ps1', 'update.ps1', 'hosting.ps1', 'check-hosting.ps1', 'log-mask.ps1', 'relay-join.ps1', 'invite.ps1', 'start-join.ps1') { Copy-Text (Join-Path $here "kit\scripts\$f") (Join-Path $kit "scripts\$f") }
# The updater's signature check (update.ps1, Test-KitSignature) runs the installed kit's own copy.
$verifyKit = Join-Path $here 'verify_kit.ps1'
Copy-Text $verifyKit (Join-Path $kit 'scripts\verify_kit.ps1')
# Update.bat: the updater in one file (the same file is attached to each release for old kits).
& (Join-Path $here 'make_update_bat.ps1') -Out (Join-Path $kit 'Update.bat')

# Hookshot (experimental): the script that builds it on the player's PC from their own game, our
# pin and patches, the tools list and the module. No Hookshot or game file goes in.
$hookshotNote = ''
$hsPin = Join-Path $repo 'hookshot\PIN'
if (-not $HookshotDll) {
    $HookshotDll = Get-ChildItem -LiteralPath $BuildDir -Filter 'hookshot_coop.dll' -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\(Debug|_deps)\\' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not (Test-Path $hsPin)) {
    Write-Warning 'No hookshot\PIN in the repo: the kit has no Hookshot part.'
} elseif (-not $HookshotDll -or -not (Test-Path -LiteralPath $HookshotDll)) {
    Write-Warning "No hookshot_coop.dll (build the hookshot_coop target, Release, or pass -HookshotDll): the kit has no Hookshot part."
} else {
    if (Test-BinaryHas $HookshotDll 'VCRUNTIME140D.dll') { throw "$HookshotDll is a Debug build; build Release." }
    $hsKit = Join-Path $kit 'hookshot'
    New-Item -ItemType Directory -Force -Path (Join-Path $hsKit 'patches') | Out-Null
    Copy-Item -LiteralPath $HookshotDll -Destination $hsKit
    if ($crt) { Get-ChildItem $crt.FullName -Filter '*.dll' | Copy-Item -Destination $hsKit }
    Copy-Text $hsPin (Join-Path $hsKit 'PIN')
    Copy-Text (Join-Path $here 'kit\hookshot\TOOLS.txt') (Join-Path $hsKit 'TOOLS.txt')
    $hsPatches = @(Get-ChildItem (Join-Path $repo 'hookshot\patches') -Filter '*.patch' -File | Sort-Object Name)
    if (-not $hsPatches) { throw 'hookshot\PIN is there but hookshot\patches has no .patch.' }
    # (byte for byte: a patch's own line ends are part of it)
    foreach ($p in $hsPatches) { Copy-Item -LiteralPath $p.FullName -Destination (Join-Path $hsKit "patches\$($p.Name)") }
    Copy-Text (Join-Path $here 'kit\Build-Hookshot-Coop.bat') (Join-Path $kit 'Build-Hookshot-Coop.bat')
    foreach ($f in 'hookshot-build.ps1', 'apply_patch.py') { Copy-Text (Join-Path $here "kit\scripts\$f") (Join-Path $kit "scripts\$f") }
    $hsPinMap = @{}
    Get-Content $hsPin | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $hsPinMap[$Matches[1]] = $Matches[2].Trim() } }
    $hookshotNote = $hsPinMap.commit
    Write-Host "Hookshot: module $(Split-Path -Leaf $HookshotDll), pin $($hsPinMap.commit), $($hsPatches.Count) patch(es)"
}

# The puppet pack tool, run on each PC by make-puppet-pack.cmd. Its tests and the
# scene-placing test helper stay out.
$packTool = Join-Path $repo 'tools\puppet_pack'
foreach ($f in 'ainb.py', 'build.py', '__init__.py', 'byml.py', 'dumpcheck.py', 'dungeon_rooms.py', 'hud.py', 'nup.py', 'rstb.py', 'sarc.py', 'texture.py', 'zs.py') {
    Copy-Text (Join-Path $packTool $f) (Join-Path $kit "tools\puppet_pack\$f")
}

# Optionally a Python for it.
$pythonNote = 'not included (each PC needs Python 3 and zstandard)'
if ($Python) {
    $info = & $Python -I -B -c 'import sys, os, zstandard; print(sys.base_prefix); print(os.path.dirname(zstandard.__file__)); print(sys.version.split()[0]); print(zstandard.__version__)' 2>$null
    if ($LASTEXITCODE -ne 0 -or $info.Count -lt 4) { throw "$Python is not a Python with the zstandard package." }
    $pyHome = $info[0].Trim(); $zstd = $info[1].Trim(); $pyVersion = $info[2].Trim(); $zstdVersion = $info[3].Trim()
    $pyOut = Join-Path $kit 'python'
    # Python's own files, without other packages, tests, Tk, headers or pip.
    $leaveOut = 'Lib\site-packages', 'Lib\test', 'Lib\idlelib', 'Lib\tkinter', 'Lib\ensurepip', 'tcl', 'include', 'libs', 'Scripts' |
        ForEach-Object { Join-Path $pyHome $_ }
    & robocopy $pyHome $pyOut /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP /XD @leaveOut __pycache__ | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying Python failed (robocopy $LASTEXITCODE)." }
    $site = Join-Path $pyOut 'Lib\site-packages'
    & robocopy $zstd (Join-Path $site 'zstandard') /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP /XD __pycache__ | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying zstandard failed (robocopy $LASTEXITCODE)." }
    # Its licence travels with it (in the dist-info folder).
    Get-ChildItem (Split-Path -Parent $zstd) -Directory -Filter 'zstandard-*.dist-info' |
        ForEach-Object { Copy-Item $_.FullName $site -Recurse }
    # -B: no __pycache__ in the kit (compiled files carry the folder they were made in).
    & (Join-Path $pyOut 'python.exe') -I -B -c 'import zstandard' 2>$null
    if ($LASTEXITCODE -ne 0) { throw "The kit's Python cannot import zstandard." }
    $pythonNote = "Python $pyVersion, zstandard $zstdVersion"
}
Copy-Text (Join-Path $here 'kit\coop.ini.template') (Join-Path $kit 'scripts\coop.ini.template')
# A fresh kit's options, for update.ps1: it adds the keys a player's coop-options.ini lacks
# (one an older updater left short) on the kit's next start.
Copy-Text (Join-Path $here 'kit\coop-options.ini') (Join-Path $kit 'scripts\coop-options.default.ini')
New-Item -ItemType Directory -Force -Path (Join-Path $kit 'logs') | Out-Null
# TESTING.md: the part for players. How the kit is made and the automatic tests are about
# this repo and its build PC, so they stop at the marker line.
Copy-Text (Join-Path $repo 'docs\testing.md') (Join-Path $kit 'TESTING.md')
$testing = [System.IO.File]::ReadAllText((Join-Path $kit 'TESTING.md'))
$cut = $testing.IndexOf('<!-- kit: TESTING.md ends here -->')
if ($cut -lt 0) { throw 'docs\testing.md has no "<!-- kit: TESTING.md ends here -->" line.' }
$testing = $testing.Substring(0, $cut).TrimEnd() + "`r`n`r`nHow the kit is made, and the automatic tests: docs/testing.md in the co-op repo (SOURCE.txt).`r`n"
[System.IO.File]::WriteAllText((Join-Path $kit 'TESTING.md'), $testing, [System.Text.UTF8Encoding]::new($false))
foreach ($doc in @(@('setup.md', 'SETUP.md'), @('play-lan.md', 'PLAY-LAN.md'), @('play-online.md', 'PLAY-ONLINE.md'), @('known-issues.md', 'KNOWN-ISSUES.md'))) {
    Copy-Text (Join-Path $repo "docs\$($doc[0])") (Join-Path $kit $doc[1])
}
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $kit 'LICENSE.txt')
Copy-Text (Join-Path $repo 'THIRD_PARTY_NOTICES.md') (Join-Path $kit 'THIRD_PARTY_NOTICES.md')

# Licence texts of what is bundled (THIRD_PARTY_NOTICES.md says which is which).
$lic = Join-Path $kit 'licenses'
New-Item -ItemType Directory -Force -Path $lic | Out-Null
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $lic 'oot-remake-multiplayer-GPL-3.0.txt')
if ($emuSource -and (Test-Path (Join-Path $emuSource 'LICENSE.txt'))) {
    Copy-Item (Join-Path $emuSource 'LICENSE.txt') (Join-Path $lic 'Fritchii-GPL-3.0.txt')
    Get-ChildItem (Join-Path $emuSource 'LICENSES') -Filter '*.txt' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $lic "SPDX-$($_.Name)") }
} else { Write-Warning "The emulator source's LICENSE.txt was not found; add the licence texts by hand." }
$enetLicense = Join-Path $BuildDir '_deps\enet-src\LICENSE'
if (Test-Path $enetLicense) { Copy-Item $enetLicense (Join-Path $lic 'ENet-MIT.txt') }
else { Write-Warning 'ENet LICENSE not found in the build folder; add it by hand.' }

# Where the source of this exact build is (GPL-3.0, section 6).
$hookCommit = Invoke-Git $hookDir rev-parse HEAD
$emuCommit = Invoke-Git $emuSource rev-parse HEAD
$source = @(
    "Source code for this build$(if ($Release) { " ($Release)" })",
    '',
    'Everything in this kit that is GPL-3.0 can be rebuilt from public source.',
    '',
    'The co-op code (hook, server, puppet pack tool, these scripts):',
    "  https://github.com/daurosk/oot-remake-multiplayer/tree/$hookCommit",
    "  $(if ($Release) { "(tag $Release) " })commit $hookCommit",
    '',
    'The emulator: Fritchii by Frizti, with the small patch from the repo above:',
    "  https://github.com/Frizti/Fritchii/tree/$($pin.tag)   (commit $($pin.commit))",
    '  Patches: lichii/patches in the co-op repo; lichii/fetch.ps1 fetches that tag and',
    '  applies them, and lichii/README.md says how to build it with the co-op hook.',
    "  This kit's emulator was built from $emuCommit (that tag plus the patches).",
    ''
)
if ($hookshotNote) {
    $source += @(
        'Hookshot (experimental): NotRexed''s static recompilation, GPL-3.0. Not in this kit:',
        'Build-Hookshot-Coop.bat downloads it and builds it on your PC from your own game:',
        "  https://github.com/NotRexed/Hookshot/tree/$hookshotNote",
        '  with the patches in hookshot\patches (also hookshot/patches in the co-op repo), and',
        '  the build tools of the Hookshot release named in hookshot\TOOLS.txt.',
        '  hookshot\hookshot_coop.dll is built from the co-op repo above (CMake target hookshot_coop).',
        ''
    )
}
if ($recipeLines) {
    $source += @(
        "Shader recipes, from Frizti's Fritchii release ($(Split-Path -Leaf $Recipes)), unchanged:"
    ) + $recipeLines + @(
        '  A recipe holds no game code: each player builds the shaders from their own game.',
        ''
    )
}
$source += @(
    'Qt 6.8.3 (LGPL-3.0): https://download.qt.io/archive/qt/6.8/6.8.3/',
    'The other libraries are fetched at pinned versions by the Fritchii build (CPM).',
    '',
    'If a link stops working, open an issue on the co-op repo and the source will be',
    'sent to you.'
)
[System.IO.File]::WriteAllLines((Join-Path $kit 'SOURCE.txt'), [string[]]$source)

$version = @(
    "kit: $name$(if ($Release) { " (release $Release)" })",
    "built: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
    "co-op repo (this script): $(Describe-Commit $repo)",
    "co-op hook built into the emulator: $(Describe-Commit $hookDir)",
    "server built from: $(Describe-Commit $serverSource)",
    "Fritchii pin: $($pin.tag) $($pin.commit)",
    "Fritchii source built: $(Describe-Commit $emuSource)",
    "  $(Invoke-Git $emuSource describe --tags --always)",
    "emulator build type: $emuType",
    "lichii.exe: $((Get-Item $lichii).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')), $((Get-FileHash $lichii -Algorithm SHA256).Hash.Substring(0, 16)) (sha256, first 16)",
    "coop_server.exe: $((Get-Item $server).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')), $((Get-FileHash $server -Algorithm SHA256).Hash.Substring(0, 16)) (sha256, first 16)",
    "visual c++ runtime: $(if ($crt) { "$($crt.Name) $(Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $crt.FullName)))" } else { 'not included' })",
    "puppet pack tool: tools\puppet_pack from the co-op repo above; packs are built on each PC",
    "python: $pythonNote",
    "default port: $Port"
)
[System.IO.File]::WriteAllLines((Join-Path $kit 'VERSION.txt'), [string[]]$version)

# Nothing of the game, no keys or firmware and no one's emulator data may go out.
$banned = Get-ChildItem $kit -Recurse -File | Where-Object {
    $_.Extension -in '.keys', '.nsp', '.nup', '.xci', '.nca', '.nro', '.nso', '.zs', '.pack', '.bfres', '.sarc' -or
    $_.FullName -match '\\emulator\\user\\(nand|sdmc|load|dump|cache|shader|keys\\.+)'
}
if ($banned) { throw "The kit holds files that must not ship: $(($banned | Select-Object -First 5 | ForEach-Object FullName) -join ', ')" }

# No compiled Python: it names the folder it was compiled in, and Python makes it again.
Get-ChildItem $kit -Recurse -Directory -Force -Filter '__pycache__' | Sort-Object FullName -Descending |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }

# Nothing may name the build machine's user folder (issue 103: a player read the
# emulator's source paths in his log as someone else's). A .pdb holds the build's
# folders by design; it ships only with -IncludePdb, for crash dumps, so it only warns.
$userFolder = [Environment]::GetFolderPath('UserProfile')
$hits = @(Find-UserPath $kit (Get-UserPathNeedles $userFolder))
$pdbHits = @($hits | Where-Object { $_.File -like '*.pdb' })
$hits = @($hits | Where-Object { $_.File -notlike '*.pdb' })
if ($pdbHits) { Write-Warning "These name the build PC's user folder (symbols, -IncludePdb): $(($pdbHits | ForEach-Object File) -join ', ')" }
if ($hits) {
    throw ("The build PC's user folder ($userFolder) appears in $($hits.Count) kit file(s): " +
        "$(($hits | Select-Object -First 8 | ForEach-Object { $_.File.Substring($kit.Length + 1) }) -join ', '). " +
        'Build the emulator with trimmed paths (lichii/README.md, CMAKE_PROJECT_TOP_LEVEL_INCLUDES) and the server from this repo.')
}

# MANIFEST.txt: the kit's own files. The next Update.bat removes those of them the newer kit
# no longer has; the player's files (emulator\user\, logs\, coop-options.ini) are not listed.
$kitRoot = [System.IO.Path]::GetFullPath($kit).TrimEnd('\') + '\'
$own = @(Get-ChildItem $kit -Recurse -File | ForEach-Object { $_.FullName.Substring($kitRoot.Length) } |
    Where-Object { $_ -notmatch '^(?i)(emulator\\user\\|logs\\|coop-options\.ini$)' } | Sort-Object)
[System.IO.File]::WriteAllLines((Join-Path $kit 'MANIFEST.txt'), [string[]](@("# Files of $name, for Update.bat. Do not edit.", 'MANIFEST.txt') + $own))

$size = (Get-ChildItem $kit -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ('Folder: {0:N0} MB' -f ($size / 1MB))
if (-not $NoZip) {
    $zip = "$kit.zip"
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $OutRoot $name
    if ($LASTEXITCODE -ne 0) { throw "Zipping failed (tar $LASTEXITCODE)." }
    Write-Host ('Zip: {0} ({1:N0} MB)' -f $zip, ((Get-Item $zip).Length / 1MB))
    # The checksum Update.bat checks the download against: attach it to the release beside the zip.
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n", [System.Text.Encoding]::ASCII)
    Write-Host "SHA-256: $hash ($zip.sha256)"
    Copy-Item -LiteralPath (Join-Path $kit 'Update.bat') -Destination (Join-Path $OutRoot 'Update.bat') -Force
    Write-Host "Release assets: $name.zip, $name.zip.sha256 and Update.bat (for kits from before the updater), all in $OutRoot"
}
