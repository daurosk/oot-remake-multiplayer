# Gets the Longshot files the kits download (the pins in tools/release/kit/hookshot/TOOLS.txt, hookshot/PIN and
# hookshot/LINUX.txt), checks every one against its pinned SHA-256, adds the licence texts they lack, README.md
# (each file's licences and source) and SHA256SUMS, and prints the gh commands that publish them as the co-op
# mirror: a pre-release of daurosk/oot-remake-multiplayer tagged longshot-mirror-<release>, which the kits try
# after Longshot's own servers
# (kit-mirror, 0.11.1). It publishes nothing itself.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\mirror_longshot.ps1 [-Out <folder>] [-Feed <url>]
#
# -Feed: get the files from there (<feed>/<asset name>, e.g. an existing mirror, to check it) instead of Longshot's.
# Exits 1 when a file cannot be had with the right SHA-256.
param(
    [string]$Out = '',
    [string]$Feed = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'kit\scripts\kit-paths.ps1')   # (Get-KitDownload, Set-KitTls)
Set-KitTls

function Read-Pin([string]$Path) {
    $map = @{}
    foreach ($line in Get-Content -LiteralPath $Path) { if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $map[$Matches[1]] = $Matches[2] } }
    $map
}
function Stop-Mirror([string]$Text) { Write-Host "mirror_longshot: $Text" -ForegroundColor Red; exit 1 }

