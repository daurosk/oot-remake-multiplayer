# Builds the Hookshot-only co-op kit (no emulator): one folder (and a zip of it) a player unzips and starts with
# "Start OoT Co-op.bat". No emulator, Fritchii or Lichii file goes in: each player builds the Hookshot game on
# their own PC from their own game (scripts\hookshot-build.ps1), and Host and Join are in the game itself.
# make_kit.ps1 still makes the emulator kits (the older releases); this script shares its kit scripts.
#
#   powershell -ExecutionPolicy Bypass -File tools\release\make_kit_hookshot.ps1 -Release <tag>
#
# The kit (docs/setup.md says what the player does with it):
#   Start OoT Co-op.bat     the one entry point: checks, update, first game build, start (scripts\start.ps1)
#   README.txt              ten lines: what to do
#   Update.bat              the updater in one file (also attached to the release for older kits)
#   coop-options.ini        the player's switches (the co-op module reads and writes it)
#   start-server.cmd        a dedicated server, for those who want one; with server\ it also marks the kit
#                           folder for the module (hookshot/src/coop_title.cpp, FindKit)
#   server\                 coop_server.exe and the Visual C++ runtime
#   hookshot\               hookshot_coop.dll (the co-op module), the runtime, PIN, TOOLS.txt, patches\
#   scripts\                everything else the launcher runs; collect-logs.cmd for bug reports
#   tools\puppet_pack\      builds the other players' Link from the player's own game
#   docs\                   SETUP.md, KNOWN-ISSUES.md
#   licenses\               GPL-3.0, THIRD_PARTY_NOTICES.md, SOURCE.txt, ENet and fmt licences
#   logs\, VERSION.txt, MANIFEST.txt (the kit's own files, for the updater)
# No game files, keys, firmware, puppet pack or Hookshot build go in.
#
# -BuildDir     this repo's CMake build folder with coop_server and hookshot_coop (Release; default build\)
# -HookshotDll  hookshot_coop.dll (default: found under -BuildDir)
# -OutRoot      where the kit folder goes (default ..\coop-kit beside the main checkout)
# -Port         the default server port written into the kit (27510)
# -Release      the release tag: the kit is named oot-coop-<Release>
# -NoZip        skip the zip
param(
    [string]$BuildDir = '',
    [string]$HookshotDll = '',
    [string]$OutRoot = '',
    [ValidateRange(1, 65535)][int]$Port = 27510,
    [ValidatePattern('^[A-Za-z0-9._-]*$')][string]$Release = '',
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
. (Join-Path $here 'check_paths.ps1')
. (Join-Path $here 'kit_text.ps1')
function Copy-Text([string]$From, [string]$To) { Copy-KitText $From $To $Port }
function Invoke-Git([string]$Dir) {
    $out = & git.exe -C $Dir @args 2>$null
    if ($LASTEXITCODE -ne 0) { return '' }
    return ($out -join "`n").Trim()
}
function Test-BinaryHas([string]$Path, [string]$Text) {
    return [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Path)).Contains($Text)
}
function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# What goes in. Files listed here and nowhere else, so a review sees the whole kit in one place.
$KitTop = @('Start OoT Co-op.bat', 'README.txt', 'start-server.cmd', 'coop-options.ini')
$KitScripts = @('start.ps1', 'start-checks.ps1', 'play.ps1', 'coop-ini.ps1', 'kit-paths.ps1', 'hosting.ps1', 'relay-join.ps1',
    'invite.ps1', 'hookshot-build.ps1', 'apply_patch.py', 'update.ps1', 'start-server.ps1', 'collect-logs.ps1', 'collect-logs.cmd',
    'log-mask.ps1', 'windows-checks.ps1')
$KitDocs = @(@('setup.md', 'SETUP.md'), @('known-issues.md', 'KNOWN-ISSUES.md'))
# Never in this kit (emulator kits only); make_kit_hookshot refuses a kit that has one.
$EmulatorOnly = '(?i)(^|\\)(emulator\\|lichii|fritchii|.*\.nx2recipe$|Host\.bat$|Join\.bat$|Host-Hookshot\.bat$|Join-Hookshot\.bat$|Play-OoT-Coop\.bat$|Build-Hookshot-Coop\.bat$|play\.cmd$|start-join\.cmd$|start-relay\.cmd$|make-puppet-pack\.cmd$|check-hosting\.cmd$|puppet-pack\.ps1$|qt6|Qt6|SDL2\.dll$)'

