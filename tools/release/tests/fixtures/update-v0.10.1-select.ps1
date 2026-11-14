# The release choice of the v0.10.1 kit's updater, as released (git show v0.10.1:tools/release/kit/scripts/update.ps1,
# lines 69-206, unchanged): update_test.ps1 checks that the kits out there still find the newer releases.
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
        "  1. Close the games and the server window, and download the newest oot-coop-....zip from $where",
        '  2. Unzip it to a new folder with a short path, such as C:\oot-coop-new.',
        '  3. Copy the folder emulator\user (keys, firmware, saves, settings, your name, the puppet pack)',
        '     and the file coop-options.ini from this kit into the new folder. Missing switches are added',
        '     to coop-options.ini when you start the new kit.',
        '  4. Play from the new folder ("Start OoT Co-op.bat", or Host.bat / Join.bat in an emulator kit).')
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

