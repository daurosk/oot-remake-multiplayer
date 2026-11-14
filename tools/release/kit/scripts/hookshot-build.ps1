# Builds a co-op Longshot game on this PC from your own copy of the game, then puts co-op in it.
# Build-Hookshot-Coop.bat runs this. Longshot (git.magicbean.dev/MagicBeanDev/Longshot, Team MagicBean, GPL-3.0; it continues Hookshot) is a static
# recompilation of the game: it is built here, from your files, and never leaves this PC.
#
#   Build-Hookshot-Coop.bat                         finds your .nup (as make-puppet-pack.cmd does), else asks
#   Build-Hookshot-Coop.bat -Game <your .nup>       no questions
#   Build-Hookshot-Coop.bat -Game <romfs folder> -Exefs <exefs folder>
#   Build-Hookshot-Coop.bat -Out <folder>           where the game goes (default: hookshot\game in this kit)
#   Build-Hookshot-Coop.bat -Language en-US         the game's language (Hookshot's codes; default en-US)
#   Build-Hookshot-Coop.bat -Rebuild                build again even when nothing changed
#   Build-Hookshot-Coop.bat -Jobs <n>               at most n compilers at once
#   Build-Hookshot-Coop.bat -CheckOnly              get the tools and check your game files; build nothing
#   (for tools\hookshot\hs.py) -Tools <folder> -Work <folder>: Hookshot's tools and source, and its work folder,
#   somewhere else than this kit's hookshot\ (one of each per pin, shared by every kit: hs-build-speed)
#
# What it does:
# 1. Hookshot's own build tools: the parts of the Hookshot release named in hookshot\TOOLS.txt (its
#    Python, llvm-mingw, the Vulkan/FFmpeg headers, wgpu-native and the prebuilt third-party files),
#    downloaded from Hookshot's GitHub release and checked against the SHA-256 in that release's
#    hookshot-setup.txt (whose own SHA-256 is in TOOLS.txt). About 400 MB, once.
# 2. Hookshot's source at the commit in hookshot\PIN (GitHub's source archive), with our patches
#    from hookshot\patches applied (scripts\apply_patch.py: no git needed).
# 3. Hookshot's own builder, nx2\make_game.py, on your .nup (or romfs + exefs): 10 to 45 minutes and
#    about 60 GB free while it runs. Its work folder is hookshot\work in this kit (kept: the next
#    build after a co-op update compiles only what changed).
# 4. Co-op into the game folder: hookshot_coop.dll beside longshot.exe, coop.ini (server and name,
#    from this kit's emulator\user\config\coop.ini when you have one), coop-options.ini, and the
#    puppet pack (built from your game) in the game's mods folder.
# Steps 1 to 3 are skipped when the tools, pin and patches are the ones the game was built with,
# so after Update.bat a run only does step 4 (a few minutes) unless the update moved the pin.
# When only the patches changed, step 2 puts the files the old ones changed back and applies the new
# ones (apply_patch.py --refresh), and step 3 is Hookshot's own incremental build: no new translation
# unless a lift file changed, and only the changed files compiled.
#
# Nothing in the kit holds the game: the tools are Hookshot's, the patches and the module are ours.
param(
    [string]$Game = '',
    [string]$Exefs = '',
    [string]$Out = '',
    [ValidatePattern('^[A-Za-z]{2}(-[A-Za-z]{2})?$')][string]$Language = 'en-US',
    [ValidateRange(0, 256)][int]$Jobs = 0,
    [string]$Tools = '',
    [string]$Work = '',
    [switch]$Rebuild,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # (Invoke-WebRequest's progress bar makes big downloads many times slower)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$kit = Split-Path -Parent $PSScriptRoot
$hs = Join-Path $kit 'hookshot'
. (Join-Path $PSScriptRoot 'kit-paths.ps1')   # (Test-KitOtherKitGame, Find-KitHookshotGameToCopy)
. (Join-Path $PSScriptRoot 'coop-ini.ps1')

function Fail([string]$Text) { Write-Host ''; Write-Host "Longshot build: $Text" -ForegroundColor Red; exit 1 }
function Step([string]$Text) { Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan }
function Read-KeyFile([string]$Path) {
    $map = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $map[$Matches[1]] = $Matches[2] }
    }
    return $map
}
function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
# $Urls are tried in order (tools.lock gives mirrors): the first that gives the file with the right SHA-256 wins.
function Get-Download([string[]]$Urls, [string]$To, [string]$Sha256, [string]$What = '') {
    if ((Test-Path -LiteralPath $To) -and (Get-Sha256 $To) -eq $Sha256) { return }
    Write-Host "  downloading $(Split-Path -Leaf $To) ..."
    $part = "$To.part"
    $why = ''
    foreach ($url in $Urls) {
        try { Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing }
        catch { $why = "The download of $url failed: $($_.Exception.Message). Check the internet connection and run this again."; continue }
        if ($Sha256 -and (Get-Sha256 $part) -ne $Sha256) {
            Remove-Item -LiteralPath $part -Force
            if (-not $What) { $What = "$(Split-Path -Leaf $To) is not the file Longshot published" }
            $why = "$What (SHA-256 differs). Run this again; if it repeats, tell the co-op testers."
            continue
        }
        Move-Item -LiteralPath $part -Destination $To -Force
        return
    }
    Fail $why
}
# -Member: only that folder of the zip, its path left out; -Keep: files already there are not overwritten.
function Expand-Zip([string]$Zip, [string]$To, [int]$Strip = 0, [string]$Member = '', [switch]$Keep) {
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    $tarArgs = @('-x', '-f', $Zip, '-C', $To)
    if ($Keep) { $tarArgs += '-k' }
    if ($Member) { $Strip = @($Member -split '/').Count }
    if ($Strip) { $tarArgs += "--strip-components=$Strip" }
    if ($Member) { $tarArgs += "$Member/" }
    & "$env:SystemRoot\System32\tar.exe" @tarArgs
    if ($LASTEXITCODE -ne 0) { Fail "Unpacking $(Split-Path -Leaf $Zip) failed (tar $LASTEXITCODE). Is the disk full?" }
}

