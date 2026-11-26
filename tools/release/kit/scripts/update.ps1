# Updates a co-op kit to the newest release, keeping everything of the player's.
#
# Update.bat runs this (make_kit_hookshot.ps1 puts this whole script inside Update.bat, so the one
# file also works dropped into an old kit that has no updater yet).
#
#   update.ps1 [-Kit <kit folder>] [-Yes]   update this kit (asks before it starts, unless -Yes)
#   update.ps1 -Check                       only say whether a newer release is out
#   update.ps1 -Status                      one line for play.ps1: "newer <version>", "unfinished",
#                                           "failed <why>" (no answer from GitHub) or nothing
#   update.ps1 -Launch                      update without asking, for play.ps1 when the player
#                                           said yes (exit code 3: updated, start the new kit);
#                                           this kit's own co-op server is stopped first
#   update.ps1 -Version v0.10.1             install that release instead of the newest (older ones too)
#   update.ps1 -Revert                      go back to the version this kit had before (Revert.bat)
#   update.ps1 -Tidy                        only point the player's shortcuts at the kit's start file (play.ps1 -NoUpdate;
#                                           -Status does it too): Update-KitStartLinks
#
# Going back (Revert.bat, -Revert, -Version with an older tag): every update keeps the kit's own files of
# the version it replaces in previous\<version>\ (the last 2; saves, keys, settings, logs and the built
# game are never in it), and -Revert puts the newest older one back (or a backup-<version>\ an older
# updater left, which holds the same files). This kit's updater (scripts\update.ps1, Update.bat,
# Revert.bat) stays the newer one, and stay-on-version.txt remembers the choice: the start no longer
# offers the version the player left, only a release newer than it (and then asks, auto_update = yes
# too). Update.bat goes forward again and forgets it.
#
# Every failure says why and how to update by hand (Get-KitManualSteps).
#
# play.ps1 (Host.bat, Join.bat, play.cmd) runs -Status on every start and, as coop-options.ini's
# auto_update says (ask, yes or no), -Launch; then it starts the new kit's own play.ps1.
#
# What it does: reads the kit's version from VERSION.txt, asks GitHub for the releases of
# daurosk/oot-remake-multiplayer (test builds count, drafts never) and takes the newest. If that
# is newer, it downloads the release's zip, checks it against the SHA-256 published beside it
# (Squadala-<version>-windows.zip.sha256; oot-coop-<version>-windows.zip.sha256 up to v0.11.2, oot-coop-<version>.zip.sha256
# before v0.11.0), unpacks it to a temp folder and copies the kit's own files
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
# coop-options.ini keeps your values; every key the new kit's coop-options.ini has and yours
# does not is appended with its comment, as the new kit has it (on or behind '#'). The merge
# is the NEW kit's own code (its scripts\update.ps1), so a key this updater does not know
# still arrives. The kit also carries scripts\coop-options.default.ini, and every run
# (-Status too, which play.ps1 runs on every start) adds missing keys from it: a kit an
# older updater left short gets them on its next start.
# Then, when this kit has scripts\verify_kit.ps1 and the GitHub CLI is logged in, the zip's
# release signature (attestation) is checked too; a zip that fails it is refused
# (verify_signature = no in coop-options.ini skips that check).
# No data about you is sent: one request for the release list and the two downloads (and, for
# the signature check, gh's lookup of the zip's attestation by its SHA-256).
#
# For tests: -Api (or the environment variable OOT_COOP_UPDATE_API, which play.ps1 passes on)
# names the release list (a URL or a local .json file) and the download addresses in it may
# be local files; -NoRun only loads the functions.
# OOT_COOP_VERIFY_NO_AUTH_CHECK=1 skips the 'gh auth status' step before the signature check.
param(
    [string]$Kit = '',
    [switch]$Yes,
    [switch]$Check,
    [switch]$Status,
    [switch]$Launch,
    [string]$Repo = 'daurosk/oot-remake-multiplayer',
    [string]$Api = '',
    [ValidatePattern('^(v?\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?)?$')][string]$Version = '',
    [switch]$Revert,
    [switch]$Tidy,
    [switch]$NoRun
)
if ($Version -and -not $Version.StartsWith('v')) { $Version = 'v' + $Version }

