# Player report 101 proof: a third-party mod whose name sorts under contents/ ("bestbuddy", in the report an OOTRR
# website seed) with RSDB files larger than the game's size entries and its own size table. Longshot serves the puppet
# pack's table then; before the fix (tools/puppet_pack/merge_tables.py) the game aborts ~1 s into the start.
# Needs the WHOLE PC (a kit-mode run in lane 1; TEST-TURN: the same id on LANE1 and LANE2) and kits set up by
# kit_pair.py setup (ls\kit-A / -B naming lane1-A / -B).
#
#   proof-size-mod.ps1 -Step before  -KitA <kit-A> -KitB <kit-B>   the kit as released: A must abort (FAIL expected)
#   proof-size-mod.ps1 -Step after   -KitA <kit-A> -KitB <kit-B>   this branch's merge_tables.py in both kits: A starts,
#                                                                  merges bestbuddy into the pack's table, puppets show
#   proof-size-mod.ps1 -Step cleanup -KitA <kit-A> -KitB <kit-B>   fake mod out, pack table back, kits' own tool back
#
# Each step prints PASS, FAIL or UNPROVEN with what it went by. Only A gets the mod; B is the control.
# before: the abort is the game's own 'FATAL: the game aborted' log line (a crash: nothing to see but a closed window),
#         log only. after: "puppets show" is visible, so besides its log lines it needs the run's screenshot checks
#         (-See, default hs-puppets-A/-B of tools\autotest\proofs.txt, graded by proof.py on the run folder): every one
#         OK = PASS, a shot showing the wrong result = FAIL, else UNPROVEN (owner rule 2026-11-18: no PASS from log
#         lines alone). Exit 0 PASS, 1 FAIL, 2 UNPROVEN.
param(
    [Parameter(Mandatory)][ValidateSet('before', 'after', 'cleanup')][string]$Step,
    [Parameter(Mandatory)][string]$KitA,
    [Parameter(Mandatory)][string]$KitB,
    [string]$Scenario = 'hs-puppets',
    [int]$Timeout = 300,
    [string]$Server = '',  # default: the 0.11.2 integrator's server, a path that ran before (no firewall prompt)
    [string[]]$See = @('hs-puppets-A', 'hs-puppets-B')   # the after step's screenshot checks (proofs.txt names)
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Server) { $Server = [System.IO.Path]::GetFullPath((Join-Path $here '..\..\..\integrate-0112\build\server\coop_server.exe')) }
$repo = [System.IO.Path]::GetFullPath((Join-Path $here '..\..'))
$py = Join-Path $repo '..\oot-remake-recomp\.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py)) { $py = Join-Path $KitA 'hookshot\tools\python\python.exe' }

function Get-GameDir([string]$Kit) {
    $note = Join-Path $Kit 'hookshot\game-folder.txt'
    $g = if (Test-Path -LiteralPath $note) { "$(Get-Content -LiteralPath $note -TotalCount 1)".Trim() } else { 'hookshot\game' }
    if (-not [System.IO.Path]::IsPathRooted($g)) { $g = Join-Path $Kit $g }
    return [System.IO.Path]::GetFullPath($g)
}
function Get-NewestLog([string]$Game) {
    @((Join-Path $Game 'logs\longshot_log.txt'), (Join-Path $Game 'longshot_log.txt')) | Where-Object { Test-Path -LiteralPath $_ } |
        Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } | Select-Object -Last 1
}
function Save-CrashRecords([string]$Game, [string]$To) {
    # (a pending crash window would hold the next unattended start: final smoke run 1)
    $pending = Join-Path $Game 'bug-reports\pending'
    if (-not (Test-Path -LiteralPath $pending)) { return }
    $items = @(Get-ChildItem -LiteralPath $pending -Force)
    if (-not $items) { return }
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    $items | Move-Item -Destination $To -Force
    Write-Host "  crash records moved out of $pending -> $To"
}

$gameA = Get-GameDir $KitA
$gameB = Get-GameDir $KitB
$modsA = Join-Path $gameA 'mods'
$fake = Join-Path $modsA 'bestbuddy'
$romfs = Join-Path $gameA 'romfs'
$ours = Join-Path $repo 'tools\puppet_pack\merge_tables.py'
# coop-kit\ is the kits' grandparent (coop-kit\ls\kit-A); run.ps1 from the main checkout beside it, which finds the lanes
# there (from a .worktrees checkout it would look in .worktrees\coop-kit)
$coopKit = Split-Path -Parent (Split-Path -Parent ([System.IO.Path]::GetFullPath($KitA)))
$runPs1 = [System.IO.Path]::GetFullPath((Join-Path $coopKit '..\oot-remake-multiplayer\tools\autotest\run.ps1'))
$keep = Join-Path $coopKit 'proof-size-mod'   # (outside the repo: game data and the kits' own tool)
New-Item -ItemType Directory -Force -Path $keep | Out-Null
Write-Host "game A: $gameA"
Write-Host "game B: $gameB"