# --- What this kit has ---------------------------------------------------------------------
if (-not [Environment]::Is64BitOperatingSystem) { Fail 'Longshot needs 64-bit Windows.' }
foreach ($need in 'PIN', 'TOOLS.txt') {
    if (-not (Test-Path -LiteralPath (Join-Path $hs $need))) { Fail "This kit has no hookshot\$need. Get a kit with Hookshot support (Update.bat)." }
}
$patches = @(Get-ChildItem -LiteralPath (Join-Path $hs 'patches') -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
if (-not $patches) { Fail 'This kit has no hookshot\patches\*.patch.' }
$pin = Read-KeyFile (Join-Path $hs 'PIN')
$toolsPin = Read-KeyFile (Join-Path $hs 'TOOLS.txt')
foreach ($k in 'repo', 'commit') { if (-not $pin[$k]) { Fail "hookshot\PIN has no $k=." } }
foreach ($k in 'release', 'lock_sha256', 'source_commit', 'source_sha256') { if (-not $toolsPin[$k]) { Fail "hookshot\TOOLS.txt has no $k=." } }
if ($pin.commit -notmatch '^[0-9a-f]{40}$') { Fail "hookshot\PIN: commit=$($pin.commit) is not a full commit id." }
# (Longshot lives on Team MagicBean's Forgejo; its release files and source archive come from there)
if ($pin.repo -notmatch '^(https://git\.magicbean\.dev)/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(\.git)?$') { Fail "hookshot\PIN: repo=$($pin.repo) is not a git.magicbean.dev repository." }
$repoUrl = "$($Matches[1])/$($Matches[2])"
# (Forgejo's API archive: the web /archive/ path answers 404 since 2026-11-13; same bytes, so it is tried first)
$apiUrl = "$($Matches[1])/api/v1/repos/$($Matches[2])"
# The source archive's SHA-256 in TOOLS.txt is for one commit: it must be PIN's.
if ($toolsPin.source_commit -ne $pin.commit) {
    Fail "hookshot\TOOLS.txt pins the source archive of $($toolsPin.source_commit), but hookshot\PIN says $($pin.commit). This kit is broken: tell the co-op testers."
}
$patchHash = (($patches | ForEach-Object { Get-Sha256 $_.FullName }) -join ',')
# What the tools folder and the game are made from: when this is unchanged, steps 1 to 3 are done.
$stamp = "release=$($toolsPin.release)`ncommit=$($pin.commit)`npatches=$patchHash"

$sharedWork = [bool]$Work
$tools = if ($Tools) { [System.IO.Path]::GetFullPath($Tools) } else { Join-Path $hs 'tools' }
$work = if ($Work) { [System.IO.Path]::GetFullPath($Work) } else { Join-Path $hs 'work' }
# (beside a shared tools folder, so its 400 MB of parts are fetched once for every kit)
$downloads = if ($Tools) { Join-Path (Split-Path -Parent $tools) 'downloads' } else { Join-Path $hs 'downloads' }
if (-not $Out) { $Out = Join-Path $hs 'game' }
$Out = [System.IO.Path]::GetFullPath($Out)
$python = Join-Path $tools 'python\python.exe'

# --- 1 and 2: the tools and the patched source ---------------------------------------------
$toolsStamp = Join-Path $tools 'coop-tools.txt'
$toolsReady = (Test-Path -LiteralPath $toolsStamp) -and ([System.IO.File]::ReadAllText($toolsStamp) -eq $stamp) -and (Test-Path -LiteralPath $python)
# The same release and pin with other patches: the source is refreshed in place (only what the patches touch).
$baseStamp = "release=$($toolsPin.release)`ncommit=$($pin.commit)`n"
$keep = Join-Path $tools '.coop-orig'
$samePin = -not $toolsReady -and (Test-Path -LiteralPath $toolsStamp) -and (Test-Path -LiteralPath $python) -and
    (Test-Path -LiteralPath (Join-Path $keep 'manifest.txt')) -and
    ([System.IO.File]::ReadAllText($toolsStamp).StartsWith($baseStamp))
if ($samePin) {
    Step "Longshot's source: the co-op patches changed ($($patches.Count) patch(es); the tools and pin stay)"
    & $python -I -B (Join-Path $PSScriptRoot 'apply_patch.py') --refresh $keep $tools @($patches | ForEach-Object FullName)
    if ($LASTEXITCODE -eq 0) {
        [System.IO.File]::WriteAllText($toolsStamp, $stamp)
        $toolsReady = $true
    } else {
        # (a refresh stopped half way, or a source changed by hand: the whole source again, once)
        Write-Host '  The source could not be refreshed in place: unpacking it again.' -ForegroundColor Yellow
        Remove-Item -LiteralPath $toolsStamp -Force -ErrorAction SilentlyContinue
    }
}
if ($toolsReady) {
    Step "Longshot's tools: ready ($($toolsPin.release) tools, Longshot $($pin.commit.Substring(0, 12)) with $($patches.Count) co-op patch(es))"
} else {
    Step "Longshot's build tools ($($toolsPin.release)) and source ($($pin.commit.Substring(0, 12)))"
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $lock = Join-Path $downloads "tools-$($toolsPin.release).lock"
    Get-Download "$repoUrl/releases/download/$($toolsPin.release)/tools.lock" $lock $toolsPin.lock_sha256.ToLowerInvariant()
    # tools.lock: "tool <name> <version> <system> <sha256> <size> <dest> <prefix> <url> [<url> ...]"; the URLs are
    # tried in order (Longshot's Cloudflare copy, git.magicbean.dev, then upstream). Of the windows-x64 tools, emsdk
    # is not fetched (llvm-mingw builds the game; the builder sets EMSDK only when its folder is there).
    $wanted = @('llvm-mingw', 'python', 'cpm', 'wgpu-native', 'third-party')
    $parts = @()
    foreach ($line in Get-Content -LiteralPath $lock) {
        $f = $line.Trim() -split '\s+'
        if ($f[0] -ne 'tool' -or $f.Count -lt 9 -or $f[3] -ne 'windows-x64' -or $wanted -notcontains $f[1]) { continue }
        if ($f[6] -notmatch '^LongshotBuilder/tools/([A-Za-z0-9_.-]+)$') { Fail "tools.lock puts $($f[1]) outside LongshotBuilder/tools: $($f[6])" }
        $folder = $Matches[1]
        $urls = @($f[8..($f.Count - 1)] | Where-Object { $_ -match '^https://' })
        if (-not $urls) { Fail "tools.lock has no https download for $($f[1])." }
        $parts += [pscustomobject]@{ Name = $f[1]; Sha = $f[4].ToLowerInvariant(); Folder = $folder; Prefix = $f[7]; Urls = $urls
                                      File = (Join-Path $downloads "$($f[1])-$($f[4].Substring(0, 12)).zip") }
    }
    if ($parts.Count -ne $wanted.Count) { Fail "tools.lock of $($toolsPin.release) does not list the $($wanted.Count) windows-x64 tools this script needs (found $($parts.Count))." }
    foreach ($p in $parts) { Get-Download $p.Urls $p.File $p.Sha }
    # Forgejo's source archive at PIN's commit, checked against TOOLS.txt's source_sha256 (a copy
    # already here is checked too, and fetched again when it differs).
    $sourceZip = Join-Path $downloads "longshot-$($pin.commit).zip"
    Get-Download @("$apiUrl/archive/$($pin.commit).zip", "$repoUrl/archive/$($pin.commit).zip") $sourceZip $toolsPin.source_sha256.ToLowerInvariant() "Longshot's source archive differs from the one this kit was made with"

    Write-Host '  unpacking ...'
    $staging = "$tools.new"
    $why = Remove-KitFolderRetry $staging
    if ($why) { Fail (Get-KitHeldFolderText $staging $why) }
    # The source: the archive's one top folder (longshot/) left out.
    Expand-Zip $sourceZip $staging 1
    if (-not (Test-Path -LiteralPath (Join-Path $staging 'nx2\make_game.py'))) { Fail "Longshot's source archive has no nx2\make_game.py." }
    # Each tool as the builder's setup lays it out: <prefix> in the zip (or the whole zip) becomes tools\<folder>,
    # unpacked straight there (no copy of the deep HookshotBuilder\tools\... paths, which pass 260 characters).
    # Of third_party only the files the source lacks (the prebuilt .dll/.exe/.lib/.sha1), as Longshot's readme
    # says: tar -k keeps the source's own. (No play profile: it is optional.)
    foreach ($p in $parts) {
        $member = if ($p.Prefix -eq '-') { '' } else { $p.Prefix.TrimEnd('/') }
        Expand-Zip $p.File (Join-Path $staging $p.Folder) -Member $member -Keep:($p.Folder -eq 'third_party')
        if (-not (Get-ChildItem -LiteralPath (Join-Path $staging $p.Folder) -Force | Select-Object -First 1)) {
            Fail "The $($p.Name) archive of $($toolsPin.release) has no $($p.Prefix) folder."
        }
    }

    Write-Host "  applying $($patches.Count) co-op patch(es) ..."
    # (--refresh keeps the originals of what the patches change: a later patch change refreshes only those)
    & (Join-Path $staging 'python\python.exe') -I -B (Join-Path $PSScriptRoot 'apply_patch.py') --refresh (Join-Path $staging '.coop-orig') $staging @($patches | ForEach-Object FullName)
    if ($LASTEXITCODE -ne 0) { Fail "The co-op patches do not apply to Longshot $($pin.commit). This kit is broken: tell the co-op testers." }
    # (antivirus, Explorer or the indexer may hold a file of the fresh unzip for a moment: tried again, then robocopy)
    $held = $tools
    $why = Remove-KitFolderRetry $tools
    if (-not $why) { $held = $staging; $why = Move-KitFolderRetry $staging $tools }
    if ($why) { Fail (Get-KitHeldFolderText $held $why) }
    # (the stamp last: a move that stopped half way, a long path or a full disk, leaves no folder that looks ready)
    [System.IO.File]::WriteAllText($toolsStamp, $stamp)
    Write-Host '  ready.'
}

# --- 3: the game ---------------------------------------------------------------------------
function Find-HookshotExe([string]$Folder) {
    foreach ($p in (Join-Path $Folder 'bin\longshot.exe'), (Join-Path $Folder 'longshot.exe')) {
        if (Test-Path -LiteralPath $p) {
            # A builder's game folder has its launcher as longshot.exe and the game in bin\.
            return $p
        }
    }
    return $null
}
# A second kit on this PC (unzipped twice, or a Windows copy of a kit): another kit's game made from the same
# release, pin and patches is copied into this kit's own hookshot\game (a minute or two) instead of built again
# (10 to 45 minutes). A copy, never the other kit's folder itself: two kits never start one game. Its coop.ini,
# coop-options.ini and logs stay behind, so this kit gets its own name, server and controller below.
$defaultOut = [System.IO.Path]::GetFullPath((Join-Path $hs 'game'))
if (-not $Rebuild -and -not $CheckOnly -and $Out -ieq $defaultOut -and -not (Find-HookshotExe $Out)) {
    $others = @()
    $named = Join-Path $hs 'game-folder.txt'
    if (Test-Path -LiteralPath $named) {
        $n = "$(Get-Content -LiteralPath $named -TotalCount 1)".Trim()
        if ($n -and [System.IO.Path]::IsPathRooted($n) -and (Test-KitOtherKitGame $kit $n)) { $others += Split-Path -Parent (Split-Path -Parent $n) }
    }
    $list = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'OoT-Coop\kits.txt' } else { '' }
    if ($list -and (Test-Path -LiteralPath $list)) { $others += @(Get-Content -LiteralPath $list | Where-Object { $_ }) }
    $from = Find-KitHookshotGameToCopy $kit $stamp $others
    if ($from) {
        Step "The game: copying the one another kit on this PC built ($from), instead of building it again"
        New-Item -ItemType Directory -Force -Path $Out | Out-Null
        & robocopy $from $Out /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XF coop.ini coop-options.ini *.log *_log.txt | Out-Null
        if ($LASTEXITCODE -ge 8 -or -not (Find-HookshotExe $Out)) {
            Write-Host "  Copying failed (robocopy $LASTEXITCODE): the game is built here instead." -ForegroundColor Yellow
            Remove-Item -LiteralPath $Out -Recurse -Force -ErrorAction SilentlyContinue
        } else {
            Write-Host "  Copied to $Out (this kit's own game from now on)."
        }
        $global:LASTEXITCODE = 0
    }
}$gameStamp = Join-Path $Out 'coop-built-from.txt'
$exe = Find-HookshotExe $Out
$gameReady = $exe -and (Test-Path -LiteralPath $gameStamp) -and ([System.IO.File]::ReadAllText($gameStamp) -eq $stamp)

