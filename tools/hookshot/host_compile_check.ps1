# Compiles the Longshot files our patches touch (host/*.c, gpu/vk/*.cpp, and the files that include a header a patch
# changes) for Windows, syntax only, with Longshot's own compiler and nx2/build.py's own flags (as the kit's game build
# runs them): no game, no link, a few minutes. The CI windows job runs it, so a patch whose Windows-only code does not
# compile (0.11.4's patch 0025: a '\\' that lost a backslash) fails there and never reaches a game build. The Linux
# side is tools\hookshot\host_compile_check.sh (the linux-longshot job).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\hookshot\host_compile_check.ps1 [-Cache <folder>] [-Work <folder>]
#
# 1. Longshot's tools.lock of the release in tools\release\kit\hookshot\TOOLS.txt, its windows-x64 llvm-mingw, Python
#    and CPM cache (the Vulkan, SPIR-V and VMA headers), and the source archive of hookshot\PIN's commit: from the co-op
#    mirror first, then Longshot's servers, each checked against its SHA-256 (the kit's Get-KitDownload). -Cache keeps
#    the downloads.
# 2. Laid out as the kit's scripts\hookshot-build.ps1 lays them out (the source, each tool in its tools.lock folder), and
#    hookshot\patches\*.patch applied with the kit's own apply_patch.py.
# 3. tools\hookshot\host_compile_check.py with Longshot's Python: nx2/build.py --standalone --dry-run gives the compiles,
#    and the ones our patches concern are compiled with -fsyntax-only. Also every .py a patch changes (py_compile).
# 4. tools\hookshot\sdl_audio_once_test.py: patch 0033's microphones run against a stand-in SDL3.dll (tracker row 156).
# 5. tools\hookshot\coop_save_remount_test.py: patch 0008's save folder switches on Longshot 3.1's staged saves, through
#    Longshot's own nn::fs harness (tests/test_fs_save_commit.py): own, guest, own, guest, multiplayer saves.
# Exit status 0 only when every file compiled and those runs passed.
param(
    [string]$Cache = '',
    [string]$Work = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$kitScripts = Join-Path $repo 'tools\release\kit\scripts'
. (Join-Path $kitScripts 'kit-paths.ps1')   # (Get-KitDownload, Set-KitTls)
Set-KitTls
function Fail([string]$message) { Write-Host "host_compile_check: $message"; exit 1 }
function Read-KeyFile([string]$Path) {
    $map = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $map[$Matches[1]] = $Matches[2] }
    }
    return $map
}
function Expand-Zip([string]$Zip, [string]$To, [int]$Strip = 0, [string]$Member = '') {
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    $tarArgs = @('-x', '-f', $Zip, '-C', $To)
    if ($Member) { $Strip = @($Member -split '/').Count }
    if ($Strip) { $tarArgs += "--strip-components=$Strip" }
    if ($Member) { $tarArgs += "$Member/" }
    & "$env:SystemRoot\System32\tar.exe" @tarArgs
    if ($LASTEXITCODE -ne 0) { Fail "unpacking $(Split-Path -Leaf $Zip) failed (tar $LASTEXITCODE)" }
}
function Get-Pinned([string[]]$Urls, [string]$To, [string]$Sha256) {
    $why = Get-KitDownload $Urls $To $Sha256 @()
    if ($why) { Fail "download of $(Split-Path -Leaf $To) failed: $why" }
}

$temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
if (-not $Cache) { $Cache = Join-Path $temp 'longshot-dl' }
if (-not $Work) { $Work = Join-Path $temp 'longshot-host-check' }
New-Item -ItemType Directory -Force -Path $Cache | Out-Null

