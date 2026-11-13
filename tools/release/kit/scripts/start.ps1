# "Start OoT Co-op.bat": the kit's one entry point (Hookshot only). It checks what could go wrong first and
# says it in plain words (start-checks.ps1), then hands over to play.ps1 choose -Hookshot, which updates
# the kit when a new version is out, builds the Hookshot game from the player's own game the first time
# (asks once for the game file) or brings it up to date, and starts it. Host and Join are in the game.
#   -NoUpdate   skip the update check (as play.ps1)
param([switch]$NoUpdate)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'start-checks.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$kit = Split-Path -Parent $PSScriptRoot
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"

function Stop-Start([string[]]$Lines) {
    Write-Host ''
    foreach ($l in $Lines) { Write-Host $l -ForegroundColor Yellow }
    exit 1
}
function Read-Yes([string]$Question) {
    $line = Read-Host "$Question [Y/n]"
    return -not ($line -and $line.Trim() -match '^(?i)(n|no)$')
}

# 1. Still in the zip: nothing can be built or saved there.
if (Test-KitInZip $kit) {
    Stop-Start @('This kit is still inside its zip file.',
        'Right-click the zip, choose "Extract All...", extract it to a short folder such as C:\OoT-Coop,',
        'then open that folder and double-click "Start OoT Co-op.bat" there.')
}

# 2. One start at a time for this folder.
$lock = Enter-KitStartLock $kit
if (-not $lock) { Stop-Start @('OoT Co-op is already starting in another window (it may be building the game). Wait for that window.') }

# 3. Files an antivirus or a broken unzip took.
$missing = @(Get-KitMissingFiles $kit)
if ($missing) { Stop-Start (Get-KitMissingFilesText $missing) }
$runtime = @(Get-KitMissingRuntime $kit)
if ($runtime) {
    Stop-Start @("The Microsoft Visual C++ runtime is missing ($($runtime -join ', ')).",
        'Install it (free, from Microsoft): https://aka.ms/vs/17/release/vc_redist.x64.exe',
        'then start "Start OoT Co-op.bat" again.')
}
$hsGame = Get-KitHookshotGame $kit
$gone = Get-KitGameQuarantined $hsGame
if ($gone) {
    Stop-Start @("Your antivirus has most likely removed $gone from the co-op game ($hsGame).",
        'Windows Security: Virus & threat protection > Protection history: restore it, and add this kit''s folder under',
        '"Exclusions". Then start "Start OoT Co-op.bat" again (the game is not built again).')
}

# 4. A folder the game build cannot use (only before the first build: a game built here shows it works).
$problem = ''
if (-not (Get-KitHookshotExe $hsGame)) {
    $problem = Get-KitPathProblem $kit
    if (-not $problem -and -not (Test-KitWritable $kit)) { $problem = 'readonly' }
}
if ($problem) {
    $to = Get-KitMoveTarget
    Write-Host (Get-KitPathProblemText $problem) -ForegroundColor Yellow
    if (-not $to) { Stop-Start @('Move this kit folder to a short folder such as C:\OoT-Coop, then start it there.') }
    if (-not (Read-Yes "Copy the kit to $to and start it from there?")) {
        Stop-Start @("Move this kit folder to a short folder such as $to yourself, then start it there.")
    }
    & robocopy $kit $to /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD (Join-Path $kit 'logs') | Out-Null
    if ($LASTEXITCODE -ge 8 -or -not (Test-Path -LiteralPath (Join-Path $to 'Start OoT Co-op.bat'))) {
        Stop-Start @("Copying to $to failed. Move this kit folder there yourself, then start it there.")
    }
    Write-Host "Copied. From now on use $to (this folder can be deleted: $kit)."
    $lock.ReleaseMutex()
    Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -WorkingDirectory $to -ArgumentList @('/c', "`"$(Join-Path $to 'Start OoT Co-op.bat')`"")
    exit 0
}

# 5. Other kits on this PC: an old one next to this, or a second copy. Said once; its game file is reused.
$others = @()
try { $others = @(Register-KitFolder (Join-Path $env:LOCALAPPDATA 'OoT-Coop\kits.txt') $kit) } catch { }
foreach ($o in $others) {
    $running = @(Get-Process hookshot -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith(([System.IO.Path]::GetFullPath($o).TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase) })
    $what = Get-KitName $o
    if ($running) { Write-Host "Note: the co-op game of another kit is running ($o)." }
    elseif ($what -and $what -ne (Get-KitName $kit)) { Write-Host "Note: another co-op kit is on this PC ($what in $o). This one is $(Get-KitName $kit); the other folder can be deleted once this one works." }
}
$reused = Copy-KitGameFilesFrom $kit $others
if ($reused) { Write-Host "Your game file (from the other kit): $reused" }

# 6. The rest: update, game build, start (play.ps1). The desktop shortcut follows the kit started last.
try {
    $desk = if ($env:OOT_COOP_DESKTOP) { $env:OOT_COOP_DESKTOP } else { [Environment]::GetFolderPath('Desktop') }   # (tests: a folder of their own)
    $link = Join-Path $desk 'Play OoT Co-op.lnk'
    if (Test-Path -LiteralPath $link) {
        $shell = New-Object -ComObject WScript.Shell
        $s = $shell.CreateShortcut($link)
        $mine = Join-Path $kit 'Start OoT Co-op.bat'
        if ($s.TargetPath -ine $mine) { $s.TargetPath = $mine; $s.WorkingDirectory = $kit; $s.Save() }
    }
} catch { }
$env:OOT_COOP_KIT = $kit
$next = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'play.ps1'), '-Mode', 'choose', '-Hookshot')
if ($NoUpdate) { $next += '-NoUpdate' }
& $ps @next
$code = $LASTEXITCODE
$lock.ReleaseMutex()
exit $code
