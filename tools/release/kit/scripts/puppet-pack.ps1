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
# Windows PowerShell stops on a native program's error output under 'Stop'; this one
# is expected (no zstandard), so let it through.
$ErrorActionPreference = 'Continue'
& $exe @pre -c 'import zstandard' 2>$null
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Python is missing the zstandard package. In a Command Prompt:'
    Write-Host "  $($python -join ' ') -m pip install zstandard"
    exit 1
}

# The dump check: finds the emulator's dump (Dump RomFS writes to the dump folder) and
# says, one line per problem, what is wrong with it and what to do.
$check = Join-Path $kit 'tools\puppet_pack\dumpcheck.py'
function Test-Dump([string[]]$More) {
    $out = & $exe @pre -I $check --user-dir $user @More
    $code = $LASTEXITCODE
    $found = $null
    foreach ($line in $out) {
        if ($line -like 'romfs=*') { $found = $line.Substring(6) } else { Write-Host $line }
    }
    return @{ Code = $code; Romfs = $found }
}

if (-not $Romfs) {
    $r = Test-Dump @()
    if ($r.Code -eq 1) { Write-Host 'Fix the dump as above, then run make-puppet-pack.cmd again.'; exit 1 }
    if ($r.Code -eq 0) {
        $Romfs = $r.Romfs
        Write-Host "Found the game's RomFS dump: $Romfs"
    } else {
        Write-Host ''
        Write-Host "If your RomFS dump is somewhere else, type its folder (the one that holds Pack, Model, AS, RSDB and System); else press Enter to stop."
        $answer = Read-Host 'Romfs folder'
        if (-not $answer) { exit 1 }
        $Romfs = $answer.Trim().Trim('"')
    }
}
$r = Test-Dump @('--romfs', $Romfs)
if ($r.Code -ne 0) { Write-Host 'Fix the dump as above, then run make-puppet-pack.cmd again.'; exit 1 }
$Romfs = $r.Romfs

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
Write-Host 'Done. Start Host.bat or Join.bat; the emulator log shows "LayeredFS patches applied" when it loads the game.'
