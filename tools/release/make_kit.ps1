# Builds a co-op test kit: one folder (and a zip of it) to copy to each test PC.
# It holds the server, a copy of the patched emulator with an empty portable user\ folder,
# Host.bat, Join.bat, play.cmd, start-server.cmd, collect-logs.cmd, make-puppet-pack.cmd
# with tools\puppet_pack, coop-options.ini (feature switches on, debug off), the player
# guides (SETUP.md, PLAY-LAN.md, PLAY-ONLINE.md, KNOWN-ISSUES.md), TESTING.md, the licence
# texts (licenses\, THIRD_PARTY_NOTICES.md), SOURCE.txt (where this build's source is) and
# VERSION.txt.
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
# -Release      a release name such as v0.1.0-test1: the kit is named oot-coop-<Release>
#               instead of oot-coop-<date>, and VERSION.txt and SOURCE.txt name it.
param(
    [string]$EmulatorBin = '',
    [string]$BuildDir = '',
    [string]$OutRoot = '',
    [ValidateRange(1, 65535)][int]$Port = 27510,
    [switch]$IncludePdb,
    [switch]$NoZip,
    [string]$Python = '',
    [ValidatePattern('^[A-Za-z0-9._-]*$')][string]$Release = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
. (Join-Path $here 'kit\scripts\coop-ini.ps1')

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
$skip = @('/XD', (Join-Path $EmulatorBin 'user'))
if (-not $IncludePdb) { $skip += @('/XF', '*.pdb') }
& robocopy $EmulatorBin $emu /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP @skip | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying the emulator failed (robocopy $LASTEXITCODE)." }
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

foreach ($f in 'Host.bat', 'Join.bat', 'start-server.cmd', 'play.cmd', 'collect-logs.cmd', 'make-puppet-pack.cmd', 'coop-options.ini') { Copy-Text (Join-Path $here "kit\$f") (Join-Path $kit $f) }
foreach ($f in 'start-server.ps1', 'play.ps1', 'collect-logs.ps1', 'coop-ini.ps1', 'kit-paths.ps1', 'puppet-pack.ps1') { Copy-Text (Join-Path $here "kit\scripts\$f") (Join-Path $kit "scripts\$f") }

# The puppet pack tool, run on each PC by make-puppet-pack.cmd. Its tests and the
# scene-placing test helper stay out.
$packTool = Join-Path $repo 'tools\puppet_pack'
foreach ($f in 'build.py', '__init__.py', 'byml.py', 'rstb.py', 'sarc.py', 'zs.py') {
    Copy-Text (Join-Path $packTool $f) (Join-Path $kit "tools\puppet_pack\$f")
}

# Optionally a Python for it.
$pythonNote = 'not included (each PC needs Python 3 and zstandard)'
if ($Python) {
    $info = & $Python -I -c 'import sys, os, zstandard; print(sys.base_prefix); print(os.path.dirname(zstandard.__file__)); print(sys.version.split()[0])' 2>$null
    if ($LASTEXITCODE -ne 0 -or $info.Count -lt 3) { throw "$Python is not a Python with the zstandard package." }
    $pyHome = $info[0].Trim(); $zstd = $info[1].Trim(); $pyVersion = $info[2].Trim()
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
    & (Join-Path $pyOut 'python.exe') -I -c 'import zstandard' 2>$null
    if ($LASTEXITCODE -ne 0) { throw "The kit's Python cannot import zstandard." }
    $pythonNote = "Python $pyVersion from $pyHome, zstandard from $zstd"
}
Copy-Text (Join-Path $here 'kit\coop.ini.template') (Join-Path $kit 'scripts\coop.ini.template')
New-Item -ItemType Directory -Force -Path (Join-Path $kit 'logs') | Out-Null
Copy-Text (Join-Path $repo 'docs\testing.md') (Join-Path $kit 'TESTING.md')
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
    '',
    'Qt 6.8.3 (LGPL-3.0): https://download.qt.io/archive/qt/6.8/6.8.3/',
    'The other libraries are fetched at pinned versions by the Fritchii build (CPM).',
    '',
    'If a link stops working, open an issue on the co-op repo and the source will be',
    'sent to you.'
)
[System.IO.File]::WriteAllLines((Join-Path $kit 'SOURCE.txt'), [string[]]$source)

$version = @(
    "kit: $name$(if ($Release) { " (release $Release)" })",
    "built: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz') on $($env:COMPUTERNAME)",
    "co-op repo (this script): $(Describe-Commit $repo)",
    "co-op hook built into the emulator: $(Describe-Commit $hookDir)",
    "  from $hookDir",
    "server built from: $(Describe-Commit $serverSource)",
    "  $server",
    "Fritchii pin: $($pin.tag) $($pin.commit)",
    "Fritchii source built: $(Describe-Commit $emuSource)",
    "  $(Invoke-Git $emuSource describe --tags --always)",
    "emulator build type: $emuType",
    "lichii.exe: $((Get-Item $lichii).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')), $((Get-FileHash $lichii -Algorithm SHA256).Hash.Substring(0, 16)) (sha256, first 16)",
    "coop_server.exe: $((Get-Item $server).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')), $((Get-FileHash $server -Algorithm SHA256).Hash.Substring(0, 16)) (sha256, first 16)",
    "visual c++ runtime: $(if ($crt) { $crt.FullName } else { 'not included' })",
    "puppet pack tool: tools\puppet_pack from the co-op repo above; packs are built on each PC",
    "python: $pythonNote",
    "default port: $Port"
)
[System.IO.File]::WriteAllLines((Join-Path $kit 'VERSION.txt'), [string[]]$version)

# Nothing of the game, no keys or firmware and no one's emulator data may go out.
$banned = Get-ChildItem $kit -Recurse -File | Where-Object {
    $_.Extension -in '.keys', '.nsp', '.nup', '.xci', '.nca', '.nro', '.nso', '.zs', '.pack', '.bfres', '.sarc' -or
    $_.FullName -match '\emulator\user\(nand|sdmc|load|dump|cache|shader|keys\.+)'
}
if ($banned) { throw "The kit holds files that must not ship: $(($banned | Select-Object -First 5 | ForEach-Object FullName) -join ', ')" }

$size = (Get-ChildItem $kit -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ('Folder: {0:N0} MB' -f ($size / 1MB))
if (-not $NoZip) {
    $zip = "$kit.zip"
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $OutRoot $name
    if ($LASTEXITCODE -ne 0) { throw "Zipping failed (tar $LASTEXITCODE)." }
    Write-Host ('Zip: {0} ({1:N0} MB)' -f $zip, ((Get-Item $zip).Length / 1MB))
}