$tp = Read-Pin (Join-Path $PSScriptRoot 'kit\hookshot\TOOLS.txt')
$pin = Read-Pin (Join-Path $repo 'hookshot\PIN')
$lx = Read-Pin (Join-Path $repo 'hookshot\LINUX.txt')
foreach ($k in 'release', 'lock_sha256', 'source_commit', 'source_sha256', 'mirror') { if (-not $tp[$k]) { Stop-Mirror "TOOLS.txt has no $k=." } }
if ($tp.source_commit -ne $pin.commit) { Stop-Mirror "TOOLS.txt's source_commit ($($tp.source_commit)) is not hookshot/PIN's commit ($($pin.commit))." }
if ($lx.tag -ne $tp.release) { Stop-Mirror "hookshot/LINUX.txt (tag $($lx.tag)) and TOOLS.txt (release $($tp.release)) name different Longshot releases." }
$tag = "longshot-mirror-$($tp.release)"
foreach ($m in $tp.mirror, $lx.mirror) {
    if ($m -ne "https://github.com/daurosk/oot-remake-multiplayer/releases/download/$tag") { Stop-Mirror "mirror= ($m) is not this release's mirror tag ($tag) in TOOLS.txt and LINUX.txt." }
}
if ($pin.repo -notmatch '^https://git\.magicbean\.dev/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { Stop-Mirror "hookshot/PIN repo= is not on git.magicbean.dev." }
$up = $pin.repo
$api = $up -replace '^(https://[^/]+)/', '$1/api/v1/repos/'
if (-not $Out) { $Out = Join-Path ([System.IO.Path]::GetTempPath()) $tag }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$Feed = $Feed.TrimEnd('/')

$assets = @()   # name, sha, what
function Get-Asset([string]$Name, [string]$Sha, [string[]]$Urls, [string]$What) {
    if ($Feed) { $Urls = @("$Feed/$Name") }
    $why = Get-KitDownload $Urls (Join-Path $Out $Name) $Sha
    if ($why) { Stop-Mirror "$Name ($What): $why" }
    $script:assets += [pscustomobject]@{ Name = $Name; Sha = $Sha.ToLowerInvariant(); What = $What }
}

# tools.lock, then the tools in it the Windows kit uses (hookshot-build.ps1's list), named <sha256>-<name>.zip
Get-Asset 'tools.lock' $tp.lock_sha256 @("$up/releases/download/$($tp.release)/tools.lock") "Longshot $($tp.release)'s tools.lock"
$wanted = @('llvm-mingw', 'python', 'cpm', 'third-party')
$n = 0
foreach ($line in Get-Content -LiteralPath (Join-Path $Out 'tools.lock')) {
    $f = $line.Trim() -split '\s+'
    if ($f[0] -ne 'tool' -or $f.Count -lt 9 -or $f[3] -ne 'windows-x64' -or $wanted -notcontains $f[1]) { continue }
    Get-Asset "$($f[4].ToLowerInvariant())-$($f[1]).zip" $f[4] @($f[8..($f.Count - 1)] | Where-Object { $_ -match '^https://' }) "$($f[1]) $($f[2]) windows-x64, as tools.lock lists it"
    $n++
}
if ($n -ne $wanted.Count) { Stop-Mirror "tools.lock lists $n of the $($wanted.Count) windows-x64 tools the kit needs." }
Get-Asset "longshot-$($pin.commit).zip" $tp.source_sha256 @("$api/archive/$($pin.commit).zip", "$up/archive/$($pin.commit).zip") "Longshot's source at $($pin.commit) (tag $($tp.release))"
Get-Asset $lx.appimage $lx.sha256 @($lx.url) "Longshot $($lx.tag)'s Linux builder"


# --- Licences: every mirrored file names its licence(s) and source ---------------------------------------------------
# What each file is under, written for this pin from what the files hold (review-kit-mirror, 2026-11-15). Inside: the
# licence texts the file itself carries (zip entry regexes; each must be there, so a new pin that drops one stops
# here). Texts: licence files the file lacks, given beside it as mirror assets: from another mirrored zip (From, Entry)
# or, the LGPL, from FFmpeg's own tag, checked against a pinned SHA-256.
$tar = "$env:SystemRoot\System32\tar.exe"
$srcZip = "longshot-$($pin.commit).zip"
# The source of every GPL/LGPL part in the mirrored files (we redistribute those binaries, so we carry their source),
# each the exact version the binary is built from. Pin: where the SHA-256 comes from (upstream's own where it has one;
# GitHub's archive of a commit has none, so the bytes fetched on 2026-11-15 are pinned).
$sources = @(
    @{ Name = 'busybox-w32-1f493261d16be3d984fe8a689f5113dafb6eaaa7.tar.gz'; Sha = '84124d4c9f4439f254f4b74a44397a7e354277fe1f9677591377a5143da3d248'
       Url = 'https://github.com/rmyorston/busybox-w32/archive/1f493261d16be3d984fe8a689f5113dafb6eaaa7.tar.gz'
       For = 'llvm-mingw zip: ``busybox/`` (busybox-w32, GPL-2.0; llvm-mingw''s build-busybox.sh pins this commit, unpatched)'; Pin = 'GitHub archive of the commit; no upstream SHA-256, pinned here' }
    @{ Name = 'make-4.4.1.tar.gz'; Sha = 'dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3'
       Url = 'https://ftpmirror.gnu.org/gnu/make/make-4.4.1.tar.gz'
       For = 'llvm-mingw zip: ``bin/mingw32-make.exe`` and ``busybox/bin/make.exe`` (GNU make 4.4.1, GPL-3.0; build-make.sh, unpatched)'; Pin = 'GNU''s release tarball (signed: make-4.4.1.tar.gz.sig beside it on ftp.gnu.org)' }
    @{ Name = 'mingw-w64-ee9bd87644156c41072a11ad957d1e11831a72df.tar.gz'; Sha = '6a346a2e3eafa0e5ec8e30df89b16a05add8b50cb4cece8c47593799ca19e43b'
       Url = 'https://github.com/mingw-w64/mingw-w64/archive/ee9bd87644156c41072a11ad957d1e11831a72df.tar.gz'
       For = 'llvm-mingw zip: ``bin/gendef.exe`` (GPL-3.0), ``bin/*widl.exe`` (LGPL-2.1, from Wine), the mingw-w64 headers and runtime (build-mingw-w64.sh pins this commit, unpatched)'; Pin = 'GitHub archive of the commit; no upstream SHA-256, pinned here' }
    @{ Name = 'ffmpeg-9.0.2.tar.xz'; Sha = '8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e'
       Url = 'https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz'
       For = 'cpm zip: ``ffmpeg/9.0.2-*-946fcce07b-*`` static libraries (LGPL-2.1-or-later; patch key None; their configure line is in avutil.lib)'; Pin = 'ffmpeg.org''s release tarball, the SHA-256 Longshot''s own scripts/linux/deps.sh pins' }
    @{ Name = 'unicorn-2.1.4.tar.gz'; Sha = '00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff'
       Url = 'https://files.pythonhosted.org/packages/b2/1b/b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/unicorn-2.1.4.tar.gz'
       For = 'python zip and the AppImage: ``unicorn`` 2.1.4 (``unicorn.dll`` / ``libunicorn.so.2``, built from QEMU: GPL-2.0)'; Pin = 'PyPI''s sdist, the SHA-256 PyPI lists' }
    @{ Name = 'fuse-3.15.0.tar.xz'; Sha = '70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0'
       Url = 'https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz'
       For = 'the AppImage''s runtime (the program in front of its image): libfuse 3.15.0, LGPL-2.1, linked in statically with type2-runtime''s patch'; Pin = 'libfuse''s release tarball, the SHA-256 type2-runtime''s install-dependencies.sh pins' }
    @{ Name = 'type2-runtime-20251108.tar.gz'; Sha = '4c4f6df4647c9f01f871d7edd3716d8aeeffda9d22ffbebe3fccd95a6ab52c95'
       Url = 'https://github.com/AppImage/type2-runtime/archive/refs/tags/20251108.tar.gz'
       For = 'the AppImage''s runtime: AppImage type2-runtime 20251108 (MIT; Longshot''s deps.sh pins this release), with its libfuse patch and build scripts'; Pin = 'GitHub archive of the tag; no upstream SHA-256, pinned here' }
)
foreach ($src in $sources) {
    $why = Get-KitDownload @($(if ($Feed) { "$Feed/$($src.Name)" } else { $src.Url })) (Join-Path $Out $src.Name) $src.Sha
    if ($why) { Stop-Mirror "$($src.Name) (source): $why" }
}
$texts = @(
    @{ Name = 'LICENSE'; From = $srcZip; Entry = '^[^/]+/LICENSE$' }
    @{ Name = 'LICENSE-llvm-Apache-2.0-with-LLVM-exception.txt'; Kind = 'llvm-mingw'; Entry = '^llvm-mingw-[^/]+/LICENSE\.TXT$' }
    @{ Name = 'LICENSE-sdl-zlib.txt'; From = $srcZip; Entry = '^[^/]+/third_party/sdl2/LICENSE\.txt$' }
    @{ Name = 'LICENSE-zbic-GPL-2.0.txt'; From = $srcZip; Entry = '^[^/]+/third_party/zbic/LICENSE$' }
    @{ Name = 'NOTICE-a64-kit.md'; From = $srcZip; Entry = '^[^/]+/third_party/a64-kit/NOTICE\.TOTKPort\.md$' }
    @{ Name = 'LICENSE-unicorn-GPL-2.0.txt'; From = 'unicorn-2.1.4.tar.gz'; Entry = '^unicorn-2\.1\.4/src/qemu/COPYING$' }
    @{ Name = 'LICENSE-libfuse-LGPL-2.1.txt'; From = 'fuse-3.15.0.tar.xz'; Entry = '^fuse-3\.15\.0/LGPL2\.txt$' }
    @{ Name = 'LICENSE-ffmpeg-LGPL-2.1.txt'; Url = 'https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.2/COPYING.LGPLv2.1'
       Sha = '246041b6ecf9bc32d718a62c57877c78b5eb397b6467e74ed7ae2626ab189c30' }
)
$lsLink = "$up/src/commit/$($pin.commit)"
$info = @{
    'tools.lock'  = @{ Licence = "Longshot's release file: GPL-3.0 (``LICENSE``)."; Source = "$up/releases/tag/$($tp.release)"; Inside = @() }
    'llvm-mingw'  = @{ Licence = 'llvm-mingw by Martin Storsjo. LLVM (clang, lld, ...): Apache-2.0 WITH LLVM-exception (``LICENSE.TXT`` inside, also ``LICENSE-llvm-Apache-2.0-with-LLVM-exception.txt`` here). mingw-w64 headers and runtime: their own licences (``*/share/mingw32/COPYING*`` inside). busybox-w32 (``busybox/``): GPL-2.0 (``busybox/LICENSE.txt`` inside). GNU make 4.4.1 (``bin/mingw32-make.exe``, ``busybox/bin/make.exe``): GPL-3.0 (``share/make/COPYING.txt`` inside). gendef: GPL-3.0 (``share/gendef/COPYING.txt`` inside). widl: LGPL-2.1 (``share/widl/COPYING.txt`` inside). Python (``python/``, with libffi): PSF-2.0 (``python/LICENSE.txt`` inside). Source of the GPL/LGPL parts: the busybox-w32, make and mingw-w64 tarballs here.'
                       Source = 'https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104 (the same zip, unchanged); its build scripts at that tag pin busybox-w32 1f493261d16b (its version string: v1.38.0.git-5894-g1f493261d), GNU make 4.4.1 and mingw-w64 ee9bd8764415 (all three here as source)'
                       Inside = @('^[^/]+/LICENSE\.TXT$', '/share/mingw32/COPYING', '/busybox/LICENSE\.txt$', '/python/LICENSE\.txt$', '^[^/]+/share/make/COPYING\.txt$', '^[^/]+/share/gendef/COPYING\.txt$', '^[^/]+/share/widl/COPYING\.txt$', '/busybox/bin/busybox\.exe$') }
    'python'      = @{ Licence = 'CPython 3.14, PSF-2.0 (``LICENSE.txt`` inside); the packages in ``Lib/site-packages`` under their own licences (their ``*.dist-info`` folders, inside), except: unicorn 2.1.4, whose ``unicorn.dll`` is GPL-2.0 (the wheel says BSD for its Python binding and carries no text: ``LICENSE-unicorn-GPL-2.0.txt`` here, source ``unicorn-2.1.4.tar.gz`` here); numpy''s OpenBLAS DLL holds GCC''s libgfortran, GPL-3.0-or-later WITH GCC-exception-3.1 (numpy''s LICENSE.txt inside), which asks for no source.'
                       Source = 'https://www.python.org/downloads/source/ (CPython 3.14); this build: Hookshot''s builder parts, https://github.com/NotRexed/Hookshot/releases/tag/builder-parts'
                       Inside = @('/python/LICENSE\.txt$', '\.dist-info/', '/site-packages/unicorn/lib/unicorn\.dll$', '/site-packages/unicorn-2\.1\.4\.dist-info/') }
    'cpm'         = @{ Licence = 'Libraries for the game''s build: dav1d 1.5.4, BSD-2-Clause (``dav1d/*/COPYING`` inside); SPIRV-Headers, MIT-style (``LICENSE`` inside); Vulkan-Headers 1.4.345, Apache-2.0 OR MIT (``LICENSE.md`` inside); VulkanMemoryAllocator 3.3.0, MIT (``LICENSE.txt`` inside); FFmpeg 9.0.2 static libraries, LGPL-2.1-or-later (configured without ``--enable-gpl``; no text inside, so ``LICENSE-ffmpeg-LGPL-2.1.txt`` here; source ``ffmpeg-9.0.2.tar.xz`` here).'
                       Source = 'https://code.videolan.org/videolan/dav1d (1.5.4); https://github.com/KhronosGroup/SPIRV-Headers (vulkan-sdk-1.4.341.0); https://github.com/KhronosGroup/Vulkan-Headers (v1.4.345); https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator (v3.3.0); FFmpeg: https://github.com/FFmpeg/FFmpeg/tree/n9.0.2 (commit 946fcce07b, as the folder name says)'
                       Inside = @('/dav1d/[^/]+/COPYING$', '/spirv-headers/[^/]+/LICENSE$', '/vulkanheaders/[^/]+/LICENSE\.md$', '/vulkanmemoryallocator/[^/]+/LICENSE\.txt$', '/ffmpeg/9\.0\.2-[^/]*-946fcce07b-') }
    'third-party' = @{ Licence = 'Longshot''s prebuilt helpers (no licence text inside; given here): ``a64-kit/lift/a64emit.dll``, Longshot''s a64-kit lifter, GPL (its suyu translator is GPL-2.0-or-later; ``NOTICE-a64-kit.md`` here); ``a64-kit/tools/lld-link.exe``, LLVM''s lld 24.0.0git, Apache-2.0 WITH LLVM-exception (``LICENSE-llvm-Apache-2.0-with-LLVM-exception.txt`` here); ``sdl2/SDL2.dll`` and ``SDL3.dll``, sdl2-compat 2.32.74 and SDL 3.4.20, zlib (``LICENSE-sdl-zlib.txt`` here); ``zbic/zbic.dll``, zstd with Atmosphere-NX''s ZBIC parts, GPL-2.0 (``LICENSE-zbic-GPL-2.0.txt`` here).'
                       Source = "Longshot's source (``$srcZip`` here): ``third_party/a64-kit``, ``third_party/sdl2/PROVENANCE.md``, ``third_party/zbic``; lld: https://github.com/llvm/llvm-project at 510126255f89d693717c1ce7105b593f995f07c1 (its version string); SDL: https://github.com/libsdl-org/sdl2-compat (release-2.32.74), https://github.com/libsdl-org/SDL (release-3.4.20)"
                       Inside = @('/a64-kit/lift/a64emit\.dll$', '/a64-kit/tools/lld-link\.exe$', '/sdl2/SDL2\.dll$', '/sdl2/SDL3\.dll$', '/zbic/zbic\.dll$') }
    'source'      = @{ Licence = 'Longshot, GPL-3.0 (``LICENSE`` inside and here); the vendored parts under ``third_party/`` and ``gpu/`` keep their own notices inside.'
                       Source = "$lsLink (tag $($tp.release)); the same bytes as $api/archive/$($pin.commit).zip"; Inside = @('^[^/]+/LICENSE$') }
    'appimage'    = @{ Licence = "Longshot $($lx.tag)'s Linux builder, GPL-3.0 (``LICENSE`` here and in ``usr/share/longshot``); the licences of what is compiled into ``longshot_vk.so`` (Dear ImGui MIT, zstd BSD, Libertinus OFL, FidelityFX MIT) in ``usr/share/longshot/licenses``. It bundles CPython 3.12.16 from python-build-standalone 20261026 (PSF-2.0, ``usr/python/lib/python3.12/LICENSE.txt``; built with libedit, not readline, and without gdbm; its ``_dbm`` holds Berkeley DB 6.0.19 under the Sleepycat licence, source https://ftp.osuosl.org/pub/blfs/conglomeration/db/db-6.0.19.tar.gz, SHA-256 2917c28f60903908c2ca4587ded1363b812c4e830a5326aaa77c9879d13ae18e as python-build-standalone pins it) and the packages of its ``site-packages`` (their ``*.dist-info``), unicorn 2.1.4 (GPL-2.0) among them: ``LICENSE-unicorn-GPL-2.0.txt`` and ``unicorn-2.1.4.tar.gz`` here. Its runtime (the program in front of the image) is AppImage's type2-runtime 20251108, MIT, with libfuse 3.15.0 linked in, LGPL-2.1: ``LICENSE-libfuse-LGPL-2.1.txt``, ``type2-runtime-20251108.tar.gz`` and ``fuse-3.15.0.tar.xz`` here."
                       Source = "``$srcZip`` here (the release tag's commit, $($pin.commit)), whose scripts/linux/deps.sh and appimage.sh make it; $up/releases/tag/$($lx.tag); CPython: https://github.com/astral-sh/python-build-standalone/releases/tag/20261026"; Inside = @() }
}
function Get-AssetKind([string]$Name) {
    if ($Name -eq 'tools.lock') { return 'tools.lock' }
    if ($Name -eq $srcZip) { return 'source' }
    if ($Name -eq $lx.appimage) { return 'appimage' }
    if ($Name -match '^[0-9a-f]{64}-(.+)\.zip$') { return $Matches[1] }
    return ''
}
$entries = @{}
foreach ($a in $assets) {
    $k = Get-AssetKind $a.Name
    if (-not $info[$k]) { Stop-Mirror "$($a.Name): no licence and source written for '$k' in mirror_longshot.ps1. Look inside it and add them." }
    if ($a.Name -like '*.zip') { $entries[$a.Name] = @(& $tar -t -f (Join-Path $Out $a.Name)) }
    foreach ($re in $info[$k].Inside) {
        if (-not @($entries[$a.Name] | Where-Object { $_ -match $re })) { Stop-Mirror "$($a.Name) has no entry matching '$re': its licence line in mirror_longshot.ps1 is out of date." }
    }
}
$tmp = Join-Path $Out 'license.tmp'
foreach ($t in $texts) {
    $dest = Join-Path $Out $t.Name
    if ($t.Url) {
        $why = Get-KitDownload @($(if ($Feed) { "$Feed/$($t.Name)" } else { $t.Url })) $dest $t.Sha
        if ($why) { Stop-Mirror "$($t.Name): $why" }
    } else {
        $from = if ($t.From) { $t.From } else { @($assets | Where-Object { (Get-AssetKind $_.Name) -eq $t.Kind })[0].Name }
        if (-not $entries[$from]) { $entries[$from] = @(& $tar -t -f (Join-Path $Out $from)) }
        $entry = @($entries[$from] | Where-Object { $_ -match $t.Entry }) | Select-Object -First 1
        if (-not $entry) { Stop-Mirror "$from has no entry matching '$($t.Entry)' (for $($t.Name))." }
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        & $tar -x -f (Join-Path $Out $from) -C $tmp $entry
        if ($LASTEXITCODE -ne 0) { Stop-Mirror "Could not take $entry out of $from." }
        Move-Item -LiteralPath (Join-Path $tmp ($entry -replace '/', '\')) -Destination $dest -Force
        Remove-Item -LiteralPath $tmp -Recurse -Force
    }
    $t.Sha256 = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLowerInvariant()
}

$utf8 = [System.Text.UTF8Encoding]::new($false)
$rows = ($assets | ForEach-Object { "| ``$($_.Name)`` | $($_.What) | ``$($_.Sha)`` |" }) -join "`n"
$each = ($assets | ForEach-Object {
    $i = $info[(Get-AssetKind $_.Name)]
    "### ``$($_.Name)```n`n- Licence: $($i.Licence)`n- Source: $($i.Source)"
}) -join "`n`n"
$textRows = ($texts | ForEach-Object { "| ``$($_.Name)`` | ``$($_.Sha256)`` |" }) -join "`n"
$srcRows = ($sources | ForEach-Object { "| ``$($_.Name)`` | $($_.For) | $($_.Url) | $($_.Pin) | ``$($_.Sha)`` |" }) -join "`n"
$readme = @"
# Longshot $($tp.release): co-op mirror

Exact copies of the files the OoT co-op kits download from Longshot's own servers, for players whose downloads from
git.magicbean.dev fail. The kits try Longshot's servers first and this mirror after them, and check every file against
the SHA-256 the kit pins (below), so a file here that differed would never be used. The files are unchanged.

None of these files are ours. Longshot is by Team MagicBean, GPL-3.0: $up (release $($tp.release), commit
$($pin.commit)). Each file's licences and source are listed below; licence texts a file does not carry itself are
given beside it here.

| File | What | SHA-256 |
| --- | --- | --- |
$rows

## Licences and sources

$each

## Source of the GPL and LGPL parts

These binaries are redistributed unchanged; their corresponding source, the exact version each is built from, is here too.

| File | Source of | From | SHA-256 pinned by | SHA-256 |
| --- | --- | --- | --- | --- |
$srcRows

## Licence texts given here

| File | SHA-256 |
| --- | --- |
$textRows
"@
[System.IO.File]::WriteAllText((Join-Path $Out 'README.md'), $readme.Replace("`r`n", "`n").Replace('``', '`'), $utf8)
$all = @($assets | ForEach-Object { "$($_.Sha)  $($_.Name)" }) + @($sources | ForEach-Object { "$($_.Sha)  $($_.Name)" }) + @($texts | ForEach-Object { "$($_.Sha256)  $($_.Name)" })
[System.IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS'), (($all -join "`n") + "`n"), $utf8)

$files = @($assets.Name) + @($sources | ForEach-Object { $_.Name }) + @($texts | ForEach-Object { $_.Name }) + 'README.md', 'SHA256SUMS'
$args1 = ($files | ForEach-Object { "`"$Out\$_`"" }) -join ' '
Write-Host ''
Write-Host "All $($assets.Count) files are in $Out and match their pins, with $($sources.Count) source tarballs, $($texts.Count) licence texts, README.md and SHA256SUMS." -ForegroundColor Green
Write-Host 'To publish the mirror (as daurosk; outward-facing, so only after review). A pre-release that is never "latest":'
Write-Host "  & 'C:\Program Files\GitHub CLI\gh.exe' release create $tag --repo daurosk/oot-remake-multiplayer --target main --prerelease --latest=false --title `"Longshot $($tp.release) mirror (for the co-op kits)`" --notes-file `"$Out\README.md`" $args1"
Write-Host 'Then check it from the mirror:'
Write-Host "  powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\mirror_longshot.ps1 -Out `"$Out-check`" -Feed $($tp.mirror)"