if (-not $Game -and (-not $gameReady -or $Rebuild -or $CheckOnly)) {
    # As make-puppet-pack.cmd: the .nup in a folder of the emulator's game list.
    $check = Join-Path $kit 'tools\puppet_pack\dumpcheck.py'
    if (Test-Path -LiteralPath $check) {
        $ErrorActionPreference = 'Continue'
        $found = & $python -I -B $check --user-dir (Join-Path $kit 'emulator\user') 2>$null | Where-Object { $_ -like 'romfs=*.nup' } | Select-Object -First 1
        $ErrorActionPreference = 'Stop'
        if ($found) { $Game = $found.Substring(6); Write-Host "Found your game: $Game" }
    }
    # The game files given the last time (hookshot\game-files.txt): asked only once.
    $last = Join-Path $hs 'game-files.txt'
    if (-not $Game -and (Test-Path -LiteralPath $last)) {
        $remembered = "$(Get-Content -LiteralPath $last -TotalCount 1)".Trim()
        if ($remembered -and (Test-Path -LiteralPath $remembered)) { $Game = $remembered; Write-Host "Your game (from the last time): $Game" }
    }
    if (-not $Game) {
        Write-Host ''
        Write-Host 'Drag your game''s .nup file (or its romfs folder) into this window and press Enter:'
        $Game = "$(Read-Host)".Trim().Trim('"')
        if (-not $Game) { Fail 'No game files given. Run this again with -Game <your .nup>.' }
    }
}
if ($Game) {
    if (-not (Test-Path -LiteralPath $Game)) { Fail "Not found: $Game" }
    $Game = (Resolve-Path -LiteralPath $Game).Path
    if ((Get-Item -LiteralPath $Game).PSIsContainer) {
        if (-not $Exefs) {
            $guess = Join-Path (Split-Path -Parent $Game) 'exefs'
            if (Test-Path -LiteralPath (Join-Path $guess 'main')) { $Exefs = $guess }
            else {
                Write-Host 'A romfs folder needs its exefs folder too. Drag the exefs folder in and press Enter:'
                $Exefs = "$(Read-Host)".Trim().Trim('"')
                if (-not $Exefs) { Fail 'No exefs folder given.' }
            }
        }
        if (-not (Test-Path -LiteralPath (Join-Path $Exefs 'main'))) { Fail "$Exefs is not an exefs folder (no main in it)." }
        $Exefs = (Resolve-Path -LiteralPath $Exefs).Path
        $gameArgs = @('--romfs', $Game, '--exefs', $Exefs)
    } else {
        $gameArgs = @('--nup', $Game)
    }
}