if ($Step -eq 'cleanup') {
    & $py -I (Join-Path $repo 'tools\puppet_pack\merge_tables.py') --mods $modsA --romfs $romfs --undo
    & $py -I (Join-Path $here 'fake_size_mod.py') --remove --out $fake
    foreach ($k in $KitA, $KitB) {
        $saved = Join-Path $keep "$(Split-Path -Leaf $k)-merge_tables.py"
        if (Test-Path -LiteralPath $saved) {
            Copy-Item -LiteralPath $saved -Destination (Join-Path $k 'tools\puppet_pack\merge_tables.py') -Force
            Remove-Item -LiteralPath $saved -Force
            Write-Host "  $k\tools\puppet_pack\merge_tables.py: the kit's own again"
        }
    }
    foreach ($g in $gameA, $gameB) { Save-CrashRecords $g (Join-Path $keep 'crash-records') }
    Write-Host 'cleanup done'
    exit 0
}

# The fake mod (made fresh each step from A's own game files).
& $py -I (Join-Path $here 'fake_size_mod.py') --romfs $romfs --out $fake
if ($LASTEXITCODE -ne 0) { throw 'the fake mod could not be made' }
if ($Step -eq 'after') {
    foreach ($k in $KitA, $KitB) {
        $tool = Join-Path $k 'tools\puppet_pack\merge_tables.py'
        $saved = Join-Path $keep "$(Split-Path -Leaf $k)-merge_tables.py"
        if (-not (Test-Path -LiteralPath $saved)) { Copy-Item -LiteralPath $tool -Destination $saved }
        Copy-Item -LiteralPath $ours -Destination $tool -Force
        Write-Host "  $tool <- this branch's"
    }
}
foreach ($g in $gameA, $gameB) { Save-CrashRecords $g (Join-Path $keep 'crash-records') }
$started = Get-Date
$env:HOS_CRASH_WATCH = '0'   # (no modal crash window: the abort is read from the log)
$runLines = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $runPs1 -Runtime hookshot -Lane 1 -Kit "$KitA,$KitB" -Scenario $Scenario -Timeout $Timeout -Server $Server |
    ForEach-Object { Write-Host $_; "$_" })
$runCode = $LASTEXITCODE
# run.ps1 prints its run folder (the shots) as its last line that is a folder
$runDir = @($runLines | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) }) | Select-Object -Last 1
Remove-Item Env:\HOS_CRASH_WATCH -ErrorAction SilentlyContinue

$log = Get-NewestLog $gameA
$text = if ($log -and (Get-Item -LiteralPath $log).LastWriteTime -ge $started) { [System.IO.File]::ReadAllText($log) } else { '' }
$rsdb = [regex]::Matches($text, 'mods: RSDB/\S+ <- .*bestbuddy').Count
$fatal = $text -match 'FATAL: the game aborted'
$table = [regex]::Match($text, 'mods: System/Resource/ResourceSizeTable\S+ <- (.*)').Groups[1].Value
$puppet = [regex]::Matches($text, 'CoopPuppet_\S+ spawned').Count
$startText = @(Get-ChildItem -Path (Join-Path $coopKit "ls\shots-lane1\$Scenario") -Recurse -Filter 'kit-start-A.txt' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $started } | ForEach-Object { [System.IO.File]::ReadAllText($_.FullName) }) -join "`n"
$merged = $startText -match "Size tables: the game reads the puppet pack's; merged in the entries of bestbuddy"
Write-Host ''
Write-Host "run.ps1 exit $runCode; A's log: $log"
Write-Host "  bestbuddy RSDB files served: $rsdb; size table served from: $table"
Write-Host "  abort: $fatal; puppets spawned: $puppet; kit start merged bestbuddy into the pack's table: $merged"
$ok = if ($Step -eq 'before') { $rsdb -gt 0 -and $fatal } else { $rsdb -gt 0 -and -not $fatal -and $merged -and $puppet -gt 0 }
$what = if ($Step -eq 'before') { 'the released kit aborts with the mod (the bug reproduced) [log only: the abort is the game''s own FATAL line]' } else { 'the fixed kit merges the mod in and the game starts with puppets' }
$state = if ($ok) { 'PASS' } else { 'FAIL' }
if ($ok -and $Step -eq 'after') {
    # the puppets: the shots of the run, not the 'spawned' lines
    if (-not $runDir) {
        $state = 'UNPROVEN'; $what += ' [no run folder from run.ps1: no shots to check]'
    } else {
        $proofOut = @(& $py -I (Join-Path $here 'proof.py') --kit-root $coopKit grade $runDir @See | ForEach-Object { "$_" })
        $proofCode = $LASTEXITCODE
        $proofOut | ForEach-Object { Write-Host "  $_" }
        $state = switch ($proofCode) { 0 { 'PASS' } 3 { 'FAIL' } default { 'UNPROVEN' } }
        $what += " [screenshot checks $($See -join ' ') in ${runDir}: $state]"
    }
}
Write-Host "$state $Step`: $what"
foreach ($g in $gameA, $gameB) { Save-CrashRecords $g (Join-Path $keep 'crash-records') }
exit $(switch ($state) { 'PASS' { 0 } 'FAIL' { 1 } default { 2 } })