$pin = Read-KeyFile (Join-Path $repo 'hookshot\PIN')
$toolsPin = Read-KeyFile (Join-Path $repo 'tools\release\kit\hookshot\TOOLS.txt')
if ($toolsPin.source_commit -ne $pin.commit) { Fail "TOOLS.txt pins the source of $($toolsPin.source_commit), hookshot\PIN says $($pin.commit)" }
if ($pin.repo -notmatch '^(https://git\.magicbean\.dev)/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(\.git)?$') { Fail "hookshot\PIN: repo=$($pin.repo) is not a git.magicbean.dev repository" }
$repoUrl = "$($Matches[1])/$($Matches[2])"
$apiUrl = "$($Matches[1])/api/v1/repos/$($Matches[2])"
$mirror = "$($toolsPin.mirror)".TrimEnd('/')
$patchDir = Join-Path $repo 'hookshot\patches'
$patches = @(Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' -File | Sort-Object Name)
if (-not $patches) { Fail 'no hookshot\patches\*.patch' }

# --- 1: tools.lock, the tools, the source -----------------------------------------------------
$lock = Join-Path $Cache "tools-$($toolsPin.release).lock"
Get-Pinned @("$mirror/tools.lock", "$repoUrl/releases/download/$($toolsPin.release)/tools.lock") $lock $toolsPin.lock_sha256
$wanted = @('llvm-mingw', 'python', 'cpm')
$parts = @{}
foreach ($line in Get-Content -LiteralPath $lock) {
    $f = $line.Trim() -split '\s+'
    if ($f[0] -ne 'tool' -or $f.Count -lt 9 -or $f[3] -ne 'windows-x64' -or $wanted -notcontains $f[1]) { continue }
    if ($f[6] -notmatch '^LongshotBuilder/tools/([A-Za-z0-9_.-]+)$') { Fail "tools.lock puts $($f[1]) outside LongshotBuilder/tools: $($f[6])" }
    $folder = $Matches[1]
    $sha = $f[4].ToLowerInvariant()
    $urls = @("$mirror/$sha-$($f[1]).zip") + @($f[8..($f.Count - 1)] | Where-Object { $_ -match '^https://' })
    $parts[$f[1]] = [pscustomobject]@{ Sha = $sha; Folder = $folder; Prefix = $f[7]; Urls = $urls
                                       File = (Join-Path $Cache "$($f[1])-$($sha.Substring(0, 12)).zip") }
}
foreach ($n in $wanted) {
    if (-not $parts[$n]) { Fail "tools.lock of $($toolsPin.release) has no windows-x64 $n" }
    Get-Pinned $parts[$n].Urls $parts[$n].File $parts[$n].Sha
}
$sourceZip = Join-Path $Cache "longshot-$($pin.commit).zip"
Get-Pinned @("$mirror/longshot-$($pin.commit).zip", "$apiUrl/archive/$($pin.commit).zip") $sourceZip $toolsPin.source_sha256

# --- 2: the patched source with its tools, as the kit lays them out ------------------------------
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
$src = Join-Path $Work 'tools'
Write-Host '  unpacking ...'
Expand-Zip $sourceZip $src 1
foreach ($n in $wanted) {
    $member = if ($parts[$n].Prefix -eq '-') { '' } else { $parts[$n].Prefix.TrimEnd('/') }
    Expand-Zip $parts[$n].File (Join-Path $src $parts[$n].Folder) -Member $member
}
$python = Join-Path $src "$($parts['python'].Folder)\python.exe"
foreach ($p in $python, (Join-Path $src 'nx2\build.py')) { if (-not (Test-Path -LiteralPath $p)) { Fail "no $p after unpacking" } }
Write-Host "  applying $($patches.Count) co-op patch(es) ..."
& $python -I -B (Join-Path $kitScripts 'apply_patch.py') $src @($patches | ForEach-Object FullName)
if ($LASTEXITCODE -ne 0) { Fail "the co-op patches do not apply to Longshot $($pin.commit)" }

# --- 3: the compiles ------------------------------------------------------------------------
# (the kit's build environment: scripts\hookshot-build.ps1 sets these two; OOT_WORK is the work folder build.py writes rt64 in)
$env:RX_CPM = Join-Path $src $parts['cpm'].Folder
$env:LONGSHOT_MINGW = Join-Path $src $parts['llvm-mingw'].Folder
$env:OOT_WORK = Join-Path $Work 'work'
New-Item -ItemType Directory -Force -Path $env:OOT_WORK | Out-Null
& $python -I -B (Join-Path $PSScriptRoot 'host_compile_check.py') $src $patchDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# --- 4: patch 0033 run (tracker row 156): host/hos_sdl.c built against a stand-in SDL3.dll and 1000 rounds of
# microphone lists, opens and reads, with SDL's audio started once and the open stream valid all along; without 0033
# (reversed) the same rounds must restart SDL's audio and use a freed stream, as the players' games did
& $python -I -B (Join-Path $PSScriptRoot 'sdl_audio_once_test.py') $src $patchDir $env:LONGSHOT_MINGW
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# --- 5: patch 0008 on staged saves (review-01111-b): every save folder stages in its own work copy and is published only
# into itself; back on the player's own folder after a session as a guest, the own save is written, not the co-op one
& $python -I -B (Join-Path $PSScriptRoot 'coop_save_remount_test.py') $src $env:LONGSHOT_MINGW
exit $LASTEXITCODE