# make_game.py runs in Longshot's tools folder with the builder's own compiler, headers and
# libraries, as Longshot Builder runs it (builder/LongshotBuilder/Runner.cs).
function Invoke-MakeGame([string[]]$MoreArgs, [string]$Log = '') {
    $env:PYTHONIOENCODING = 'utf-8'
    $env:RX_CPM = Join-Path $tools 'cpm'
    $env:WGPU_NATIVE = Join-Path $tools 'wgpu-native'
    $env:LONGSHOT_MINGW = Join-Path $tools 'llvm-mingw'
    foreach ($v in 'LONGSHOT_NGX_SDK', 'LONGSHOT_FFX_SDK', 'LONGSHOT_MSVC', 'EMSDK', 'LONGSHOT_BUILDER_EXE', 'LONGSHOT_TOOLS') {
        Remove-Item "Env:\$v" -ErrorAction SilentlyContinue
    }
    # (a shared work folder keeps all its build files, the translated C too: the next build of this pin
    # skips the translation unless a lift file changed. A kit's own folder keeps Longshot's default.)
    if ($sharedWork) { $env:LONGSHOT_KEEP_BUILD_FILES = '1' }
    # (the release the menu names, top left: the source archive has no git history and no VERSION file, so
    # without this the builder's nx2\version.py says "unknown")
    if ($pin.tag) { $env:LONGSHOT_VERSION = $pin.tag }
    # (Longshot's lift key hashes every A64_/OOT_/RX_ variable it is started with: co-op's own OOT_COOP_* ones
    # (the test cache, the test password) would make a new key, and a whole new translation, for nothing)
    Get-ChildItem Env: | Where-Object { $_.Name -like 'OOT_COOP_*' } | ForEach-Object { Remove-Item "Env:\$($_.Name)" }
    $logArgs = if ($Log) { @('--log', $Log) } else { @() }
    Push-Location $tools
    try {
        $ErrorActionPreference = 'Continue'
        # (its "@@<word> ..." lines are for the builder window: the stage names and errors are shown, the rest left out)
        & $python -I (Join-Path $tools 'nx2\make_game.py') @MoreArgs @logArgs 2>&1 | ForEach-Object {
            $line = "$_"
            if ($line -match '^@@stage \S+ (.*)$') { Write-Host "  -- $($Matches[1])" -ForegroundColor Cyan }
            elseif ($line -match '^@@error (.*)$') { Write-Host "  $($Matches[1])" -ForegroundColor Red }
            elseif ($line -notmatch '^@@') { Write-Host "  $line" }
        }
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
    } finally { Pop-Location }
    return $code
}