if (-not $Api -and $env:OOT_COOP_UPDATE_API) { $Api = $env:OOT_COOP_UPDATE_API }
if ($Launch) { $Yes = [switch]$true }
$UserAgent = 'oot-coop-updater'
$InProgress = 'update-in-progress.txt'
$Previous = 'previous'               # previous\<version>\: the kit's own files of the versions it had
$StayFile = 'stay-on-version.txt'    # the player went back: stay=<version>, left=<the newest they left>
$KeepPrevious = 2
# Not replaced when going back: the updater stays the newer one (it knows stay-on-version.txt; the old one would
# update straight back on the next start).
$UpdaterFiles = @('scripts\update.ps1', 'Update.bat', 'Revert.bat')
$KitPage = ''

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
# ("kit: Squadala-v0.11.3 (release v0.11.3)", "kit: oot-coop-..." up to v0.11.2); '' for a kit that is no release.
function Get-KitVersion([string]$KitDir) {
    $file = Join-Path $KitDir 'VERSION.txt'
    if (-not (Test-Path -LiteralPath $file)) { return '' }
    foreach ($line in [System.IO.File]::ReadAllLines($file)) {
        if ($line -match '\(release ([^)]+)\)') { return $Matches[1] }
        if ($line -match '^kit: (?:Squadala|oot-coop)-(v\d[^ ]*)') { return $Matches[1] }
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

# One line for a failure: the error's first line, and what a refusal from GitHub means.
function Get-KitErrorText($Err) {
    $text = [string]$Err
    if ($Err -is [System.Management.Automation.ErrorRecord] -and $Err.Exception) { $text = $Err.Exception.Message }
    $text = ($text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    if (-not $text) { $text = 'unknown error' }
    $text = $text.Trim()
    if ($text -match '\b403\b|rate limit') {
        $text = "GitHub turned the request away ($text); it allows 60 checks an hour from one internet address: try again later"
    }
    if ($text.Length -gt 200) { $text = $text.Substring(0, 200) + '...' }
    return $text
}

# How to update by hand, for every failure message.
function Get-KitManualSteps([string]$Page) {
    $where = if ($Page -and $Page -match '^https://') { $Page } else { "https://github.com/$Repo/releases" }
    return [string[]]@(
        'To update by hand:',
        "  1. Close the games and the server window, and download the newest Squadala-...-windows.zip from $where",
        '  2. Unzip it to a new folder with a short path, such as C:\Squadala-new.',
        '  3. Copy the folder emulator\user (keys, firmware, saves, settings, your name, the puppet pack)',
        '     and the file coop-options.ini from this kit into the new folder. Missing switches are added',
        '     to coop-options.ini when you start the new kit.',
        '  4. Play from the new folder ("Start Squadala.bat", or Host.bat / Join.bat in an emulator kit).')
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

# The Windows kit zip of a release and its .sha256: Squadala-<tag>-windows.zip from v0.11.3 on,
# oot-coop-<tag>-windows.zip from v0.11.0 to v0.11.2, else oot-coop-<tag>.zip (every release before, and the same
# file again in later ones, for the older kits: up to v0.10.1 their updater knows only that name, and from v0.11.3
# it is the one the v0.11.0 to v0.11.2 updaters find). @(zip asset, sha256 asset) or $null.
function Get-KitReleaseZip($Release, [string]$Tag) {
    foreach ($n in "Squadala-$Tag-windows.zip", "oot-coop-$Tag-windows.zip", "oot-coop-$Tag.zip") {
        $zip = @($Release.assets) | Where-Object { $_.name -eq $n } | Select-Object -First 1
        $sum = @($Release.assets) | Where-Object { $_.name -eq "$n.sha256" } | Select-Object -First 1
        if ($zip -and $sum) { return @($zip, $sum) }
    }
    return $null
}

# The newest release that has a Windows kit zip and its .sha256 beside it; drafts never.
function Select-KitRelease($Releases) {
    $best = $null
    foreach ($r in @($Releases)) {
        if ($r.draft) { continue }
        $tag = [string]$r.tag_name
        if (-not (ConvertTo-KitVersion $tag)) { continue }
        $pair = Get-KitReleaseZip $r $tag
        if (-not $pair) { continue }
        $zip = $pair[0]; $sum = $pair[1]
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
    if ($p.StartsWith($Previous + '\', [StringComparison]::OrdinalIgnoreCase) -or $p -ieq $StayFile) { return $true }
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

# The key names a coop-options.ini mentions, set or behind a '#' ('name =' or 'name='):
# every key, not a list of known ones, so a key a later kit adds is never missed (an older
# updater knew only some names and so never added server_allow).
$OptionName = '([A-Za-z_][A-Za-z0-9_]*)'
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

# The player's coop-options.ini lines with every key the new template names and the
# player's file does not, appended as the template has it (on, or behind '#') with the
# template's comment above it. The player's own lines stay as they are; nothing is dropped.
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
                $added.Add($t)
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
        $out.Add("# --- Added by Update.bat ($Version): keys new in this version, as a new kit has them. A line with '#' in front is off. ---")
        foreach ($a in $added) { $out.Add($a) }
    }
    return ,([string[]]$out.ToArray())
}

# The merge as the new kit does it: its own scripts\update.ps1 is loaded (only its functions)
# and its Merge-KitOptions used, so keys and rules this older updater does not know still
# reach the player's file. When it has none or fails: this one's.
function Merge-KitOptionsWith([string]$NewKit, [string[]]$Mine, [string[]]$New, [string]$Version) {
    $newer = Join-Path $NewKit 'scripts\update.ps1'
    if ($NewKit -and (Test-Path -LiteralPath $newer)) {
        try {
            $r = & {
                param($f, $a, $b, $v)
                . $f -NoRun
                Merge-KitOptions $a $b $v
            } $newer $Mine $New $Version
            $r = [string[]]@($r)
            # Never fewer lines than the player had: a merge that drops one is not used.
            if ($r.Count -ge $Mine.Count) { return ,$r }
        } catch { }
    }
    return ,(Merge-KitOptions $Mine $New $Version)
}

# The kit's own copy of the options a fresh kit has (make_kit_hookshot.ps1).
$OptionsDefault = 'scripts\coop-options.default.ini'

# Adds to the kit's coop-options.ini every key scripts\coop-options.default.ini has and it
# does not (a kit an older updater left short); writes a coop-options.ini when there is none.
# True when it changed the file. Never throws.
function Update-KitOptions([string]$KitDir) {
    try {
        $tmpl = Join-Path $KitDir $OptionsDefault
        $mine = Join-Path $KitDir 'coop-options.ini'
        if (-not (Test-Path -LiteralPath $tmpl)) { return $false }
        if (-not (Test-Path -LiteralPath $mine)) { Copy-Item -LiteralPath $tmpl -Destination $mine; return $true }
        $lines = [System.IO.File]::ReadAllLines($mine)
        $v = Get-KitVersion $KitDir
        $merged = Merge-KitOptions $lines ([System.IO.File]::ReadAllLines($tmpl)) $(if ($v) { $v } else { 'this kit' })
        if ($merged.Count -eq $lines.Count) { return $false }
        # Written aside and moved over: -Status can be stopped at any moment, and a
        # cut coop-options.ini would lose the player's settings.
        $part = "$mine.part"
        [System.IO.File]::WriteAllLines($part, $merged)
        Move-Item -LiteralPath $part -Destination $mine -Force
        return $true
    } catch { return $false }
}

# --- Installing -------------------------------------------------------------------------

# coop-options.ini's verify_signature: 'no' skips the signature check, anything else is 'auto'.
function Get-KitVerifySignature([string]$KitDir) {
    $file = Join-Path $KitDir 'coop-options.ini'
    if (Test-Path -LiteralPath $file) {
        foreach ($line in [System.IO.File]::ReadAllLines($file)) {
            if ($line -match '^\s*verify_signature\s*=\s*no\s*$') { return 'no' }
        }
    }
    return 'auto'
}

# The release signature (issue 161): this kit's OWN scripts\verify_kit.ps1 (never the one in the
# download, which is not trusted yet) checks the zip's attestation with the GitHub CLI. Its exit
# codes: 0 good, 1 does not check, 2 could not check. Returns 'good', 'none' (no verifier in
# this kit, gh missing or not logged in, or verify_signature = no) or throws when it does not
# check. A kit from before the verifier has none: the SHA-256 is then the only check.
function Test-KitSignature([string]$KitDir, [string]$Zip) {
    if ((Get-KitVerifySignature $KitDir) -eq 'no') { Write-Host 'Signature check skipped (verify_signature = no).'; return 'none' }
    $verifier = Join-Path $KitDir 'scripts\verify_kit.ps1'
    if (-not (Test-Path -LiteralPath $verifier -PathType Leaf)) { return 'none' }
    # gh that is not logged in fails every check: that is "could not check", not a bad file.
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $env:OOT_COOP_VERIFY_NO_AUTH_CHECK -and $gh) {
        & $gh.Source auth status *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Signature not checked: the GitHub CLI is not logged in ('gh auth login'). The SHA-256 is checked."
            return 'none'
        }
    }
    $ps = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $ps)) { $ps = 'powershell' }
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $verifier -Zip $Zip -Repo $Repo 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    if ($code -eq 0) { return 'good' }
    if ($code -eq 1) {
        $last = @($out | Where-Object { $_ -match 'FAIL' } | Select-Object -Last 1)
        if ($last) { Write-Host $last[0] }
        Write-Host 'A release only minutes old may not be signed yet: try again later. To update on the SHA-256 alone,'
        Write-Host "put 'verify_signature = no' in coop-options.ini."
        throw "The download's release signature does not check. Nothing changed."
    }
    $why = @($out | Where-Object { $_ -match 'Could not' } | Select-Object -First 1)
    Write-Host "Signature not checked$(if ($why) { ': ' + $why[0] } else { '' }) The SHA-256 is checked."
    return 'none'
}

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

# The start file of a Hookshot-only kit: "Start Squadala.bat" from v0.11.3 on, else "Start OoT Co-op.bat" (the name up to
# v0.11.2; v0.11.3 kept it as a one-line call of the new one, v0.11.4 drops it); '' when there is neither.
$StartNames = @('Start Squadala.bat', 'Start OoT Co-op.bat')
function Get-KitStartFile([string]$Dir) {
    foreach ($n in $StartNames) {
        $f = Join-Path $Dir $n
        if (Test-Path -LiteralPath $f) { return $f }
    }
    return ''
}

# Where the player's shortcuts to a kit can be: the desktop (theirs and the shared one), the Start menu (theirs and the
# shared one) and the taskbar pins. OOT_COOP_SHORTCUT_DIRS (';'-separated) instead, for tests; OOT_COOP_DESKTOP alone
# (older tests) means only that folder.
function Get-KitShortcutDirs {
    if ($env:OOT_COOP_SHORTCUT_DIRS) { return @($env:OOT_COOP_SHORTCUT_DIRS -split ';' | Where-Object { $_ }) }
    if ($env:OOT_COOP_DESKTOP) { return @($env:OOT_COOP_DESKTOP) }
    $dirs = @('Desktop', 'CommonDesktopDirectory', 'Programs', 'CommonPrograms') | ForEach-Object { [Environment]::GetFolderPath($_) }
    if ($env:APPDATA) { $dirs += Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned' }
    return @($dirs | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
}

# Steam's non-Steam game list of each Steam user (userdata\<id>\config\shortcuts.vdf). OOT_COOP_STEAM_DIR names the Steam
# folder (tests); OOT_COOP_SHORTCUT_DIRS or OOT_COOP_DESKTOP without it means none (a test never touches the PC's Steam).
function Get-KitSteamLists {
    $steam = $env:OOT_COOP_STEAM_DIR
    if (-not $steam) {
        if ($env:OOT_COOP_SHORTCUT_DIRS -or $env:OOT_COOP_DESKTOP) { return @() }
        try { $steam = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch { return @() }
    }
    if (-not $steam -or -not (Test-Path -LiteralPath (Join-Path $steam 'userdata'))) { return @() }
    return @(Get-ChildItem -LiteralPath (Join-Path $steam 'userdata') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'config\shortcuts.vdf' } | Where-Object { Test-Path -LiteralPath $_ })
}

# Every way in to this kit follows its start file. A shortcut (desktop, Start menu, taskbar pin) or Steam entry that
# starts a launcher of the kit that is gone, or the old name "Start OoT Co-op.bat" once this version's MANIFEST.txt no
# longer has it (v0.11.4 on), now starts the kit's start file (Get-KitStartFile): forward after an update (the emulator
# kits' Play-OoT-Coop.bat, Host.bat and Join.bat too) and back after Revert.bat to a kit up to v0.11.2. Then that
# old-name file goes (an updater up to v0.11.3 removes it already, by MANIFEST.txt; a kit going back to v0.11.3 or older
# gets it back from previous\ and keeps it). Runs on every start (update.ps1 -Status / -Tidy) and after every update or
# Revert.bat. Returns the lines to say.
function Update-KitStartLinks([string]$KitDir) {
    $start = Get-KitStartFile $KitDir
    if (-not $start) { return @() }
    $startName = Split-Path -Leaf $start
    $root = [System.IO.Path]::GetFullPath($KitDir).TrimEnd('\')
    $old = Join-Path $root 'Start OoT Co-op.bat'
    $manifest = Join-Path $root 'MANIFEST.txt'
    $retire = ($startName -eq 'Start Squadala.bat') -and (Test-Path -LiteralPath $old) -and (Test-Path -LiteralPath $manifest) -and
        -not [bool](@([System.IO.File]::ReadAllLines($manifest)) | Where-Object { $_.Trim() -ieq 'Start OoT Co-op.bat' })
    $lines = @()
    $isOld = {
        param([string]$Target)
        if (-not $Target) { return $false }
        try { $t = [System.IO.Path]::GetFullPath($Target) } catch { return $false }
        if (-not ([System.IO.Path]::GetDirectoryName($t)).Equals($root, [StringComparison]::OrdinalIgnoreCase)) { return $false }
        if ($t.Equals($start, [StringComparison]::OrdinalIgnoreCase)) { return $false }
        if ($retire -and $t.Equals($old, [StringComparison]::OrdinalIgnoreCase)) { return $true }
        return ($t -match '(?i)\.(bat|cmd)$') -and -not (Test-Path -LiteralPath $t)
    }
    $shell = $null
    $leaf = Split-Path -Leaf $root
    # Bounded (0.11.9): a Desktop holding project folders took 30 s to walk with -Recurse, which made -Status miss
    # play.ps1's 15 s. Shortcuts sit at most 2 folders down (Start menu\Programs\Folder\x.lnk), and the whole scan stops
    # after 8 s whatever is left.
    $deadline = [DateTime]::UtcNow.AddSeconds($(if ($env:OOT_COOP_LINKS_SECONDS) { [double]$env:OOT_COOP_LINKS_SECONDS } else { 8 }))
    foreach ($d in Get-KitShortcutDirs) {
        if ([DateTime]::UtcNow -gt $deadline) { break }
        foreach ($f in @(Get-ChildItem -LiteralPath $d -Filter '*.lnk' -File -Recurse -Depth 3 -ErrorAction SilentlyContinue)) {
            if ([DateTime]::UtcNow -gt $deadline) { break }
            try {
                # (only a shortcut that names this kit's folder is opened: the Start menu holds hundreds)
                $raw = [System.IO.File]::ReadAllBytes($f.FullName)
                $named = $false
                foreach ($t in [System.Text.Encoding]::Unicode.GetString($raw), [System.Text.Encoding]::Default.GetString($raw),
                        $(if ($raw.Length -gt 1) { [System.Text.Encoding]::Unicode.GetString($raw, 1, $raw.Length - 1) } else { '' })) {
                    if ($t.IndexOf($leaf, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $named = $true; break }
                }
                if (-not $named) { continue }
                if (-not $shell) { $shell = New-Object -ComObject WScript.Shell }
                $s = $shell.CreateShortcut($f.FullName)
                if (-not (& $isOld $s.TargetPath)) { continue }
                $s.TargetPath = $start; $s.WorkingDirectory = $root; $s.Save()
                $lines += "The shortcut ""$($f.BaseName)"" now starts ""$startName""."
            } catch { }
        }
    }
    # Steam keeps each path as plain UTF-8 text between the binary fields (no lengths), so a path is swapped in place.
    $latin = [System.Text.Encoding]::GetEncoding(28591)
    $names = @('Start Squadala.bat', 'Start OoT Co-op.bat', 'Play-OoT-Coop.bat', 'Host.bat', 'Join.bat') | Where-Object { $_ -ne $startName }
    foreach ($vdf in Get-KitSteamLists) {
        try {
            $text = $latin.GetString([System.IO.File]::ReadAllBytes($vdf))
            $new = $text
            foreach ($n in $names) {
                $p = Join-Path $root $n
                if (-not (& $isOld $p)) { continue }
                $from = $latin.GetString([System.Text.Encoding]::UTF8.GetBytes($p))
                $to = $latin.GetString([System.Text.Encoding]::UTF8.GetBytes($start))
                $new = [regex]::Replace($new, [regex]::Escape($from), { param($m) $to }, 'IgnoreCase')
            }
            if ($new -ceq $text) { continue }
            Copy-Item -LiteralPath $vdf -Destination "$vdf.before-squadala" -Force
            [System.IO.File]::WriteAllBytes("$vdf.new", $latin.GetBytes($new))
            Move-Item -LiteralPath "$vdf.new" -Destination $vdf -Force
            $lines += "Your Steam entry for this kit now starts ""$startName""" +
                $(if (Get-Process -Name steam -ErrorAction SilentlyContinue) { ' (after Steam restarts; until then start it from the kit folder).' } else { '.' })
        } catch { }
    }
    if ($retire) { try { Remove-Item -LiteralPath $old -Force -ErrorAction Stop } catch { } }
    return $lines
}

# A co-op kit folder: an emulator kit (emulator\) or a Hookshot-only kit (make_kit_hookshot.ps1: hookshot\PIN and its launcher).
function Test-KitFolder([string]$Dir) {
    if (Test-Path -LiteralPath (Join-Path $Dir 'emulator')) { return $true }
    return (Test-Path -LiteralPath (Join-Path $Dir 'hookshot\PIN')) -and [bool](Get-KitStartFile $Dir)
}

# Why an unpacked release must not be installed, or ''. An emulator kit: its emulator (Test-KitEmulator).
# A Hookshot-only kit: the co-op module, the Hookshot pin, the launcher and its script.
function Test-KitNewKit([string]$Dir) {
    if (Test-Path -LiteralPath (Join-Path $Dir 'emulator')) { return (Test-KitEmulator (Join-Path $Dir 'emulator\lichii.exe')) }
    foreach ($f in 'hookshot\hookshot_coop.dll', 'hookshot\PIN', 'scripts\start.ps1') {
        if (-not (Test-Path -LiteralPath (Join-Path $Dir $f))) { return "it has no $f" }
    }
    if (-not (Get-KitStartFile $Dir)) { return 'it has no Start Squadala.bat' }
    return ''
}

# After an update or a going back: every shortcut follows the kit's start file (Update-KitStartLinks), and after an
# emulator kit became a Hookshot-only kit the player hears that emulator\user (theirs: saves, keys, settings) is kept.
# Returns the lines to say.
function Complete-KitHookshotOnly([string]$KitDir, [bool]$HadEmulator) {
    $start = Get-KitStartFile $KitDir
    if (-not $start) { return @() }
    $startName = Split-Path -Leaf $start
    $lines = @(Update-KitStartLinks $KitDir)
    if ($HadEmulator -and -not (Test-Path -LiteralPath (Join-Path $KitDir 'emulator\lichii.exe'))) {
        $lines += 'This version is Longshot only (no emulator): the emulator and its .bat files were removed (they are in the backup folder).'
        if (Test-Path -LiteralPath (Join-Path $KitDir 'emulator\user')) {
            $lines += 'Your emulator files (emulator\user: its saves, keys and settings) were kept; delete that folder when you no longer need them.'
        }
        $lines += "From now on start the game with ""$startName""."
    }
    return $lines
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
    return @(Get-Process -Name lichii, coop_server, longshot, hookshot -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)
    })
}

# The one folder at the top of an unpacked release zip (Squadala-<version>\, oot-coop-<version>\ up to v0.11.2).
function Get-KitUnpackedRoot([string]$Dir) {
    $top = @(Get-ChildItem -LiteralPath $Dir -Force)
    if ($top.Count -eq 1 -and $top[0].PSIsContainer) { return $top[0].FullName }
    return $Dir
}

# Copies the new kit's own files over KitDir. Every file replaced or removed goes to
# Backup first; on failure everything is put back. With MoveShaders the shader cache is moved
# to Backup before anything else (and back on failure). Returns the number of files written.
# Copy-Item, tried again for about 3 s when the file is held: an antivirus or the search indexer opens a file it has
# just seen being written (the kit's own exe a moment ago) for a short scan, and one write in that moment failed the
# whole update (launch_test's update checks, on a PC's first run). A file held for longer (a running emulator) still fails.
function Copy-KitFile([string]$From, [string]$To) {
    for ($try = 1; ; $try++) {
        try { Copy-Item -LiteralPath $From -Destination $To -Force -ErrorAction Stop; return }
        catch { if ($try -ge 12) { throw } }
        Start-Sleep -Milliseconds 250
    }
}

# $Leave: kit-relative files left as they are (neither written nor removed); $NoOptions: coop-options.ini is not merged
# (going back: the player's file keeps its newer keys, an older kit ignores them).
function Install-KitFiles([string]$NewKit, [string]$KitDir, [string]$Backup, [string]$Version, [bool]$MoveShaders = $false, [string]$From = '',
                          [string[]]$Leave = @(), [bool]$NoOptions = $false) {
    $NewKit = [System.IO.Path]::GetFullPath($NewKit).TrimEnd('\')
    $left = @{}
    foreach ($k in $Leave) { if (Test-Path -LiteralPath (Join-Path $KitDir $k) -PathType Leaf) { $left[$k.ToLowerInvariant()] = $true } }
    $newFiles = Get-KitInstallOrder @(Get-KitFiles $NewKit | Where-Object { -not (Test-KitPlayerPath $_) -and -not $left.ContainsKey($_.ToLowerInvariant()) })
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
            -not $keep.ContainsKey($_.ToLowerInvariant()) -and -not $left.ContainsKey($_.ToLowerInvariant()) -and (Test-Path -LiteralPath (Join-Path $KitDir $_) -PathType Leaf)
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
        if ($NoOptions) {
            # (kept as it is)
        } elseif ((Test-Path -LiteralPath $mine) -and (Test-Path -LiteralPath $tmpl)) {
            $lines = [System.IO.File]::ReadAllLines($mine)
            $merged = Merge-KitOptionsWith $NewKit $lines ([System.IO.File]::ReadAllLines($tmpl)) $Version
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
            Copy-KitFile (Join-Path $NewKit $rel) $dest
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
                Copy-KitFile $from $to
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

# --- Going back -------------------------------------------------------------------------

# stay-on-version.txt when it is about this kit's version ($Mine): @{ Stay; Left }, else $null (a file about another
# version, as after the player copied old files in by hand, counts for nothing).
function Get-KitStay([string]$KitDir, [string]$Mine) {
    $file = Join-Path $KitDir $StayFile
    if (-not $Mine -or -not (Test-Path -LiteralPath $file)) { return $null }
    $r = @{ Stay = ''; Left = '' }
    foreach ($line in [System.IO.File]::ReadAllLines($file)) {
        if ($line -match '^\s*stay\s*=\s*(\S+)\s*$' -and (ConvertTo-KitVersion $Matches[1])) { $r.Stay = $Matches[1] }
        if ($line -match '^\s*left\s*=\s*(\S+)\s*$' -and (ConvertTo-KitVersion $Matches[1])) { $r.Left = $Matches[1] }
    }
    if (-not $r.Stay -or -not $r.Left -or (Compare-KitVersion $r.Stay $Mine) -ne 0) { return $null }
    return $r
}

function Write-KitStay([string]$KitDir, [string]$Stay, [string]$Left) {
    [System.IO.File]::WriteAllText((Join-Path $KitDir $StayFile), (
        "# You went back to $Stay on purpose (Revert.bat or Update.bat -Version). Starting the kit does not update to`r`n" +
        "# $Left or older again; a newer release is asked about first. Update.bat goes forward and removes this file.`r`n" +
        "stay=$Stay`r`nleft=$Left`r`n"))
}

# The kit's own files as they are now (kit-relative): what its MANIFEST.txt lists and what of $Also is here, never
# the player's (Test-KitPlayerPath). Exactly what an update replaces or removes, so previous\<version>\ can put it back.
function Get-KitOwnFiles([string]$KitDir, [string[]]$Also) {
    $names = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    $manifest = Join-Path $KitDir 'MANIFEST.txt'
    $from = @($Also) + @('VERSION.txt', 'MANIFEST.txt')
    if (Test-Path -LiteralPath $manifest) { $from += @([System.IO.File]::ReadAllLines($manifest) | ForEach-Object { $_.Trim() }) }
    foreach ($rel in $from) {
        if (-not $rel -or $rel.StartsWith('#') -or -not (Test-KitRelativePath $rel) -or (Test-KitPlayerPath $rel)) { continue }
        if ($seen.ContainsKey($rel.ToLowerInvariant())) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $KitDir $rel) -PathType Leaf)) { continue }
        $seen[$rel.ToLowerInvariant()] = $true
        $names.Add($rel)
    }
    return ,([string[]]$names.ToArray())
}

# A version that may name a folder (previous\<version>\, backup-<version>\): a release version (ConvertTo-KitVersion),
# with no '..' and no trailing dot, so a damaged or hand-edited VERSION.txt ('(release ..)', '(release ..\..)') never
# becomes a path outside the kit.
function Test-KitSafeVersion([string]$Ver) {
    return [bool](ConvertTo-KitVersion $Ver) -and -not $Ver.Contains('..') -and -not $Ver.EndsWith('.')
}

# Removes a folder, only one directly inside the kit's previous\ (never the kit, never above it).
function Remove-KitPreviousDir([string]$KitDir, [string]$Path) {
    $root = [System.IO.Path]::GetFullPath((Join-Path $KitDir $Previous)).TrimEnd('\')
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ((Split-Path -Parent $full) -ine $root -or (Split-Path -Leaf $full) -in '', '.', '..') { throw "Not removed (outside $Previous\): $Path" }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}

# Copies $Files of the kit to previous\<version>.part\ (Complete-KitPrevious names it previous\<version>\ once the
# update went through). Returns that folder, or '' for a kit with no version.
function Save-KitPrevious([string]$KitDir, [string]$Ver, [string[]]$Files) {
    if (-not (Test-KitSafeVersion $Ver) -or -not $Files) { return '' }
    $part = Join-Path (Join-Path $KitDir $Previous) "$Ver.part"
    Remove-KitPreviousDir $KitDir $part
    foreach ($rel in $Files) {
        $to = Join-Path $part $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
        Copy-Item -LiteralPath (Join-Path $KitDir $rel) -Destination $to -Force
    }
    return $part
}

# previous\<version>.part\ becomes previous\<version>\ (an older copy of that version goes), and only the last
# $KeepPrevious stay. Returns the folder, or ''.
function Complete-KitPrevious([string]$KitDir, [string]$Part) {
    if (-not $Part -or -not (Test-Path -LiteralPath $Part)) { return '' }
    $dest = $Part.Substring(0, $Part.Length - '.part'.Length)
    Remove-KitPreviousDir $KitDir $dest
    Move-Item -LiteralPath $Part -Destination $dest
    (Get-Item -LiteralPath $dest).LastWriteTime = Get-Date
    $root = Join-Path $KitDir $Previous
    Get-ChildItem -LiteralPath $root -Directory -Filter '*.part' -Force | ForEach-Object { try { Remove-KitPreviousDir $KitDir $_.FullName } catch { } }
    @(Get-ChildItem -LiteralPath $root -Directory -Force | Sort-Object LastWriteTime -Descending | Select-Object -Skip $KeepPrevious) |
        ForEach-Object { try { Remove-KitPreviousDir $KitDir $_.FullName } catch { } }
    return $dest
}

# The versions this kit can go back to without a download: previous\<version>\ (newest first), then the backup-*\
# folders updates left (an update copies every file it replaces or removes there, so one holds that whole version;
# older updaters made only those). Each: @{ Version; Dir }; only complete kits (Test-KitNewKit).
function Get-KitKept([string]$KitDir) {
    $dirs = @(Get-ChildItem -LiteralPath (Join-Path $KitDir $Previous) -Directory -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike '*.part' } | Sort-Object LastWriteTime -Descending) +
        @(Get-ChildItem -LiteralPath $KitDir -Directory -Filter 'backup-*' -Force -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
    $out = @()
    $seen = @{}
    foreach ($d in $dirs) {
        $v = Get-KitVersion $d.FullName
        if (-not (ConvertTo-KitVersion $v) -or $seen.ContainsKey($v)) { continue }
        if (-not (Test-KitFolder $d.FullName) -or (Test-KitNewKit $d.FullName) -or -not (Test-KitComplete $d.FullName)) { continue }
        $seen[$v] = $true
        $out += , @{ Version = $v; Dir = $d.FullName }
    }
    return , $out
}

# A kept kit folder that holds its whole version: every file its own MANIFEST.txt names is in it (a backup-*\ of an
# update that failed while copying holds only some). One without a MANIFEST.txt (kits before the updater) counts.
function Test-KitComplete([string]$Dir) {
    $manifest = Join-Path $Dir 'MANIFEST.txt'
    if (-not (Test-Path -LiteralPath $manifest)) { return $true }
    foreach ($l in [System.IO.File]::ReadAllLines($manifest)) {
        $rel = $l.Trim()
        if (-not $rel -or $rel.StartsWith('#') -or -not (Test-KitRelativePath $rel) -or (Test-KitPlayerPath $rel)) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $Dir $rel) -PathType Leaf)) { return $false }
    }
    return $true
}

# How the co-op game changes from kit $A to kit $B: 'same' (only co-op itself: seconds on the next start), 'patches'
# (its co-op patches: the next start updates the game, a few minutes) or 'pin' (another Longshot release or commit:
# built again, 15 to 45 minutes; the start asks first). An emulator kit on either side has no such game: 'same'.
function Get-KitGameChange([string]$A, [string]$B) {
    $pin = { param($k) (@('PIN', 'TOOLS.txt') | ForEach-Object { @(Get-Content -LiteralPath (Join-Path $k "hookshot\$_") -ErrorAction SilentlyContinue) } |
        Where-Object { $_ -match '^\s*(release|commit)\s*=' } | ForEach-Object { $_.Trim() }) -join "`n" }
    $patches = { param($k) (@(Get-ChildItem -LiteralPath (Join-Path $k 'hookshot\patches') -Filter '*.patch' -File -ErrorAction SilentlyContinue |
        Sort-Object Name | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash })) -join ',' }
    $pa = & $pin $A; $pb = & $pin $B
    if (-not $pa -or -not $pb) { return 'same' }   # (no game of its own on one side: an emulator kit)
    if ($pa -ne $pb) { return 'pin' }
    if ((& $patches $A) -ne (& $patches $B)) { return 'patches' }
    return 'same'
}

function Get-KitGameChangeText([string]$Change, [string]$To) {
    switch ($Change) {
        'pin' { "The co-op game is built again for $To on the next start (Longshot changed between these versions: 15 to 45 minutes; it asks first). Your saves are kept." }
        'patches' { "The next start updates the co-op game for $To (a few minutes). Your saves are kept." }
        default { 'The co-op game stays as it is (only the co-op part changes, seconds on the next start).' }
    }
}

function Get-KitSameVersionText([string]$To) {
    return [string[]]@(
        "Every player in a session needs the same version: $To does not talk to other versions (the co-op protocol differs).",
        "Tell your friends to go back too (Revert.bat, or Update.bat -Version $To), or they cannot join you, nor you them.")
}

# Yes or no, $Default when no key comes in $Seconds (or no answer comes from a redirected input).
function Read-KitYesNo([string]$Question, [int]$Seconds, [bool]$Default) {
    $hint = if ($Default) { '[Y/n]' } else { '[y/N]' }
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected } catch { }
    if ($redirected) {
        Write-Host "$Question $hint"
        $line = [Console]::In.ReadLine()
        if (-not $line -or -not $line.Trim()) { return $Default }
        return $line.Trim() -match '^(?i)(y|yes)$'
    }
    Write-Host -NoNewline "$Question $hint ($(if ($Default) { 'yes' } else { 'no' }) in $Seconds s) "
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq 'N') { Write-Host 'n'; return $false }
            if ($key.Key -eq 'Y') { Write-Host 'y'; return $true }
            if ($key.Key -eq 'Enter') { Write-Host $(if ($Default) { 'y' } else { 'n' }); return $Default }
        }
        Start-Sleep -Milliseconds 100
    }
    Write-Host $(if ($Default) { 'y' } else { 'n' })
    return $Default
}

