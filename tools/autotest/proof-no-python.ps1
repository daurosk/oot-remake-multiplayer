# Player report 235 proof: a kit WITHOUT its own Python (hookshot\tools\python gone) and with a third-party mod whose
# size table wins (the fake mod of proof-size-mod.ps1, "CS on") must still reach the title and start: the kit start
# parks the pack's title/pause menu files (Move-KitMenuFilesAside) instead of letting the larger files overrun the
# smaller size table. Needs the WHOLE PC (a kit-mode run in lane 1). Kits as kit_pair.py setup made them.
#
#   proof-no-python.ps1 -KitA <kit-A> -KitB <kit-B>        runs, then puts the kit's Python back (always, also on error)
#
# PASS = no "FATAL: the game aborted" in A's log, the puppets show (the run's screenshot checks hs-puppets-A/-B OK),
# and the kit start text says the menu files were parked. Exit 0 PASS, 1 FAIL, 2 UNPROVEN (owner rule: no PASS from
# log lines alone). The screenshot of the title: the run folder's shots (read them).
param(
    [Parameter(Mandatory)][string]$KitA,
    [Parameter(Mandatory)][string]$KitB,
    [string]$Scenario = 'hs-puppets',
    [int]$Timeout = 300,
    [string]$Server = '',
    [string[]]$See = @('hs-puppets-A', 'hs-puppets-B')
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Server) { $Server = [System.IO.Path]::GetFullPath((Join-Path $here '..\..\..\integrate-0112\build\server\coop_server.exe')) }
$repo = [System.IO.Path]::GetFullPath((Join-Path $here '..\..'))
$py = Join-Path $repo '..\oot-remake-recomp\.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py)) { $py = Join-Path $repo '..\tools-venv\Scripts\python.exe' }
if (-not (Test-Path -LiteralPath $py)) { $py = Join-Path $KitA 'hookshot\tools\python\python.exe' }   # (before it is moved)
$coopKit = Split-Path -Parent (Split-Path -Parent ([System.IO.Path]::GetFullPath($KitA)))
$runPs1 = Join-Path $here 'run.ps1'   # this checkout's own (its scenarios; run.ps1 finds coop-kit by walking up)
function Get-GameDir([string]$Kit) {
    $note = Join-Path $Kit 'hookshot\game-folder.txt'
    $g = if (Test-Path -LiteralPath $note) { "$(Get-Content -LiteralPath $note -TotalCount 1)".Trim() } else { 'hookshot\game' }
    if (-not [System.IO.Path]::IsPathRooted($g)) { $g = Join-Path $Kit $g }
    return [System.IO.Path]::GetFullPath($g)
}
$gameA = Get-GameDir $KitA
$fake = Join-Path $gameA 'mods\bestbuddy'
& $py -I (Join-Path $here 'fake_size_mod.py') --romfs (Join-Path $gameA 'romfs') --out $fake
if ($LASTEXITCODE -ne 0) { throw 'the fake mod could not be made' }
$moved = @()
try {
    foreach ($k in $KitA, $KitB) {
        $p = Join-Path $k 'hookshot\tools\python'
        if (Test-Path -LiteralPath $p) { Rename-Item -LiteralPath $p -NewName 'python.off'; $moved += $p; Write-Host "  $p moved aside" }
    }
    $started = Get-Date
    $env:HOS_CRASH_WATCH = '0'
    $runLines = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $runPs1 -Runtime hookshot -Lane 1 -Kit "$KitA,$KitB" -Scenario $Scenario -Timeout $Timeout -Server $Server |
        ForEach-Object { Write-Host $_; "$_" })
    Remove-Item Env:\HOS_CRASH_WATCH -ErrorAction SilentlyContinue
} finally {
    foreach ($p in $moved) { if (Test-Path -LiteralPath "$p.off") { Rename-Item -LiteralPath "$p.off" -NewName 'python' } }
    & $py -I (Join-Path $here 'fake_size_mod.py') --remove --out $fake
    Write-Host "  the kits' Python put back; fake mod removed"
}
$runDir = @($runLines | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) }) | Select-Object -Last 1
$log = @((Join-Path $gameA 'logs\longshot_log.txt'), (Join-Path $gameA 'longshot_log.txt')) | Where-Object { Test-Path -LiteralPath $_ } |
    Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } | Select-Object -Last 1
$text = if ($log -and (Get-Item -LiteralPath $log).LastWriteTime -ge $started) { [System.IO.File]::ReadAllText($log) } else { '' }
$fatal = $text -match 'FATAL: the game aborted'
$startText = @(Get-ChildItem -Path (Join-Path $coopKit "ls\shots-lane1\$Scenario") -Recurse -Filter 'kit-start-A.txt' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $started } | ForEach-Object { [System.IO.File]::ReadAllText($_.FullName) }) -join "`n"
Write-Host "  abort: $fatal; kit start text:"
Write-Host ($startText -split "`n" | Select-String -Pattern 'menu|Python|python' | ForEach-Object { "    $_" })
$state = if ($fatal -or -not $text) { 'FAIL' } elseif (-not $runDir) { 'UNPROVEN' } else {
    $out = @(& $py -I (Join-Path $here 'proof.py') --kit-root $coopKit grade $runDir @See | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host "  $_" }
    switch ($code) { 0 { 'PASS' } 3 { 'FAIL' } default { 'UNPROVEN' } }
}
Write-Host "$state no-python: the kit without Python reaches the title with the size mod on [shots in $runDir]"
exit $(switch ($state) { 'PASS' { 0 } 'FAIL' { 1 } default { 2 } })
