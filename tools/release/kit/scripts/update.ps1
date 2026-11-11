# Updates a co-op kit to the newest release, keeping everything of the player's.
#
# Update.bat runs this (make_kit.ps1 puts this whole script inside Update.bat, so the one
# file also works dropped into an old kit that has no updater yet).
#
#   update.ps1 [-Kit <kit folder>] [-Yes]   update this kit (asks before it starts, unless -Yes)
#   update.ps1 -Check                       only say whether a newer release is out
#   update.ps1 -Status                      one line for play.ps1: "newer <version>", "unfinished"
#                                           or nothing (none, or no network)
#   update.ps1 -Launch                      update without asking, for play.ps1 when the player
#                                           said yes (exit code 3: updated, start the new kit)
#
# play.ps1 (Host.bat, Join.bat, play.cmd) runs -Status on every start and, as coop-options.ini's
# auto_update says (ask, yes or no), -Launch; then it starts the new kit's own play.ps1.
#
# What it does: reads the kit's version from VERSION.txt, asks GitHub for the releases of
# daurosk/oot-remake-multiplayer (test builds count, drafts never) and takes the newest. If that
# is newer, it downloads the release's zip, checks it against the SHA-256 published beside it
# (oot-coop-<version>.zip.sha256), unpacks it to a temp folder and copies the kit's own files
# over this kit. Every file it replaces is first copied to backup-<old version>\; if anything
# fails, those are put back and the new files removed. VERSION.txt is written last, and
# update-in-progress.txt marks an update that has started: an update that was stopped half way
# (window closed, power cut) is done again by the next run.
#
# The shader cache (emulator\user\cache\shader\) belongs to one emulator build: when the new
# release brings a different emulator\lichii.exe, it is moved to backup-<old version>\ too, so
# the new emulator starts with an empty cache (an old one can crash it). Same emulator: kept.
#
# Never touched: emulator\user\ (keys, firmware, saves, settings, coop.ini with your name and
# server, the puppet pack), logs\, backup-*\ and every file that is not part of the kit.
# coop-options.ini keeps your values; a switch the new kit adds is appended as a '#' line.
# No data about you is sent: one request for the release list and the two downloads.
#
# For tests: -Api (or the environment variable OOT_COOP_UPDATE_API, which play.ps1 passes on)
# names the release list (a URL or a local .json file) and the download addresses in it may
# be local files; -NoRun only loads the functions.
param(
    [string]$Kit = '',
    [switch]$Yes,
    [switch]$Check,
    [switch]$Status,
    [switch]$Launch,
    [string]$Repo = 'daurosk/oot-remake-multiplayer',
    [string]$Api = '',
    [switch]$NoRun
)

if (-not $Api -and $env:OOT_COOP_UPDATE_API) { $Api = $env:OOT_COOP_UPDATE_API }
if ($Launch) { $Yes = [switch]$true }
$UserAgent = 'oot-coop-updater'
$InProgress = 'update-in-progress.txt'

# --- Versions ---------------------------------------------------------------------------

# 'v0.1.0-beta2' -> @{ Core = 0,1,0; Pre = 'beta2' }; $null when it is not a version.
function ConvertTo-KitVersion([string]$Text) {
    if ($Text -notmatch '^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$') { return $null }
    return @{ Core = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3]); Pre = [string]$Matches[4] }
}

# One part of a test-build name, compared as text and number runs: beta2 < beta10 < betab.
function Compare-KitPreRun([string]$A, [string]$B) {
    $ra = @([regex]::Matches($A, '\d+|\D+') | ForEach-Object { $_.Value })
    $rb = @([regex]::Matches($B, '\d+|\D+') | ForEach-Object { $_.Value })
    for ($i = 0; $i -lt [Math]::Min($ra.Count, $rb.Count); $i++) {
        $x = $ra[$i]; $y = $rb[$i]
        $nx = $x -match '^\d+$'; $ny = $y -match '^\d+$'
        if ($nx -and $ny) {
            $c = ([decimal]$x).CompareTo([decimal]$y)
        } elseif ($nx -ne $ny) {
            $c = if ($nx) { -1 } else { 1 }
        } else {
            $c = [string]::CompareOrdinal($x, $y)
        }
        if ($c -ne 0) { return [Math]::Sign($c) }
    }
    return [Math]::Sign($ra.Count - $rb.Count)
}