if ($gameReady -and -not $Rebuild -and -not $CheckOnly) {
    Step "The game: built already ($Out)"
} else {
    if (-not $gameArgs) { Fail 'No game files given.' }
    Step 'Checking your game files'
    $code = Invoke-MakeGame ($gameArgs + @('--check'))
    if ($code -ne 0) { Fail 'Longshot cannot build from these files (it said why above). This is not the game version co-op needs: program 0400486024DEC000, update v131072, build e0f3ccfa. Dump that version of your game and give its .nup.' }
    if ($CheckOnly) { Write-Host ''; Write-Host 'Your game files are right for Longshot. Run this again without -CheckOnly to build.' -ForegroundColor Green; exit 0 }
    $drive = [System.IO.Path]::GetPathRoot($work)
    $free = (Get-PSDrive -Name $drive.Substring(0, 1) -ErrorAction SilentlyContinue).Free
    if ($free -and $free -lt 60GB -and -not (Test-Path -LiteralPath (Join-Path $work 'sa'))) {
        Write-Host ('Warning: {0:N0} GB free on {1}; the first build wants about 60 GB.' -f ($free / 1GB), $drive) -ForegroundColor Yellow
    }
    Step 'Building the game (10 to 45 minutes; the PC stays usable)'
    $buildLog = Join-Path $hs 'build.log'
    $buildArgs = $gameArgs + @('--out', $Out, '--work', $work, '--language', $Language)
    if ($Jobs) { $buildArgs += @('--jobs', "$Jobs") }
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    $code = Invoke-MakeGame $buildArgs $buildLog
    if ($code -ne 0) { Fail "The build stopped (exit $code). Its log: $buildLog" }
    $exe = Find-HookshotExe $Out
    if (-not $exe) { Fail "The build finished but there is no longshot.exe in $Out. Its log: $buildLog" }
    [System.IO.File]::WriteAllText($gameStamp, $stamp)
}
# Where the game is, for Host-Hookshot.bat and Join-Hookshot.bat (play.ps1 -Hookshot).
# (relative when it is inside the kit, so a copied or moved kit keeps its own game: kit-copy)
$kitFull = [System.IO.Path]::GetFullPath($kit).TrimEnd('\') + '\'
$outNote = if ($Out.StartsWith($kitFull, [StringComparison]::OrdinalIgnoreCase)) { $Out.Substring($kitFull.Length) } else { $Out }
[System.IO.File]::WriteAllText((Join-Path $hs 'game-folder.txt'), "$outNote`n")

# --- 4: co-op into the game ----------------------------------------------------------------
Step 'Co-op into the game'
$exeDir = Split-Path -Parent $exe
# The module and the Visual C++ runtime it needs (when the kit carries it), beside longshot.exe.
# (a kit always has it; a hand-made test folder may not yet: the game is built, and co-op is off until it is there)
$module = Join-Path $hs 'hookshot_coop.dll'
if (-not (Test-Path -LiteralPath $module)) {
    Write-Host "  No hookshot\hookshot_coop.dll in this kit: the game is built without co-op. Put the module there and run this again." -ForegroundColor Yellow
    exit 1
}
Copy-Item -LiteralPath $module -Destination $exeDir -Force
Get-ChildItem -LiteralPath $hs -Filter '*.dll' -File | Where-Object { $_.Name -ne 'hookshot_coop.dll' } |
    Where-Object { -not (Test-Path -LiteralPath (Join-Path $exeDir $_.Name)) } | Copy-Item -Destination $exeDir
Write-Host "  hookshot_coop.dll -> $exeDir"

# coop.ini beside longshot.exe (the module reads it there): the player's own server and name from
# this kit's emulator when the game has none yet.
$ini = Join-Path $exeDir 'coop.ini'
$kitIni = Join-Path $kit 'emulator\user\config\coop.ini'
$template = Join-Path $PSScriptRoot 'coop.ini.template'
if (-not (Test-Path -LiteralPath $ini)) {
    $have = if (Test-Path -LiteralPath $kitIni) { Read-CoopIni $kitIni } else { @{ server = ''; port = ''; name = ''; password = '' } }
    $port = if ($have.port) { $have.port } else { '27510' }
    Write-CoopIni $template $ini $have.server $port $have.name $have.password
}
$now = Read-CoopIni $ini
Write-Host "  coop.ini -> $ini (server: $(if ($now.server) { $now.server } else { 'not set' }), name: $(if ($now.name) { $now.name } else { 'not set' }))"
$options = Join-Path $exeDir 'coop-options.ini'
if (-not (Test-Path -LiteralPath $options)) {
    $kitOptions = Join-Path $kit 'coop-options.ini'
    if (-not (Test-Path -LiteralPath $kitOptions)) { $kitOptions = Join-Path $PSScriptRoot 'coop-options.default.ini' }
    if (Test-Path -LiteralPath $kitOptions) { Copy-Item -LiteralPath $kitOptions -Destination $options }
}

# The puppet pack, from the same game files, where the co-op hook looks for it: its load folder
# (<exe dir>\mods\contents on Hookshot)\0400486024DEC000\coop-puppet, the emulator's layout, which Hookshot's
# host/mods.c also loads as a mod. A pack an earlier version of this script put in mods\coop-puppet goes.
$packTool = Join-Path $kit 'tools\puppet_pack\build.py'
$mod = Join-Path $exeDir 'mods\contents\0400486024DEC000\coop-puppet'
$oldMod = Join-Path $exeDir 'mods\coop-puppet'
if (Test-Path -LiteralPath (Join-Path $oldMod 'coop_puppet.json')) { Remove-Item -LiteralPath $oldMod -Recurse -Force }
$packFrom = $Game
if (-not $packFrom) {
    $last = Join-Path $hs 'game-files.txt'
    if (Test-Path -LiteralPath $last) { $packFrom = (Get-Content -LiteralPath $last -TotalCount 1).Trim() }
}
if ($Game) { [System.IO.File]::WriteAllText((Join-Path $hs 'game-files.txt'), "$Game`n") }
if ($packFrom -and (Test-Path -LiteralPath $packFrom) -and (Test-Path -LiteralPath $packTool)) {
    Write-Host '  building the puppet pack ...'
    $ErrorActionPreference = 'Continue'
    # (no --title-rows: the title stays the game's own; co-op is the pause menu's Multiplayer tab)
    & $python -I -B $packTool --romfs $packFrom --mod-dir $mod 2>&1 | ForEach-Object { "    $_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { Write-Host '  The puppet pack was not built (said why above): other players show as stand-ins.' -ForegroundColor Yellow }
} else {
    Write-Host '  No game files to build the puppet pack from: run this again with -Game <your .nup>.' -ForegroundColor Yellow
}

# A desktop shortcut to the kit's launcher ("Start OoT Co-op.bat"; Play-OoT-Coop.bat in an older emulator kit), once,
# for the kit's own game (not for a game hs.py builds elsewhere).
$launcher = Join-Path $kit 'Start OoT Co-op.bat'
if (-not (Test-Path -LiteralPath $launcher)) { $launcher = Join-Path $kit 'Play-OoT-Coop.bat' }
if (-not $PSBoundParameters.ContainsKey('Out') -and -not $Tools -and (Test-Path -LiteralPath $launcher) -and
    -not $env:OOT_COOP_NO_SHORTCUT) {
    try {
        $desktop = [Environment]::GetFolderPath('Desktop')
        $link = Join-Path $desktop 'Play OoT Co-op.lnk'
        if ($desktop -and -not (Test-Path -LiteralPath $link)) {
            $shell = New-Object -ComObject WScript.Shell
            $s = $shell.CreateShortcut($link)
            $s.TargetPath = $launcher
            $s.WorkingDirectory = $kit
            $s.IconLocation = "$exe,0"
            $s.Description = 'OoT co-op on Hookshot: host or join, updates itself'
            $s.Save()
            Write-Host "  A shortcut 'Play OoT Co-op' is on your desktop."
        }
    } catch { Write-Host "  (no desktop shortcut: $($_.Exception.Message))" }
}

Write-Host ''
if (Test-Path -LiteralPath (Join-Path $kit 'Start OoT Co-op.bat')) {
    Write-Host 'Done. The game is ready: "Start OoT Co-op.bat" (or the desktop shortcut) starts it; Host and Join are in the game.' -ForegroundColor Green
    Write-Host "  The game: $exe. Its longshot_log.txt says 'coop: module loaded'."
    exit 0
}
Write-Host 'Done. To play co-op on Hookshot: Play-OoT-Coop.bat (or the desktop shortcut) starts the game, Host and Join are on its title screen; Host-Hookshot.bat hosts, Join-Hookshot.bat joins (as Host.bat and' -ForegroundColor Green
Write-Host "  Join.bat, with the Hookshot game in place of the emulator). The game: $exe"
Write-Host "  Its longshot_log.txt says 'coop: module loaded' and 'coop: connected'. HOS_COOP=0 starts it without co-op."
Write-Host '  Updates come by themselves: the launchers check for a new version and bring the game up to date.'
exit 0
