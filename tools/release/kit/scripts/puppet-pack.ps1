# Builds the puppet Link packs from this PC's own copy of the game into the kit's
# emulator (a LayeredFS mod in the emulator's load folder). Nothing from the game is in
# the kit: the packs are made here, from the romfs you dumped, and stay on this PC.
# Usage: make-puppet-pack.cmd                   asks for the romfs folder
#        make-puppet-pack.cmd -Romfs <folder>   no questions
param([string]$Romfs = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$kit = Split-Path -Parent $PSScriptRoot
$user = Join-Path $kit 'emulator\user'
$tool = Join-Path $kit 'tools\puppet_pack\build.py'
if (-not (Test-Path $tool)) { Write-Host "Not found: $tool"; exit 1 }

# A copied kit must write into its own load folder, not the first copy's.
$fixed = Repair-KitPaths $user
if ($fixed) { Write-Host "This kit was copied: pointed $($fixed -join ', ') at $user" }

# Python: the one in the kit, else the PC's own.
$python = $null
$bundled = Join-Path $kit 'python\python.exe'
if (Test-Path $bundled) { $python = @($bundled) }
elseif (Get-Command py.exe -ErrorAction SilentlyContinue) { $python = @('py.exe', '-3') }
elseif (Get-Command python.exe -ErrorAction SilentlyContinue) { $python = @('python.exe') }
if (-not $python) {
    Write-Host 'No Python found. Install Python 3 from python.org, then in a Command Prompt:'
    Write-Host '  py -m pip install zstandard'
    exit 1
}
$exe = $python[0]; $pre = @($python | Select-Object -Skip 1)
& $exe @pre -c 'import zstandard' 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Python is missing the zstandard package. In a Command Prompt:'
    Write-Host "  $($python -join ' ') -m pip install zstandard"
    exit 1
}

if (-not $Romfs) {
    # The emulator's Dump RomFS writes here.
    $dumped = Get-ChildItem (Join-Path $user 'dump') -Directory -Recurse -Filter romfs -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'Pack\ZsDic.pack.zs') } | Select-Object -First 1
    $hint = if ($dumped) { $dumped.FullName } else { '' }
    Write-Host "The game's romfs folder: the one that holds Pack, Model, AS, RSDB and System (TESTING.md says how to dump it)."
    $answer = Read-Host "Romfs folder$(if ($hint) { " [$hint]" })"
    $Romfs = if ($answer) { $answer.Trim().Trim('"') } else { $hint }
}
if (-not $Romfs -or -not (Test-Path -LiteralPath (Join-Path $Romfs 'Pack\ZsDic.pack.zs'))) {
    Write-Host "Not a romfs folder (no Pack\ZsDic.pack.zs): $Romfs"
    exit 1
}

# The longest file it writes; Python (and the emulator) cannot use paths past 259
# characters unless Windows long paths are on.
$loadDir = Get-KitLoadDir $user
$longest = Join-Path $loadDir '0400486024DEC000\coop-puppet\romfs\System\Resource\ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs.tmp'
if ($longest.Length -gt 259) {
    Write-Host "The kit folder is too deep for Windows ($($longest.Length) characters in the longest path, 259 at most)."
    Write-Host 'Move the kit to a shorter folder, for example C:\oot-coop, and run this again.'
    exit 1
}

Write-Host "Building the puppet packs into $loadDir ..."
& $exe @pre -I $tool --romfs $Romfs --user-dir $user
if ($LASTEXITCODE -ne 0) { Write-Host "The pack build failed ($LASTEXITCODE)."; exit 1 }
Write-Host 'Done. Start play.cmd; the emulator log shows "LayeredFS patches applied" when it loads the game.'
