# Player row 238 proof (lane 1, kits as kit_pair.py setup made them):
#   proof-join-hearts-238.ps1 -KitA <kit-A> -KitB <kit-B> [-Rejoin]
# hs-join-fresh-238: B's baseline save edited to one heart more than A's, B never joined: a fresh join (the reported
# case). -Rejoin first runs hs-join-hearts-238 too (B gains a heart while the server is down and rejoins; B is frozen
# over the server's restart so that A gets back in and hosts first: see its header). B's baseline is put back from a copy in a finally block. No PASS from here: the shots
# decide (read the run folders printed); exit 0 when the runs ended.
param(
    [Parameter(Mandatory)][string]$KitA,
    [Parameter(Mandatory)][string]$KitB,
    [int]$Timeout = 600,
    [string]$Server = '',
    [switch]$Rejoin
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Server) { $Server = [System.IO.Path]::GetFullPath((Join-Path $here '..\..\..\coop-kit\ls\kit-A\server\coop_server.exe')) }
$repo = [System.IO.Path]::GetFullPath((Join-Path $here '..\..'))
$py = Join-Path $repo '..\tools-venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py)) { $py = Join-Path $repo '..\oot-remake-recomp\.venv\Scripts\python.exe' }
$coopKit = Split-Path -Parent (Split-Path -Parent ([System.IO.Path]::GetFullPath($KitA)))
$runPs1 = Join-Path $here 'run.ps1'   # this checkout's own (its scenarios; run.ps1 finds coop-kit by walking up)
function Get-GameDir([string]$Kit) {
    $note = Join-Path $Kit 'hookshot\game-folder.txt'
    $g = if (Test-Path -LiteralPath $note) { "$(Get-Content -LiteralPath $note -TotalCount 1)".Trim() } else { 'hookshot\game' }
    if (-not [System.IO.Path]::IsPathRooted($g)) { $g = Join-Path $Kit $g }
    return [System.IO.Path]::GetFullPath($g)
}
function Invoke-Run([string]$Scenario) {
    Write-Host "== $Scenario"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $runPs1 -Runtime hookshot -Lane 1 -Scenario $Scenario -Timeout $Timeout -Server $Server
    Write-Host "== $Scenario exit $LASTEXITCODE"
}
$base = Join-Path $KitB 'autotest-save'   # lane1-B is the game folder itself
$keep = Join-Path $coopKit 'proof-join-hearts-238-save'
if ($Rejoin) { Invoke-Run 'hs-join-hearts-238' }
if (Test-Path -LiteralPath $keep) { Remove-Item -LiteralPath $keep -Recurse -Force }
Copy-Item -LiteralPath $base -Destination $keep -Recurse
try {
    # One heart more than the host's own baseline (run 20261123-010422: both baselines had 16, so a fixed 16 was no
    # difference at all). A's slot_00 is the save its `load` plays.
    $aSave = Join-Path (Join-Path $KitA 'autotest-save') 'slot_00\progress.sav'
    $aLine = "$(& $py -I (Join-Path $here 'save_flags.py') get $aSave int:0xfbe01da1)"
    if ($LASTEXITCODE -ne 0 -or $aLine -notmatch '= (\d+)\s*$') { throw "cannot read A's MaxLife from ${aSave}: $aLine" }
    $more = [int]$Matches[1] + 4
    if ($more -gt 80) { throw "A's MaxLife $($Matches[1]) leaves no room for one heart more" }
    Write-Host "A's baseline MaxLife $($Matches[1]); B's set to $more"
    foreach ($sav in Get-ChildItem -Path $base -Recurse -Filter progress.sav) {
        & $py -I (Join-Path $here 'save_flags.py') set $sav.FullName "int:0xfbe01da1=$more" "int:0x31ab5580=$more"
        if ($LASTEXITCODE -ne 0) { throw "save_flags set failed on $($sav.FullName)" }
    }
    Invoke-Run 'hs-join-fresh-238'
} finally {
    Remove-Item -LiteralPath $base -Recurse -Force
    Copy-Item -LiteralPath $keep -Destination $base -Recurse
    Write-Host "B's save baseline put back"
}