# -1, 0 or 1. A version with no test-build part is newer than its test builds
# (v0.1.0 > v0.1.0-beta2). Something that is not a version counts as the oldest.
function Compare-KitVersion([string]$A, [string]$B) {
    $va = ConvertTo-KitVersion $A
    $vb = ConvertTo-KitVersion $B
    if (-not $va -and -not $vb) { return 0 }
    if (-not $va) { return -1 }
    if (-not $vb) { return 1 }
    for ($i = 0; $i -lt 3; $i++) {
        if ($va.Core[$i] -ne $vb.Core[$i]) { return [Math]::Sign($va.Core[$i] - $vb.Core[$i]) }
    }
    if ($va.Pre -eq $vb.Pre) { return 0 }
    if (-not $va.Pre) { return 1 }
    if (-not $vb.Pre) { return -1 }
    $pa = $va.Pre.Split('.'); $pb = $vb.Pre.Split('.')
    for ($i = 0; $i -lt [Math]::Min($pa.Count, $pb.Count); $i++) {
        $c = Compare-KitPreRun $pa[$i] $pb[$i]
        if ($c -ne 0) { return $c }
    }
    return [Math]::Sign($pa.Count - $pb.Count)
}

# The release this kit was made from, from the first line of VERSION.txt
# ("kit: oot-coop-v0.1.0-beta2 (release v0.1.0-beta2)"); '' for a kit that is no release.
function Get-KitVersion([string]$KitDir) {
    $file = Join-Path $KitDir 'VERSION.txt'
    if (-not (Test-Path -LiteralPath $file)) { return '' }
    foreach ($line in [System.IO.File]::ReadAllLines($file)) {
        if ($line -match '\(release ([^)]+)\)') { return $Matches[1] }
        if ($line -match '^kit: oot-coop-(v\d[^ ]*)') { return $Matches[1] }
    }
    return ''
}

# True when an update of this kit started and did not finish.
function Test-KitUnfinished([string]$KitDir) {
    return Test-Path -LiteralPath (Join-Path $KitDir $InProgress)
}

# The version a stopped update started from, from update-in-progress.txt ("... this kit from
# v0.1.0-beta4.1 to ..."); '' when there is no marker or it does not say (older updaters).
# VERSION.txt may already be the new one then, so the backup is named after this one.
function Get-KitUnfinishedFrom([string]$KitDir) {
    $file = Join-Path $KitDir $InProgress
    if (-not (Test-Path -LiteralPath $file)) { return '' }
    $text = [System.IO.File]::ReadAllText($file)
    if ($text -match '\bfrom (v\d[0-9A-Za-z.-]*) to ' -and (ConvertTo-KitVersion $Matches[1])) { return $Matches[1] }
    return ''
}

# Shader caches an earlier update moved into a backup-*\ folder of the kit (full paths).
function Get-KitBackedUpShaders([string]$KitDir) {
    return @(Get-ChildItem -LiteralPath $KitDir -Directory -Filter 'backup-*' -Force -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName $ShaderCache } | Where-Object { Test-Path -LiteralPath $_ -PathType Container })
}

# --- Releases ---------------------------------------------------------------------------

function Read-KitReleaseList([string]$Source) {
    if ($Source -notmatch '^https?://') {
        return [System.IO.File]::ReadAllText($Source) | ConvertFrom-Json
    }
    # Windows PowerShell 5.1 does not offer TLS 1.2 unless asked; GitHub needs it.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $timeout = if ($Check -or $Status) { 5 } else { 30 }
    return Invoke-RestMethod -Uri $Source -UserAgent $UserAgent -TimeoutSec $timeout -Headers @{ Accept = 'application/vnd.github+json' }
}