$repo = Invoke-Git $here rev-parse --show-toplevel
if (-not $repo) { throw 'tools\release is not inside a git checkout' }
$repo = [System.IO.Path]::GetFullPath($repo)
$mainCheckout = Split-Path -Parent ([System.IO.Path]::GetFullPath((Invoke-Git $repo rev-parse --path-format=absolute --git-common-dir)))
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build' }
if (-not $OutRoot) { $OutRoot = Join-Path (Split-Path -Parent $mainCheckout) 'coop-kit' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutRoot = [System.IO.Path]::GetFullPath($OutRoot)
if (($OutRoot + '\').StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "The kit must go outside the repo: $OutRoot" }

# The server and the module, both Release.
$server = @('server\coop_server.exe', 'server\Release\coop_server.exe') | ForEach-Object { Join-Path $BuildDir $_ } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $server) { throw "No coop_server.exe in $BuildDir. Build it: cmake --build build --config Release --target coop_server" }
if (Test-BinaryHas $server 'VCRUNTIME140D.dll') { throw "$server is a Debug build; build Release." }
if (-not $HookshotDll) {
    $HookshotDll = Get-ChildItem -LiteralPath $BuildDir -Filter 'hookshot_coop.dll' -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\(Debug|_deps)\\' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $HookshotDll -or -not (Test-Path -LiteralPath $HookshotDll)) { throw 'No hookshot_coop.dll: build the hookshot_coop target (Release) or pass -HookshotDll.' }
if (Test-BinaryHas $HookshotDll 'VCRUNTIME140D.dll') { throw "$HookshotDll is a Debug build; build Release." }
$hsPin = Join-Path $repo 'hookshot\PIN'
if (-not (Test-Path $hsPin)) { throw 'No hookshot\PIN in the repo.' }
$hsPatches = @(Get-ChildItem (Join-Path $repo 'hookshot\patches') -Filter '*.patch' -File | Sort-Object Name)
if (-not $hsPatches) { throw 'hookshot\patches has no .patch.' }
$pin = @{}
Get-Content $hsPin | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $pin[$Matches[1]] = $Matches[2].Trim() } }

# The Visual C++ runtime, beside the server and the module (hookshot-build.ps1 puts it beside hookshot.exe too):
# the kit refuses to be made without it, as a PC without it could not run co-op.
$crt = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    foreach ($vs in & $vswhere -all -products * -property installationPath) {
        $crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($crt) { break }
    }
}
if (-not $crt) { throw 'The Visual C++ runtime folder (VC\Redist\MSVC\...\x64\Microsoft.VC14x.CRT) was not found: install the VS build tools.' }

# fmt's licence (the module links it), from the build the module came from, then -BuildDir.
$fmtRoots = @()
for ($d = Split-Path -Parent ([System.IO.Path]::GetFullPath($HookshotDll)); $d; $d = Split-Path -Parent $d) {
    if (Test-Path -LiteralPath (Join-Path $d 'CMakeCache.txt')) { $fmtRoots += $d; break }
}
$fmtRoots += $BuildDir
$fmtLicense = $fmtRoots | ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $_ '_deps\fmt-src') -Filter 'LICENSE*' -File -ErrorAction SilentlyContinue } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $fmtLicense) { throw "fmt's LICENSE not found under _deps\fmt-src of the build that made $HookshotDll." }
$enetLicense = Join-Path $BuildDir '_deps\enet-src\LICENSE'
if (-not (Test-Path $enetLicense)) { throw "ENet's LICENSE not found in $BuildDir\_deps\enet-src." }

# Lay out the kit.
$name = if ($Release) { "oot-coop-$Release" } else { 'oot-coop-{0:yyyyMMdd-HHmm}' -f (Get-Date) }
$kit = Join-Path $OutRoot $name
if (Test-Path $kit) { throw "$kit already exists; wait a minute or remove it." }
New-Item -ItemType Directory -Force -Path $kit | Out-Null
Write-Host "Kit: $kit"
$src = Join-Path $here 'kit'
foreach ($f in $KitTop) { Copy-Text (Join-Path $src $f) (Join-Path $kit $f) }
foreach ($f in $KitScripts) { Copy-Text (Join-Path $src "scripts\$f") (Join-Path $kit "scripts\$f") }
Copy-Text (Join-Path $src 'coop.ini.template') (Join-Path $kit 'scripts\coop.ini.template')
Copy-Text (Join-Path $src 'coop-options.ini') (Join-Path $kit 'scripts\coop-options.default.ini')
Copy-Text (Join-Path $here 'verify_kit.ps1') (Join-Path $kit 'scripts\verify_kit.ps1')
& (Join-Path $here 'make_update_bat.ps1') -Out (Join-Path $kit 'Update.bat')

