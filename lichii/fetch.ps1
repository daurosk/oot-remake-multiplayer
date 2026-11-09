# Clones or updates the emulator source to the commit in lichii/PIN and applies
# lichii/patches/*.patch in order, as commits on a local branch "coop".
# Refuses if the source tree has uncommitted changes. Never pushes anywhere.
#
#   powershell -ExecutionPolicy Bypass -File lichii\fetch.ps1 [-Dest <folder>]
#
# The default folder is fritchii-src next to the main checkout of this repo.

param(
    [string]$Dest = ""
)

$ErrorActionPreference = "Stop"

function Fail([string]$message) {
    Write-Host "fetch.ps1: $message"
    exit 1
}

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE -ne 0) { Fail "git $($args -join ' ') failed" }
}

# The pin.
$pin = @{}
foreach ($line in Get-Content (Join-Path $PSScriptRoot "PIN")) {
    if ($line -match '^\s*([a-z]+)=(.+?)\s*$') { $pin[$Matches[1]] = $Matches[2] }
}
foreach ($key in "repo", "tag", "commit") {
    if (-not $pin.ContainsKey($key)) { Fail "lichii/PIN has no $key=" }
}

# Default destination: beside the main checkout (works from a worktree too).
if ($Dest -eq "") {
    $common = (& git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir)
    if ($LASTEXITCODE -ne 0) { Fail "not inside a git checkout" }
    $mainRoot = Split-Path -Parent $common
    $Dest = Join-Path (Split-Path -Parent $mainRoot) "fritchii-src"
}
Write-Host "fetch.ps1: emulator source in $Dest"

if (-not (Test-Path $Dest)) {
    Invoke-Git clone $pin.repo $Dest
} else {
    if (-not (Test-Path (Join-Path $Dest ".git"))) { Fail "$Dest exists and is not a git checkout" }
    $dirty = (& git -C $Dest status --porcelain)
    if ($LASTEXITCODE -ne 0) { Fail "git status failed in $Dest" }
    if ($dirty) {
        Write-Host ($dirty | Select-Object -First 10 | Out-String)
        Fail "$Dest has uncommitted changes; commit, stash or remove them first"
    }
    if (Test-Path (Join-Path $Dest ".git\rebase-apply")) { Fail "a git am is in progress in $Dest" }
}

# Get the pinned commit if it is not here yet.
& git -C $Dest cat-file -e "$($pin.commit)^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) {
    Invoke-Git -C $Dest fetch --tags $pin.repo
    & git -C $Dest cat-file -e "$($pin.commit)^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) { Fail "commit $($pin.commit) not found in $($pin.repo)" }
}
$tagCommit = (& git -C $Dest rev-parse --verify --quiet "refs/tags/$($pin.tag)^{commit}")
if ($tagCommit -and $tagCommit -ne $pin.commit) {
    Write-Host "fetch.ps1: warning: tag $($pin.tag) is $tagCommit here, the pin says $($pin.commit)"
}

# Fresh branch at the pin, then our patches on top. The local commits get a
# fixed identity so whatever git identity is set globally is never used.
Invoke-Git -C $Dest checkout --quiet -B coop $pin.commit
$patches = @(Get-ChildItem (Join-Path $PSScriptRoot "patches") -Filter *.patch | Sort-Object Name)
foreach ($patch in $patches) {
    Write-Host "fetch.ps1: applying $($patch.Name)"
    & git -C $Dest -c user.name=coop-fetch -c user.email=coop-fetch@localhost am --quiet $patch.FullName
    if ($LASTEXITCODE -ne 0) {
        & git -C $Dest am --abort
        Fail "$($patch.Name) does not apply to $($pin.commit)"
    }
}
Write-Host "fetch.ps1: $Dest is at $($pin.tag) ($($pin.commit)) with $($patches.Count) patch(es) on branch coop"