# The newest release that has a kit zip and its .sha256 beside it; drafts never.
function Select-KitRelease($Releases) {
    $best = $null
    foreach ($r in @($Releases)) {
        if ($r.draft) { continue }
        $tag = [string]$r.tag_name
        if (-not (ConvertTo-KitVersion $tag)) { continue }
        $zip = @($r.assets) | Where-Object { $_.name -eq "oot-coop-$tag.zip" } | Select-Object -First 1
        $sum = @($r.assets) | Where-Object { $_.name -eq "oot-coop-$tag.zip.sha256" } | Select-Object -First 1
        if (-not $zip -or -not $sum) { continue }
        if (-not $best -or (Compare-KitVersion $tag $best.Tag) -gt 0) {
            $best = @{ Tag = $tag; Zip = [string]$zip.browser_download_url; Sum = [string]$sum.browser_download_url;
                       Size = [long]$zip.size; Page = [string]$r.html_url }
        }
    }
    return $best
}

# True for a download address of this repository's own releases. Outside tests nothing else
# is fetched, so an odd address in the list can never make the updater read a local or
# network path.
function Test-KitDownloadUrl([string]$Url, [string]$RepoName) {
    $prefix = "https://github.com/$RepoName/releases/download/"
    return $Url.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and $Url -notmatch '[\\]|/\.\.?(/|$)'
}

function Get-KitDownload([string]$From, [string]$To) {
    if ($From -notmatch '^https?://') { Copy-Item -LiteralPath $From -Destination $To; return }
    $old = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'   # the progress bar makes 5.1's download very slow
    try { Invoke-WebRequest -Uri $From -OutFile $To -UserAgent $UserAgent -UseBasicParsing -TimeoutSec 600 }
    finally { $ProgressPreference = $old }
}

# The hash in a .sha256 file ("<64 hex>  name" or only the hash).
function Read-KitSha256([string]$File) {
    $text = [System.IO.File]::ReadAllText($File)
    if ($text -notmatch '(?i)\b([0-9a-f]{64})\b') { throw "No SHA-256 in $File." }
    return $Matches[1].ToLowerInvariant()
}

# --- What belongs to the player ---------------------------------------------------------