$srv = Join-Path $kit 'server'
New-Item -ItemType Directory -Force -Path $srv | Out-Null
Copy-Item $server $srv
$hsKit = Join-Path $kit 'hookshot'
New-Item -ItemType Directory -Force -Path (Join-Path $hsKit 'patches') | Out-Null
Copy-Item -LiteralPath $HookshotDll -Destination $hsKit
foreach ($dir in $srv, $hsKit) { Get-ChildItem $crt.FullName -Filter '*.dll' | Copy-Item -Destination $dir }
Copy-Text $hsPin (Join-Path $hsKit 'PIN')
Copy-Text (Join-Path $src 'hookshot\TOOLS.txt') (Join-Path $hsKit 'TOOLS.txt')
Copy-Text (Join-Path $src 'hookshot\OOTRR.txt') (Join-Path $hsKit 'OOTRR.txt')   # (the randomizer the build fetches)
foreach ($p in $hsPatches) { Copy-Item -LiteralPath $p.FullName -Destination (Join-Path $hsKit "patches\$($p.Name)") }   # (byte for byte)

# The puppet pack tool (hookshot-build.ps1 runs it with Hookshot's own Python): every module build.py imports.
$packTool = Join-Path $repo 'tools\puppet_pack'
# (make_kit.ps1's list: its tests and the scene-placing test helper stay out), plus every module build.py imports.
$packFiles = @('ainb.py', 'build.py', '__init__.py', 'byml.py', 'dumpcheck.py', 'dungeon_rooms.py', 'hud.py', 'nup.py', 'rstb.py',
    'sarc.py', 'texture.py', 'title_menu.py', 'tunic.py', 'tunic_extra.py', 'zs.py')
foreach ($line in (Get-Content (Join-Path $packTool 'build.py') | Where-Object { $_ -match '^\s*from \. import ' })) {
    $packFiles += (($line -replace '#.*$', '') -replace '^\s*from \. import ', '').Split(',') | ForEach-Object { "$($_.Trim()).py" } | Where-Object { $_ -ne '.py' }
}
foreach ($f in ($packFiles | Sort-Object -Unique)) {
    if (-not (Test-Path (Join-Path $packTool $f))) { throw "tools\puppet_pack\$f (needed by build.py) is not in the repo" }
    Copy-Text (Join-Path $packTool $f) (Join-Path $kit "tools\puppet_pack\$f")
}

foreach ($doc in $KitDocs) { Copy-Text (Join-Path $repo "docs\$($doc[0])") (Join-Path $kit "docs\$($doc[1])") }
$lic = Join-Path $kit 'licenses'
New-Item -ItemType Directory -Force -Path $lic, (Join-Path $kit 'logs') | Out-Null
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $lic 'oot-remake-multiplayer-GPL-3.0.txt')
Copy-Text (Join-Path $repo 'THIRD_PARTY_NOTICES.md') (Join-Path $lic 'THIRD_PARTY_NOTICES.md')
Copy-Item $enetLicense (Join-Path $lic 'ENet-MIT.txt')
Copy-Item -LiteralPath $fmtLicense (Join-Path $lic 'fmt-MIT.txt')

$commit = Invoke-Git $repo rev-parse HEAD
[System.IO.File]::WriteAllLines((Join-Path $lic 'SOURCE.txt'), [string[]]@(
    "Source code for this build$(if ($Release) { " ($Release)" })",
    '',
    'The co-op code (the module, the server, the puppet pack tool, these scripts), GPL-3.0:',
    "  https://github.com/daurosk/oot-remake-multiplayer/tree/$commit",
    "  $(if ($Release) { "(tag $Release) " })commit $commit; hookshot\hookshot_coop.dll is its CMake target hookshot_coop.",
    '',
    'Hookshot: NotRexed''s static recompilation, GPL-3.0. Not in this kit: "Start OoT Co-op.bat" downloads it and builds',
    'it on your PC from your own game:',
    "  https://github.com/NotRexed/Hookshot/tree/$($pin.commit)",
    '  with the patches in hookshot\patches (also hookshot/patches in the co-op repo), and the build tools of the',
    '  Hookshot release named in hookshot\TOOLS.txt.',
    '',
    'If a link stops working, open an issue on the co-op repo and the source will be sent to you.'))
