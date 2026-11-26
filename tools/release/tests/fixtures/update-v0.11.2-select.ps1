# The release choice and new-kit check of the v0.11.2 kit's updater, as released (git show
# v0.11.2:tools/release/kit/scripts/update.ps1, these functions unchanged): update_test.ps1 checks that the
# v0.11.0 to v0.11.2 kits out there still find and accept the releases from v0.11.3 on (Squadala- names).

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

# The Windows kit zip of a release and its .sha256: oot-coop-<tag>-windows.zip from v0.11.0 on, else
# oot-coop-<tag>.zip (every release before, and the same file again in later ones, for the kits up to v0.10.1,
# whose updater knows only that name). @(zip asset, sha256 asset) or $null.
function Get-KitReleaseZip($Release, [string]$Tag) {
    foreach ($n in "oot-coop-$Tag-windows.zip", "oot-coop-$Tag.zip") {
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

# A co-op kit folder: an emulator kit (emulator\) or a Hookshot-only kit (make_kit_hookshot.ps1: hookshot\PIN and its launcher).
function Test-KitFolder([string]$Dir) {
    if (Test-Path -LiteralPath (Join-Path $Dir 'emulator')) { return $true }
    return (Test-Path -LiteralPath (Join-Path $Dir 'hookshot\PIN')) -and (Test-Path -LiteralPath (Join-Path $Dir 'Start OoT Co-op.bat'))
}

# Why an unpacked release must not be installed, or ''. An emulator kit: its emulator (Test-KitEmulator).
# A Hookshot-only kit: the co-op module, the Hookshot pin, the launcher and its script.
function Test-KitNewKit([string]$Dir) {
    if (Test-Path -LiteralPath (Join-Path $Dir 'emulator')) { return (Test-KitEmulator (Join-Path $Dir 'emulator\lichii.exe')) }
    foreach ($f in 'hookshot\hookshot_coop.dll', 'hookshot\PIN', 'Start OoT Co-op.bat', 'scripts\start.ps1') {
        if (-not (Test-Path -LiteralPath (Join-Path $Dir $f))) { return "it has no $f" }
    }
    return ''
}