# True for a kit-relative path the updater never writes or removes.
function Test-KitPlayerPath([string]$Rel) {
    $p = $Rel.Replace('/', '\').TrimStart('\')
    if ($p -ieq 'coop-options.ini') { return $true }            # merged, never replaced
    foreach ($dir in 'emulator\user\', 'logs\') {
        if ($p.StartsWith($dir, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    if ($p -match '^(?i)backup-[^\\]+\\') { return $true }
    return $false
}

# True for a plain path inside the kit: not rooted, no drive, no '.' or '..' part. A line of
# an old MANIFEST.txt that is not one is never acted on.
function Test-KitRelativePath([string]$Rel) {
    if (-not $Rel -or $Rel.Contains(':') -or [System.IO.Path]::IsPathRooted($Rel)) { return $false }
    foreach ($part in $Rel.Replace('/', '\').Split('\')) {
        if ($part -eq '' -or $part -eq '.' -or $part -eq '..') { return $false }
    }
    return $true
}

# Files of a kit folder, as kit-relative paths.
function Get-KitFiles([string]$Dir) {
    $root = [System.IO.Path]::GetFullPath($Dir).TrimEnd('\') + '\'
    return @(Get-ChildItem -LiteralPath $Dir -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($root.Length) })
}

# The order files are written in: VERSION.txt and MANIFEST.txt last, so a kit whose update
# stopped half way never says it is the new version.
function Get-KitInstallOrder([string[]]$Files) {
    $last = @('MANIFEST.txt', 'VERSION.txt')
    $first = @($Files | Where-Object { $last -notcontains $_ })
    $end = @($last | Where-Object { $Files -contains $_ })
    return [string[]](@($first) + @($end))
}

# The switch names a coop-options.ini mentions, set or behind a '#': the emulator's
# LICHII_COOP_... switches and the kit's own auto_update, server_allow and server_bind.
$OptionName = '(LICHII_COOP_[A-Z0-9_]+|auto_update|server_allow|server_bind)'
function Get-KitOptionNames([string[]]$Lines) {
    $names = @{}
    foreach ($l in $Lines) {
        if ($l -match ('^\s*[#;]?\s*' + $OptionName + '\s*=')) { $names[$Matches[1]] = $true }
    }
    return $names
}

# coop-options.ini's auto_update: 'ask' (the default, also for a value it does not know),
# 'yes' or 'no'.
function Get-KitAutoUpdate([string]$KitDir) {
    $file = Join-Path $KitDir 'coop-options.ini'
    if (Test-Path -LiteralPath $file) {
        foreach ($line in [System.IO.File]::ReadAllLines($file)) {
            if ($line -match '^\s*auto_update\s*=\s*(ask|yes|no)\s*$') { return $Matches[1].ToLowerInvariant() }
        }
    }
    return 'ask'
}

# The player's coop-options.ini lines with every switch the new template names and the
# player's file does not, appended behind '#' with the template's comment above it.
# The player's own lines stay as they are.
function Merge-KitOptions([string[]]$Mine, [string[]]$New, [string]$Version) {
    $have = Get-KitOptionNames $Mine
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($l in $Mine) { $out.Add($l) }
    $added = New-Object System.Collections.Generic.List[string]
    $comment = New-Object System.Collections.Generic.List[string]
    foreach ($l in $New) {
        $t = $l.Trim()
        if ($t -match ('^[#;]?\s*' + $OptionName + '\s*=')) {
            $name = $Matches[1]
            if (-not $have.ContainsKey($name)) {
                foreach ($c in $comment) { $added.Add($c) }
                $added.Add('# ' + $t.TrimStart('#', ';').Trim())
                $have[$name] = $true
            }
        } elseif ($t.StartsWith('#') -and -not $t.StartsWith('# ---')) {
            $comment.Add($t)
            continue
        }
        $comment.Clear()
    }
    if ($added.Count) {
        while ($out.Count -and -not $out[$out.Count - 1].Trim()) { $out.RemoveAt($out.Count - 1) }
        $out.Add('')
        $out.Add("# --- Added by Update.bat ($Version): switches new in this version, off. Remove the '#' to use one. ---")
        foreach ($a in $added) { $out.Add($a) }
    }
    return ,([string[]]$out.ToArray())
}

# --- Installing -------------------------------------------------------------------------

# Why a kit's emulator must not be installed, or ''. A kit holds only the co-op build of the
# emulator: never a plain Fritchii (no co-op), and never one with Fritchii's own updater,
# which would offer to swap the co-op build for a plain one.
function Test-KitEmulator([string]$Exe) {
    if (-not (Test-Path -LiteralPath $Exe)) { return 'it has no emulator\lichii.exe' }
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Exe))
    if (-not $text.Contains('coop: loaded')) { return 'its emulator is not the co-op build' }
    foreach ($s in 'Check for Updates', 'update-old') {
        if ($text.Contains($s)) { return 'its emulator has Fritchii''s own updater' }
    }
    return ''
}

# Where the emulator keeps its shader cache in a kit (Fritchii's portable user\ folder:
# user\cache\shader\, a folder per game and vulkan_static_pipelines.bin).
$ShaderCache = 'emulator\user\cache\shader'

# True when NewExe is another emulator build than OldExe (or OldExe cannot be read), so the
# old shader cache must not be used with it.
function Test-KitEmulatorChanged([string]$OldExe, [string]$NewExe) {
    if (-not (Test-Path -LiteralPath $OldExe)) { return $true }
    try {
        return (Get-FileHash -LiteralPath $OldExe -ErrorAction Stop).Hash -ne (Get-FileHash -LiteralPath $NewExe -ErrorAction Stop).Hash
    } catch { return $true }
}

function Get-KitRunning([string]$KitDir) {
    $root = [System.IO.Path]::GetFullPath($KitDir).TrimEnd('\') + '\'
    return @(Get-Process -Name lichii, coop_server -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)
    })
}

# The one folder at the top of an unpacked release zip (oot-coop-<version>\).
function Get-KitUnpackedRoot([string]$Dir) {
    $top = @(Get-ChildItem -LiteralPath $Dir -Force)
    if ($top.Count -eq 1 -and $top[0].PSIsContainer) { return $top[0].FullName }
    return $Dir
}

# Copies the new kit's own files over KitDir. Every file replaced or removed goes to
# Backup first; on failure everything is put back. With MoveShaders the shader cache is moved
# to Backup before anything else (and back on failure). Returns the number of files written.
function Install-KitFiles([string]$NewKit, [string]$KitDir, [string]$Backup, [string]$Version, [bool]$MoveShaders = $false, [string]$From = '') {
    $NewKit = [System.IO.Path]::GetFullPath($NewKit).TrimEnd('\')
    $newFiles = Get-KitInstallOrder @(Get-KitFiles $NewKit | Where-Object { -not (Test-KitPlayerPath $_) })
    # Files of the old kit that the new one no longer has: only those the old kit's
    # MANIFEST.txt lists (a kit before this updater has none, so nothing is removed), and
    # only plain files inside the kit.
    $gone = @()
    $manifest = Join-Path $KitDir 'MANIFEST.txt'
    if (Test-Path -LiteralPath $manifest) {
        $keep = @{}
        foreach ($f in $newFiles) { $keep[$f.ToLowerInvariant()] = $true }
        $gone = @([System.IO.File]::ReadAllLines($manifest) | ForEach-Object { $_.Trim() } | Where-Object {
            $_ -and -not $_.StartsWith('#') -and (Test-KitRelativePath $_) -and -not (Test-KitPlayerPath $_) -and
            -not $keep.ContainsKey($_.ToLowerInvariant()) -and (Test-Path -LiteralPath (Join-Path $KitDir $_) -PathType Leaf)
        })
    }
    # A changed puppet pack tool means the pack should be made again (make-puppet-pack.cmd).
    $script:PackToolChanged = $false
    foreach ($rel in $newFiles) {
        if (-not $rel.StartsWith('tools\puppet_pack\', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $old = Join-Path $KitDir $rel
        if (-not (Test-Path -LiteralPath $old) -or
            (Get-FileHash -LiteralPath $old).Hash -ne (Get-FileHash -LiteralPath (Join-Path $NewKit $rel)).Hash) {
            $script:PackToolChanged = $true
            break
        }
    }
    $saved = New-Object System.Collections.Generic.List[string]    # backed up (replaced or removed)
    $created = New-Object System.Collections.Generic.List[string]  # new, nothing to put back
    $marker = Join-Path $KitDir $InProgress
    $script:ShadersMoved = $false
    $shaders = Join-Path $KitDir $ShaderCache
    $shadersBackup = Join-Path $Backup $ShaderCache
    try {
        # From here until the end, the kit is between two versions.
        $fromText = if ($From) { "from $From " } else { '' }
        [System.IO.File]::WriteAllText($marker, "Update.bat was updating this kit ${fromText}to $Version. Run Update.bat again to finish it.`r`n")
        # First: a run stopped later has already moved it (and the next run sees the new exe).
        if ($MoveShaders -and (Test-Path -LiteralPath $shaders -PathType Container)) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $shadersBackup) | Out-Null
            Move-Item -LiteralPath $shaders -Destination $shadersBackup -ErrorAction Stop
            $script:ShadersMoved = $true
        }
        foreach ($rel in @($newFiles) + @($gone)) {
            $dest = Join-Path $KitDir $rel
            if (Test-Path -LiteralPath $dest) {
                $b = Join-Path $Backup $rel
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $b) | Out-Null
                Copy-Item -LiteralPath $dest -Destination $b -Force
                $saved.Add($rel)
            }
        }
        foreach ($rel in $gone) { Remove-Item -LiteralPath (Join-Path $KitDir $rel) -Force }
        # The player's options: kept, with the new switches added behind '#'.
        $mine = Join-Path $KitDir 'coop-options.ini'
        $tmpl = Join-Path $NewKit 'coop-options.ini'
        if ((Test-Path -LiteralPath $mine) -and (Test-Path -LiteralPath $tmpl)) {
            $lines = [System.IO.File]::ReadAllLines($mine)
            $merged = Merge-KitOptions $lines ([System.IO.File]::ReadAllLines($tmpl)) $Version
            if ($merged.Count -ne $lines.Count) {
                Copy-Item -LiteralPath $mine -Destination (Join-Path $Backup 'coop-options.ini') -Force
                $saved.Add('coop-options.ini')
                [System.IO.File]::WriteAllLines($mine, $merged)
            }
        } elseif (Test-Path -LiteralPath $tmpl) {
            Copy-Item -LiteralPath $tmpl -Destination $mine
            $created.Add('coop-options.ini')
        }
        # Folders the new kit has that this one lacks (empty user\ folders, logs\).
        Get-ChildItem -LiteralPath $NewKit -Recurse -Directory -Force | ForEach-Object {
            $d = Join-Path $KitDir $_.FullName.Substring($NewKit.Length + 1)
            if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
        }
        # The kit's files, VERSION.txt last (Get-KitInstallOrder).
        foreach ($rel in $newFiles) {
            $dest = Join-Path $KitDir $rel
            if (-not $saved.Contains($rel)) { $created.Add($rel) }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
            Copy-Item -LiteralPath (Join-Path $NewKit $rel) -Destination $dest -Force
        }
        Remove-Item -LiteralPath $marker -Force
    } catch {
        $why = $_
        $lost = New-Object System.Collections.Generic.List[string]
        if ($script:ShadersMoved) {
            try {
                if (Test-Path -LiteralPath $shaders) { Remove-Item -LiteralPath $shaders -Recurse -Force -ErrorAction Stop }
                Move-Item -LiteralPath $shadersBackup -Destination $shaders -ErrorAction Stop
                $script:ShadersMoved = $false
            } catch { $lost.Add($ShaderCache + '\') }
        }
        foreach ($rel in $created) {
            try { Remove-Item -LiteralPath (Join-Path $KitDir $rel) -Force -ErrorAction Stop } catch { $lost.Add($rel) }
        }
        foreach ($rel in $saved) {
            $from = Join-Path $Backup $rel
            $to = Join-Path $KitDir $rel
            try {
                # A file the failed write never changed (a running emulator's exe) is fine as it is.
                if ((Test-Path -LiteralPath $to) -and (Get-FileHash -LiteralPath $to).Hash -eq (Get-FileHash -LiteralPath $from).Hash) { continue }
                Copy-Item -LiteralPath $from -Destination $to -Force -ErrorAction Stop
            } catch { $lost.Add($rel) }
        }
        if ($lost.Count) {
            throw ("Updating failed ($why), and these could not be put back: $(($lost | Select-Object -First 5) -join ', ')" +
                   $(if ($lost.Count -gt 5) { " and $($lost.Count - 5) more" } else { '' }) +
                   ". Copy the files in $Backup back into the kit by hand, or run Update.bat again.")
        }
        Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
        throw "Updating failed, the old files are back in place: $why"
    }
    return $newFiles.Count
}

# --- The run ----------------------------------------------------------------------------

function Invoke-KitUpdate {
    $dir = $Kit
    if (-not $dir) { $dir = Split-Path -Parent $PSScriptRoot }
    $dir = [System.IO.Path]::GetFullPath($dir)
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'emulator'))) {
        throw "$dir is not a co-op kit (no emulator folder). Put Update.bat in the kit folder, beside Host.bat."
    }
    $mine = Get-KitVersion $dir
    $unfinished = Test-KitUnfinished $dir
    $have = if ($mine) { $mine } else { 'a test build' }
    $list = if ($Api) { $Api } else { "https://api.github.com/repos/$Repo/releases?per_page=30" }

    if ($Status) {
        # Straight to stdout: a function's own output would become its return value.
        if ($unfinished) { [Console]::Out.WriteLine('unfinished'); return 0 }
        try { $newest = Select-KitRelease (Read-KitReleaseList $list) } catch { return 0 }
        if ($newest -and (Compare-KitVersion $newest.Tag $mine) -gt 0) { [Console]::Out.WriteLine("newer $($newest.Tag)") }
        return 0
    }
    if ($Check) {
        if ($unfinished) {
            Write-Host 'An update of this kit did not finish: close the games and run Update.bat.'
            return 0
        }
        try { $newest = Select-KitRelease (Read-KitReleaseList $list) } catch { return 0 }
        if ($newest -and (Compare-KitVersion $newest.Tag $mine) -gt 0) {
            Write-Host "A new version is out: $($newest.Tag) (you have $have). Close the games and run Update.bat."
        }
        return 0
    }

    Write-Host "Kit: $dir"
    Write-Host "This kit: $have"
    if ($unfinished) { Write-Host 'An update of this kit did not finish: it is done again now.' }
    Write-Host 'Looking for the newest release...'
    $newest = Select-KitRelease (Read-KitReleaseList $list)
    if (-not $newest) { Write-Host 'No release with a kit was found.'; return 1 }
    if (-not $Api) {
        foreach ($u in $newest.Zip, $newest.Sum) {
            if (-not (Test-KitDownloadUrl $u $Repo)) { throw "The release list names a download outside this project's releases: $u" }
        }
    }
    if (-not $unfinished -and (Compare-KitVersion $newest.Tag $mine) -le 0) {
        Write-Host "You have the newest version ($mine)."
        return 0
    }
    Write-Host ("Newest: {0}, {1:N0} MB.  {2}" -f $newest.Tag, ($newest.Size / 1MB), $newest.Page)

    while ($true) {
        $running = Get-KitRunning $dir
        if (-not $running) { break }
        Write-Host "Close these first (they run from this kit): $(($running | ForEach-Object { $_.Name + '.exe' } | Sort-Object -Unique) -join ', ')"
        if ($Yes) { throw 'The emulator or server of this kit is still running.' }
        Read-Host 'Press Enter when they are closed' | Out-Null
    }
    if (-not $Yes) {
        $a = Read-Host "Update to $($newest.Tag)? Your saves, keys, settings and coop-options.ini are kept. [Y/n]"
        if ($a -and $a.Trim() -notmatch '^(y|yes)$') { Write-Host 'Nothing changed.'; return 0 }
    }

    $work = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-update-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        $zip = Join-Path $work "oot-coop-$($newest.Tag).zip"
        $sum = "$zip.sha256"
        Write-Host 'Downloading...'
        Get-KitDownload $newest.Sum $sum
        Get-KitDownload $newest.Zip $zip
        $want = Read-KitSha256 $sum
        $got = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($got -ne $want) { throw "The download is damaged (SHA-256 $got, the release says $want). Nothing changed; try again." }
        Write-Host "SHA-256 checked: $got"

        $unpacked = Join-Path $work 'kit'
        New-Item -ItemType Directory -Force -Path $unpacked | Out-Null
        $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
        if (Test-Path -LiteralPath $tar) {
            & $tar -x -f $zip -C $unpacked
            if ($LASTEXITCODE -ne 0) { throw "Unpacking failed (tar $LASTEXITCODE). Nothing changed." }
        } else {
            Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
        }
        $newKit = Get-KitUnpackedRoot $unpacked
        if (-not (Test-Path -LiteralPath (Join-Path $newKit 'emulator'))) { throw 'The release zip holds no kit. Nothing changed.' }
        $why = Test-KitEmulator (Join-Path $newKit 'emulator\lichii.exe')
        if ($why) { throw "The release zip is not a co-op kit ($why). Nothing changed." }

        # A stopped update may have written the new VERSION.txt already: the backup is named
        # after the version that update started from.
        $from = if ($unfinished) { Get-KitUnfinishedFrom $dir } else { '' }
        if (-not $from) { $from = $mine }
        $backup = Join-Path $dir ('backup-' + $(if ($from) { $from } else { 'old' }))
        if (Test-Path -LiteralPath $backup) { $backup += '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
        $newEmu = Test-KitEmulatorChanged (Join-Path $dir 'emulator\lichii.exe') (Join-Path $newKit 'emulator\lichii.exe')
        $n = Install-KitFiles $newKit $dir $backup $newest.Tag $newEmu $from
        Write-Host "Updated to $($newest.Tag): $n files. The replaced files are in $backup (delete it once the new version works)."
        if ($script:ShadersMoved) {
            Write-Host "The emulator changed, so its old shader cache was moved to $backup\$ShaderCache. The first minutes of play may stutter while a new one is built."
        } elseif ($unfinished -and -not (Test-Path -LiteralPath (Join-Path $dir $ShaderCache))) {
            # The stopped run moved it already, into its own backup folder.
            foreach ($s in Get-KitBackedUpShaders $dir) {
                Write-Host "The old shader cache, moved by the update that stopped, is in $s."
            }
        }
        if ($script:PackToolChanged) {
            Write-Host 'The puppet pack tool changed: run make-puppet-pack.cmd once more before you play, for the other players'' sounds and the latest puppet fixes.'
        }
        Write-Host 'Every player needs the same version: tell the others to update too.'
        if ($Launch) { return 3 }
        return 0
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (-not $NoRun) {
    try {
        exit (Invoke-KitUpdate)
    } catch {
        if ($Check -or $Status) { exit 0 }
        Write-Host "Update failed: $($_.Exception.Message)"
        exit 1
    }
}