[System.IO.File]::WriteAllLines((Join-Path $kit 'VERSION.txt'), [string[]]@(
    "kit: $name$(if ($Release) { " (release $Release)" })",
    'kind: hookshot',
    "built: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
    "co-op repo: $commit ($(Invoke-Git $repo rev-parse --abbrev-ref HEAD))$(if (Invoke-Git $repo status --porcelain --untracked-files=no) { ' with uncommitted changes' })",
    "hookshot pin: $($pin.commit)",
    "hookshot_coop.dll: $((Get-Sha $HookshotDll).Substring(0, 16)) (sha256, first 16)",
    "coop_server.exe: $((Get-Sha $server).Substring(0, 16)) (sha256, first 16)",
    "visual c++ runtime: $($crt.Name)",
    "default port: $Port"))

# Nothing of the game, of an emulator or of anyone's data, and no compiled Python.
$banned = Get-ChildItem $kit -Recurse -File | Where-Object {
    $_.Extension -in '.keys', '.nsp', '.nup', '.xci', '.nca', '.nro', '.nso', '.zs', '.pack', '.bfres', '.sarc' }
if ($banned) { throw "The kit holds files that must not ship: $(($banned | Select-Object -First 5 | ForEach-Object FullName) -join ', ')" }
$kitRoot = [System.IO.Path]::GetFullPath($kit).TrimEnd('\') + '\'
$emu = @(Get-ChildItem $kit -Recurse -Force | ForEach-Object { $_.FullName.Substring($kitRoot.Length) } | Where-Object { $_ -match $EmulatorOnly })
if ($emu) { throw "Emulator files in the Hookshot kit: $($emu -join ', ')" }
Get-ChildItem $kit -Recurse -Directory -Force -Filter '__pycache__' | Remove-Item -Recurse -Force
$hits = @(Find-UserPath $kit (Get-UserPathNeedles ([Environment]::GetFolderPath('UserProfile'))))
if ($hits) { throw "The build PC's user folder appears in: $(($hits | Select-Object -First 8 | ForEach-Object { $_.File.Substring($kitRoot.Length) }) -join ', ')" }

# MANIFEST.txt: the kit's own files (not logs\ or coop-options.ini, the player's).
$own = @(Get-ChildItem $kit -Recurse -File | ForEach-Object { $_.FullName.Substring($kitRoot.Length) } |
    Where-Object { $_ -notmatch '^(?i)(logs\\|coop-options\.ini$)' } | Sort-Object)
[System.IO.File]::WriteAllLines((Join-Path $kit 'MANIFEST.txt'), [string[]](@("# Files of $name, for Update.bat. Do not edit.", 'MANIFEST.txt') + $own))

$size = (Get-ChildItem $kit -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ('Folder: {0:N1} MB, {1} files' -f ($size / 1MB), ($own.Count + 1))
if (-not $NoZip) {
    # Two names, the same bytes: oot-coop-<tag>-windows.zip is the one players download (beside the Linux kit's
    # oot-coop-<tag>-linux.zip); oot-coop-<tag>.zip is the name every updater up to v0.10.1 looks for (its
    # Select-KitRelease takes nothing else), so those kits still find and install this release. Both go up.
    $zip = Join-Path $OutRoot "$name-windows.zip"
    $old = "$kit.zip"
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $OutRoot $name
    if ($LASTEXITCODE -ne 0) { throw "Zipping failed (tar $LASTEXITCODE)." }
    Copy-Item -LiteralPath $zip -Destination $old -Force
    $hash = Get-Sha $zip
    if ((Get-Sha $old) -ne $hash) { throw "The copy $old does not match $zip." }
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  $name-windows.zip`n", [System.Text.Encoding]::ASCII)
    [System.IO.File]::WriteAllText("$old.sha256", "$hash  $name.zip`n", [System.Text.Encoding]::ASCII)
    Copy-Item -LiteralPath (Join-Path $kit 'Update.bat') -Destination (Join-Path $OutRoot 'Update.bat') -Force
    Write-Host ('Zip: {0} ({1:N1} MB), SHA-256 {2}' -f $zip, ((Get-Item $zip).Length / 1MB), $hash)
    Write-Host "Release assets: $name-windows.zip and $name.zip (the same file, the name kits up to v0.10.1 update from), each with its .sha256, and Update.bat, all in $OutRoot"
}