# Puts the kit $Source (an unpacked release, or a kept one) in place of this kit's files. Keeps this version in
# previous\<version>\ first. $Back: going back (the updater files stay, coop-options.ini is not merged, and
# stay-on-version.txt is written); else stay-on-version.txt goes. @{ N; Backup; Kept; HadEmu; Game }.
function Invoke-KitSwitch([string]$Dir, [string]$Source, [string]$To, [string]$Mine, [bool]$Back, [bool]$Unfinished) {
    # A stopped update may have written the new VERSION.txt already: the backup is named
    # after the version that update started from.
    $from = if ($Unfinished) { Get-KitUnfinishedFrom $Dir } else { '' }
    if (-not $from) { $from = $Mine }
    if (-not (Test-KitSafeVersion $from)) { $from = '' }
    $backup = Join-Path $Dir ('backup-' + $(if ($from) { $from } else { 'old' }))
    if (Test-Path -LiteralPath $backup) { $backup += '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
    # (a Hookshot-only kit has no emulator: the old emulator's cache stays in emulator\user, the player's)
    $newEmu = (Test-Path -LiteralPath (Join-Path $Source 'emulator\lichii.exe')) -and
        (Test-KitEmulatorChanged (Join-Path $Dir 'emulator\lichii.exe') (Join-Path $Source 'emulator\lichii.exe'))
    $hadEmu = Test-Path -LiteralPath (Join-Path $Dir 'emulator\lichii.exe')
    $game = Get-KitGameChange $Dir $Source
    # This version, so the player can come back to it (not from a half-done kit: that is no one version).
    $part = ''
    if (-not $Unfinished -and $Mine) {
        try { $part = Save-KitPrevious $Dir $Mine (Get-KitOwnFiles $Dir (Get-KitFiles $Source)) } catch {
            Write-Host "Could not keep $Mine in $Previous\ ($(Get-KitErrorText $_)); going on without it."
            $part = ''
        }
    }
    $keep = if ($Back) { $UpdaterFiles } else { @() }
    try { $n = Install-KitFiles $Source $Dir $backup $To $newEmu $from $keep $Back } catch {
        if ($part) { try { Remove-KitPreviousDir $Dir $part } catch { } }
        throw
    }
    $kept = ''
    try { $kept = Complete-KitPrevious $Dir $part } catch { }
    $stayPath = Join-Path $Dir $StayFile
    if ($Back) {
        $old = Get-KitStay $Dir $Mine
        $left = if ($old -and (Compare-KitVersion $old.Left $Mine) -gt 0) { $old.Left } else { $Mine }
        if (-not $left) { $left = $To }
        Write-KitStay $Dir $To $left
    } elseif (Test-Path -LiteralPath $stayPath) {
        Remove-Item -LiteralPath $stayPath -Force -ErrorAction SilentlyContinue
    }
    return @{ N = $n; Backup = $backup; Kept = $kept; HadEmu = $hadEmu; Game = $game }
}

# Waits until nothing runs from this kit ($Yes: fails instead).
function Wait-KitNotRunning([string]$Dir) {
    while ($true) {
        $running = Get-KitRunning $Dir
        if (-not $running) { return }
        Write-Host "Close these first (they run from this kit): $(($running | ForEach-Object { $_.Name + '.exe' } | Sort-Object -Unique) -join ', ')"
        if ($Yes) { throw "This kit's game or server is still running: close it, then run this again." }
        Read-Host 'Press Enter when they are closed' | Out-Null
    }
}

# A release by its tag ($Tag, 'v0.10.1'), in the release list or else asked for on its own (older than the
# list's 30); as Select-KitRelease gives it, or $null.
function Find-KitRelease([string]$List, [string]$Tag) {
    $all = @(Read-KitReleaseList $List | ForEach-Object { $_ })
    $rel = @($all | Where-Object { [string]$_.tag_name -ieq $Tag }) | Select-Object -First 1
    if (-not $rel -and -not $Api) {
        try { $rel = Read-KitReleaseList "https://api.github.com/repos/$Repo/releases/tags/$Tag" } catch { $rel = $null }
    }
    if (-not $rel) { return $null }
    return Select-KitRelease @($rel)
}

# Revert.bat (update.ps1 -Revert): back to the newest kept version older than this one.
function Invoke-KitRevert([string]$Dir, [string]$Mine, [bool]$Unfinished) {
    Write-Host "Kit: $Dir"
    Write-Host "This kit: $(if ($Mine) { $Mine } else { 'a test build' })"
    if ($Unfinished) { throw 'An update of this kit did not finish: run Update.bat to finish it first, then Revert.bat.' }
    $best = $null
    foreach ($k in @(Get-KitKept $Dir)) {
        if ($Mine -and (Compare-KitVersion $k.Version $Mine) -ge 0) { continue }
        if (-not $best -or (Compare-KitVersion $k.Version $best.Version) -gt 0) { $best = $k }
    }
    if (-not $best) {
        throw ("This kit keeps no older version (an update keeps the one it replaces, from 0.11 on). To install an older release, " +
               "run Update.bat -Version <tag>, such as: Update.bat -Version v0.10.1 (the tags: https://github.com/$Repo/releases).")
    }
    $to = $best.Version
    Write-Host "Going back to $to (kept in $($best.Dir))."
    foreach ($l in Get-KitSameVersionText $to) { Write-Host $l }
    Write-Host (Get-KitGameChangeText (Get-KitGameChange $Dir $best.Dir) $to)
    Write-Host "Your saves, keys, settings and coop-options.ini are kept, and so is $(if ($Mine) { $Mine } else { 'this version' }): Update.bat goes forward again."
    Wait-KitNotRunning $Dir
    if (-not $Yes) {
        $a = Read-Host "Go back to $to? [Y/n]"
        if ($a -and $a.Trim() -notmatch '^(y|yes)$') { Write-Host 'Nothing changed.'; return 0 }
    }
    $r = Invoke-KitSwitch $Dir $best.Dir $to $Mine $true $false
    Write-Host "Back on $($to): $($r.N) files. The replaced files are in $($r.Backup)."
    foreach ($l in Update-KitStartLinks $Dir) { Write-Host $l }
    if ($script:ShadersMoved) { Write-Host "The emulator changed, so its shader cache was moved to $($r.Backup)\$ShaderCache." }
    Write-Host "Starting the kit stays on $to now; it asks only when a release newer than $(Get-KitStayLeft $Dir $to) comes out. Update.bat goes forward."
    Write-Host 'Tell the others in your session to go back too: everyone needs the same version.'
    return 0
}

function Get-KitStayLeft([string]$Dir, [string]$Ver) {
    $s = Get-KitStay $Dir $Ver
    if ($s) { return $s.Left }
    return $Ver
}

# --- The run ----------------------------------------------------------------------------

function Invoke-KitUpdate {
    $dir = $Kit
    if (-not $dir) { $dir = Split-Path -Parent $PSScriptRoot }
    $dir = [System.IO.Path]::GetFullPath($dir)
    if (-not (Test-KitFolder $dir)) {
        throw "$dir is not a co-op kit. Put Update.bat in the kit folder, beside ""Start Squadala.bat"" (or Host.bat in an older kit)."
    }
    $mine = Get-KitVersion $dir
    # (a damaged VERSION.txt: this kit counts as no release, the update goes on, nothing is named after it)
    if ($mine -and -not (Test-KitSafeVersion $mine)) { $mine = '' }
    $unfinished = Test-KitUnfinished $dir
    $have = if ($mine) { $mine } else { 'a test build' }
    $list = if ($Api) { $Api } else { "https://api.github.com/repos/$Repo/releases?per_page=30" }
    # Keys an older updater did not add (not while an update is half done).
    if (-not $unfinished) { Update-KitOptions $dir | Out-Null }
    # Shortcuts to a start file that is gone (the old "Start OoT Co-op.bat" after an update by an older updater) follow
    # the kit's start file (said with -Tidy; -Status prints only its one line).
    # -Status does it after its line is printed (below): the line must not wait for the shortcut scan.
    if (-not $unfinished -and -not $Status) {
        $links = @(Update-KitStartLinks $dir)
        if ($Tidy) { foreach ($l in $links) { Write-Host $l } }
    }
    if ($Tidy) { return 0 }
    # The player went back to this version on purpose: only a release newer than the one they left counts.
    $stay = Get-KitStay $dir $mine
    $wanted = { param($tag) (Compare-KitVersion $tag $mine) -gt 0 -and (-not $stay -or (Compare-KitVersion $tag $stay.Left) -gt 0) }

    if ($Status) {
        # Straight to stdout: a function's own output would become its return value.
        # (flushed before the shortcut scan: play.ps1 reads this line even when it stops the scan at its time limit)
        if ($unfinished) { [Console]::Out.WriteLine('unfinished'); [Console]::Out.Flush(); return 0 }
        $line = ''
        try { $newest = Select-KitRelease (Read-KitReleaseList $list) } catch {
            $line = 'failed ' + (Get-KitErrorText $_)
        }
        if (-not $line -and $newest -and (& $wanted $newest.Tag)) { $line = "newer $($newest.Tag)" }
        # (nothing to say is a line too, 'current': a check cut off after this point is an answer, not a silence)
        [Console]::Out.WriteLine($(if ($line) { $line } else { 'current' }))
        [Console]::Out.Flush()
        [void](Update-KitStartLinks $dir)
        return 0
    }
    if ($Check) {
        if ($unfinished) {
            Write-Host 'An update of this kit did not finish: close the games and run Update.bat.'
            return 0
        }
        try { $newest = Select-KitRelease (Read-KitReleaseList $list) } catch {
            Write-Host "Could not check for a new version: $(Get-KitErrorText $_)"
            return 0
        }
        if ($newest -and (& $wanted $newest.Tag)) {
            Write-Host "A new version is out: $($newest.Tag) (you have $have). Close the games and run Update.bat."
        } elseif ($stay -and $newest -and (Compare-KitVersion $newest.Tag $mine) -gt 0) {
            Write-Host "You stay on $mine (you went back from $($stay.Left)); $($newest.Tag) is out. Update.bat goes forward again."
        }
        return 0
    }
    if ($Revert) { return (Invoke-KitRevert $dir $mine $unfinished) }

    Write-Host "Kit: $dir"
    Write-Host "This kit: $have"
    if ($unfinished) { Write-Host 'An update of this kit did not finish: it is done again now.' }
    if ($Version) {
        Write-Host "Looking for release $Version..."
        $newest = Find-KitRelease $list $Version
        if (-not $newest) { throw "No release $Version with a Windows kit was found (the releases: https://github.com/$Repo/releases)." }
    } else {
        Write-Host 'Looking for the newest release...'
        $newest = Select-KitRelease (Read-KitReleaseList $list)
        if (-not $newest) { throw 'No release with a kit was found in the release list.' }
    }
    $script:KitPage = $newest.Page
    if (-not $Api) {
        foreach ($u in $newest.Zip, $newest.Sum) {
            if (-not (Test-KitDownloadUrl $u $Repo)) { throw "The release list names a download outside this project's releases: $u" }
        }
    }
    $cmp = Compare-KitVersion $newest.Tag $mine
    if (-not $unfinished -and $cmp -eq 0 -and $Version) { Write-Host "This kit is $mine already."; return 0 }
    if (-not $unfinished -and $cmp -le 0 -and -not $Version) {
        Write-Host "You have the newest version ($mine)."
        return 0
    }
    # Going back: an older release by its tag (Update.bat -Version).
    $back = $cmp -lt 0 -and [bool]$Version
    if ($Launch -and $stay -and -not $Version -and (Get-KitAutoUpdate $dir) -eq 'yes') {
        # (auto_update = ask: the start asked already)
        if (-not (Read-KitYesNo "You went back to $mine on purpose. Co-op $($newest.Tag) is out, newer than $($stay.Left). Update now?" 20 $false)) {
            Write-Host "Staying on $mine. Update.bat updates when you want."
            return 0
        }
    }
    Write-Host ("{0}: {1}, {2:N0} MB.  {3}" -f $(if ($Version) { 'Release' } else { 'Newest' }), $newest.Tag, ($newest.Size / 1MB), $newest.Page)
    if ($back) {
        foreach ($l in Get-KitSameVersionText $newest.Tag) { Write-Host $l }
        Write-Host "This version ($have) is kept in $Previous\: Update.bat goes forward again."
    } elseif ($stay) {
        Write-Host "You went back to $mine on purpose; this goes forward to $($newest.Tag)."
    }

    while ($true) {
        $running = Get-KitRunning $dir
        if (-not $running) { break }
        # Started from Host.bat or Join.bat: this kit's server (a window left open since the last
        # game) is stopped, as it is replaced now and is the old version; Host.bat starts the
        # new one. The emulator is never stopped (the game may not be saved).
        $servers = @($running | Where-Object { $_.Name -ieq 'coop_server' })
        if ($Launch -and $servers.Count) {
            Write-Host "Stopping this kit's co-op server (the old version); hosting again starts the new one."
            $servers | Stop-Process -Force -ErrorAction SilentlyContinue
            $servers | ForEach-Object { try { $_.WaitForExit(5000) | Out-Null } catch { } }
            continue
        }
        Write-Host "Close these first (they run from this kit): $(($running | ForEach-Object { $_.Name + '.exe' } | Sort-Object -Unique) -join ', ')"
        if ($Yes) { throw "This kit's emulator or server is still running: close it, then start the kit (""Start Squadala.bat"", Host.bat or Join.bat) or Update.bat again." }
        Read-Host 'Press Enter when they are closed' | Out-Null
    }
    if (-not $Yes) {
        $verb = if ($back) { 'Go back' } else { 'Update' }
        $a = Read-Host "$verb to $($newest.Tag)? Your saves, keys, settings and coop-options.ini are kept. [Y/n]"
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
        if ((Test-KitSignature $dir $zip) -eq 'good') { Write-Host 'Signature checked: signed by this project''s release workflow.' }

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
        if (-not (Test-KitFolder $newKit)) { throw 'The release zip holds no kit. Nothing changed.' }
        $why = Test-KitNewKit $newKit
        if ($why) { throw "The release zip is not a co-op kit ($why). Nothing changed." }

        $r = Invoke-KitSwitch $dir $newKit $newest.Tag $mine $back $unfinished
        $n = $r.N; $backup = $r.Backup
        # After the install: an emulator kit updated to a Hookshot-only one has the new launcher now.
        $hsOnly = [bool](Get-KitStartFile $dir)
        Write-Host "$(if ($back) { 'Went back' } else { 'Updated' }) to $($newest.Tag): $n files. The replaced files are in $backup (delete it once the new version works)."
        if ($script:ShadersMoved) {
            Write-Host "The emulator changed, so its old shader cache was moved to $backup\$ShaderCache. The first minutes of play may stutter while a new one is built."
        } elseif ($unfinished -and -not (Test-Path -LiteralPath (Join-Path $dir $ShaderCache))) {
            # The stopped run moved it already, into its own backup folder.
            foreach ($s in Get-KitBackedUpShaders $dir) {
                Write-Host "The old shader cache, moved by the update that stopped, is in $s."
            }
        }
        foreach ($l in Complete-KitHookshotOnly $dir $r.HadEmu) { Write-Host $l }
        if ($hsOnly -and $r.Game -ne 'same') { Write-Host (Get-KitGameChangeText $r.Game $newest.Tag) }
        if ($script:PackToolChanged) {
            if ($hsOnly) { Write-Host 'The puppet pack tool changed: "Start Squadala.bat" makes the pack again when it starts.' }
            else { Write-Host 'The puppet pack tool changed: run make-puppet-pack.cmd once more before you play, for the other players'' sounds and the latest puppet fixes.' }
        }
        if ($back) {
            Write-Host "Starting the kit stays on $($newest.Tag) now; it asks only when a release newer than $(Get-KitStayLeft $dir $newest.Tag) comes out. Update.bat goes forward."
            Write-Host 'Tell the others in your session to go back too: everyone needs the same version.'
        } else {
            if ($r.Kept) {
                $how = if (Test-Path -LiteralPath (Join-Path $dir 'Revert.bat')) { 'Revert.bat' } else { 'Update.bat -Revert' }
                Write-Host "Changed your mind? $how goes back to $mine (kept in $Previous\$mine)."
            }
            Write-Host 'Every player needs the same version: tell the others to update too.'
        }
        if ($Launch) { return 3 }
        return 0
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# QuickEdit off in this window: a click in it would pause the update (issue #68). The same function as
# console-mode.ps1 (this file runs alone from a temp file, so it carries its own copy; update_test.ps1 checks they match).
function Disable-KitQuickEdit {
    try {
        $name = New-Object System.Reflection.AssemblyName 'OotCoopConsoleMode'
        $asm = [System.Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, 'Run')
        $type = $asm.DefineDynamicModule('OotCoopConsoleMode').DefineType('OotCoopConsoleMode', 'Public, Class')
        foreach ($m in @(@('CreateFileW', [IntPtr], @([string], [uint32], [uint32], [IntPtr], [uint32], [uint32], [IntPtr])),
                @('GetConsoleMode', [bool], @([IntPtr], [uint32].MakeByRefType())),
                @('SetConsoleMode', [bool], @([IntPtr], [uint32])),
                @('CloseHandle', [bool], @([IntPtr])))) {
            $type.DefinePInvokeMethod($m[0], 'kernel32.dll', 'Public, Static, PinvokeImpl', 'Standard', $m[1],
                [Type[]]$m[2], 'Winapi', 'Unicode').SetImplementationFlags('PreserveSig')   # (else the result is lost)
        }
        $native = $type.CreateType()
        # GENERIC_READ | GENERIC_WRITE, shared for reading and writing, OPEN_EXISTING
        $in = $native::CreateFileW('CONIN$', [uint32]3221225472, [uint32]3, [IntPtr]::Zero, [uint32]3, [uint32]0, [IntPtr]::Zero)
        if ($in -eq [IntPtr]::Zero -or $in -eq [IntPtr]-1) { return $false }
        try {
            $mode = [uint32]0
            if (-not $native::GetConsoleMode($in, [ref]$mode)) { return $false }
            # ENABLE_QUICK_EDIT_MODE (0x40) off; ENABLE_EXTENDED_FLAGS (0x80) on, without it the change is ignored.
            $new = [uint32](($mode -band 0xFFFFFFBF) -bor 0x80)
            return [bool]$native::SetConsoleMode($in, $new)
        } finally { [void]$native::CloseHandle($in) }
    } catch { return $false }
}

# Update.bat's last step, after an update and also when the kit is the newest already: the game's co-op parts this kit
# pins (the module, the puppet pack, the randomizer OOTRR in the game's tools\ootrr, seeds to build again) are put in now,
# by the kit's OWN scripts\hookshot-build.ps1 -Repair (the version just installed, not this updater's), while the game is
# closed. Before, only the next "Start Squadala.bat" did it, and a failure there was one line in a window that closes:
# Velvet's kit (2026-11-19) played on with no OOTRR, the randomizer broken, after an Update.bat that "worked".
# A kit with no Longshot game or no hookshot-build.ps1 has nothing to repair. Returns the repair's exit code (0: fine or
# nothing to do); never throws.
function Invoke-KitGameRepair([string]$KitDir) {
    try {
        $build = Join-Path $KitDir 'scripts\hookshot-build.ps1'
        if (-not (Test-Path -LiteralPath (Join-Path $KitDir 'hookshot\PIN')) -or -not (Test-Path -LiteralPath $build)) { return 0 }
        if (-not (Select-String -LiteralPath $build -SimpleMatch '[switch]$Repair' -Quiet)) { return 0 }   # (an older kit's script)
        if (@(Get-KitRunning $KitDir).Count) {
            Write-Host 'The game is running from this kit: close it, then run Update.bat again so it can check the game''s co-op parts and the randomizer.'
            return 0
        }
        Write-Host ''
        Write-Host 'Checking the game''s co-op parts and the randomizer...'
        $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        # (Start-Process in this window: its questions (the pack's game file) reach the player, and its lines never
        # become this function's return value as `& powershell` output would)
        $p = Start-Process -FilePath $ps -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', """$build""", '-Repair') -NoNewWindow -Wait -PassThru
        $code = $p.ExitCode
        if ($code -ne 0) { Write-Host 'The game could not be repaired now (above: why). "Start Squadala.bat" tries again at its next start.' }
        return $code
    } catch {
        Write-Host "The game's co-op parts were not checked: $($_.Exception.Message)"
        return 1
    }
}

# The window's lines into the start log (player report 266; kit-paths.ps1 Start-KitStartLog, which this copy cannot
# dot-source: it runs from a copy in TEMP while the kit's own files are replaced): appended to the file of the start
# that runs this (OOT_COOP_START_LOG, play.ps1 stopped its own meanwhile), or, from Update.bat, a new
# <kit>\hookshot\logs\start-log.txt with the one before kept as start-log.1.txt. Not for -Check, -Status or -Tidy (a
# start's quiet look while its own log is open). Never stops the update.
function Start-KitUpdateLog([string]$KitDir) {
    try {
        $log = $env:OOT_COOP_START_LOG
        if (-not $log) {
            if (-not $KitDir -or -not (Test-Path -LiteralPath (Join-Path $KitDir 'hookshot') -PathType Container)) { return $false }
            $logDir = Join-Path $KitDir 'hookshot\logs'
            $log = Join-Path $logDir 'start-log.txt'
            New-Item -ItemType Directory -Force -Path $logDir | Out-Null
            if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination (Join-Path $logDir 'start-log.1.txt') -Force -ErrorAction Stop }
        }
        Start-Transcript -LiteralPath $log -Append -Force -ErrorAction Stop | Out-Null
        return $true
    } catch {
        return $false
    }
}

if (-not $NoRun) {
    if (-not ($Check -or $Status -or $Tidy)) {
        [void](Disable-KitQuickEdit)
        [void](Start-KitUpdateLog $(if ($Kit) { $Kit } else { Split-Path -Parent $PSScriptRoot }))
    }
    try {
        $code = Invoke-KitUpdate
        # (not for play.ps1's -Launch, whose new play.ps1 does the same step next, nor going back: the older kit's start does)
        if ($code -eq 0 -and -not ($Check -or $Status -or $Tidy -or $Launch -or $Revert)) {
            $kitDir = if ($Kit) { $Kit } else { Split-Path -Parent $PSScriptRoot }
            if (Test-Path -LiteralPath $kitDir) { [void](Invoke-KitGameRepair ([System.IO.Path]::GetFullPath($kitDir))) }
        }
        exit $code
    } catch {
        if ($Check -or $Status -or $Tidy) { exit 0 }
        Write-Host "$(if ($Revert) { 'Going back failed' } else { 'Update failed' }): $(Get-KitErrorText $_)"
        if (-not $Revert) { foreach ($l in Get-KitManualSteps $script:KitPage) { Write-Host $l } }
        exit 1
    }
}
