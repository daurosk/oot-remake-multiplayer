# The checks "Start OoT Co-op.bat" (start.ps1) runs before the game starts, one function each, so
# tools/release/tests/launch_test.ps1 can try each case without a game. Each says what is wrong in
# plain words; start.ps1 decides what to do about it.

# True when the kit runs from inside a zip: Windows unpacks a file started from an open zip into a
# temp folder (Temp1_<name>.zip, or a folder named like the zip) and runs it from there.
function Test-KitInZip([string]$Kit, [string]$Temp = $env:TEMP) {
    $full = [System.IO.Path]::GetFullPath($Kit).TrimEnd('\')
    foreach ($part in $full.Split('\')) {
        if ($part -match '(?i)\.zip$' -or $part -match '(?i)^Temp\d+_') { return $true }
    }
    if ($Temp) {
        $t = [System.IO.Path]::GetFullPath($Temp).TrimEnd('\') + '\'
        if (($full + '\').StartsWith($t, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

# The OneDrive folders of this user (Desktop and Documents are often in one).
function Get-KitOneDriveRoots {
    @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial) | Where-Object { $_ } | Sort-Object -Unique
}

# Why this folder is a bad place for the game build, or '':
#   'onedrive' inside OneDrive (it would upload the 60 GB the build writes, and its online-only files break it)
#   'unicode'  letters outside plain ASCII in the path (Hookshot's build tools cannot take them)
#   'long'     a path so long the build's deep folders pass Windows' 260-letter limit
# Spaces are fine (the test kits ran in "OOT REMAKE\coop-kit\...").
function Get-KitPathProblem([string]$Kit, [string[]]$OneDriveRoots = @(Get-KitOneDriveRoots)) {
    $full = [System.IO.Path]::GetFullPath($Kit).TrimEnd('\')
    foreach ($root in $OneDriveRoots) {
        $r = [System.IO.Path]::GetFullPath($root).TrimEnd('\')
        if (($full + '\').StartsWith($r + '\', [StringComparison]::OrdinalIgnoreCase)) { return 'onedrive' }
    }
    if ($full -match '\\OneDrive( - [^\\]+)?(\\|$)') { return 'onedrive' }
    if ($full -match '[^\x20-\x7E]') { return 'unicode' }
    if ($full.Length -gt 80) { return 'long' }
    return ''
}

function Get-KitPathProblemText([string]$Problem) {
    switch ($Problem) {
        'onedrive' { 'This folder is in OneDrive. The game build writes about 60 GB, which OneDrive would try to upload, and OneDrive''s online-only files break the build.' }
        'unicode' { 'This folder''s path has letters the game build cannot handle (only plain A-Z letters, digits and spaces work).' }
        'long' { 'This folder''s path is too long for the game build (Windows allows 260 letters, and the build goes many folders deep).' }
        'readonly' { 'This folder cannot be written to without administrator rights (for example under Program Files).' }
        default { '' }
    }
}

# True when this user can write in the kit folder (no admin rights needed or used).
function Test-KitWritable([string]$Kit) {
    $probe = Join-Path $Kit ('logs\write-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.tmp')
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $probe) -ErrorAction Stop | Out-Null
        [System.IO.File]::WriteAllText($probe, 'ok')
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        return $true
    } catch { return $false }
}

# A short folder for the kit when this one cannot be used: C:\OoT-Coop (a new name when that one is taken
# by something else), else C:\Users\Public\OoT-Coop. Both are plain ASCII and outside OneDrive.
function Get-KitMoveTarget([string[]]$Bases = @("$env:SystemDrive\OoT-Coop", "$env:SystemDrive\Users\Public\OoT-Coop")) {
    foreach ($base in $Bases) {
        for ($i = 1; $i -le 9; $i++) {
            $d = if ($i -eq 1) { $base } else { "$base-$i" }
            if (-not (Test-Path -LiteralPath $d)) { return $d }
            if (-not @(Get-ChildItem -LiteralPath $d -Force -ErrorAction SilentlyContinue)) { return $d }
        }
    }
    return ''
}

# The kit's own files (MANIFEST.txt) that are not there. A missing .exe or .dll is most often an
# antivirus quarantine; anything else a damaged unpack.
function Get-KitMissingFiles([string]$Kit) {
    $manifest = Join-Path $Kit 'MANIFEST.txt'
    if (-not (Test-Path -LiteralPath $manifest)) { return @() }
    @(Get-Content -LiteralPath $manifest | Where-Object { $_ -and $_ -notmatch '^#' } |
        Where-Object { -not (Test-Path -LiteralPath (Join-Path $Kit $_)) })
}

function Get-KitMissingFilesText([string[]]$Missing) {
    $bin = @($Missing | Where-Object { $_ -match '(?i)\.(exe|dll)$' })
    $first = ($Missing | Select-Object -First 3) -join ', '
    if ($bin) {
        return @(
            "Your antivirus has most likely removed a file of this kit: $first.",
            'Windows Security: Virus & threat protection > Protection history: restore it, and add this kit''s folder under',
            '"Exclusions". Or unzip the kit again. Then start "Start OoT Co-op.bat" again.')
    }
    return @("Files of this kit are missing ($first): the unzip did not finish. Unzip the kit again, then start ""Start OoT Co-op.bat"".")
}

# The Visual C++ runtime the co-op module and the server need: the DLLs not in the kit's hookshot\ folder
# nor in Windows (a kit ships them; an antivirus or a partial unzip may lose them).
function Get-KitMissingRuntime([string]$Kit, [string]$System32 = "$env:SystemRoot\System32") {
    @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $Kit "hookshot\$_")) -and -not (Test-Path -LiteralPath (Join-Path $System32 $_))
    }
}

# A built game whose files are gone (coop-built-from.txt is there, hookshot.exe or the co-op module is not):
# an antivirus took them. Said, not rebuilt (a rebuild is 45 minutes and would be taken again).
function Get-KitGameQuarantined([string]$Game) {
    if (-not (Test-Path -LiteralPath (Join-Path $Game 'coop-built-from.txt'))) { return '' }
    $exe = @((Join-Path $Game 'bin\hookshot.exe'), (Join-Path $Game 'hookshot.exe')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $exe) { return 'hookshot.exe' }
    if (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $exe) 'hookshot_coop.dll'))) { return 'hookshot_coop.dll' }
    return ''
}

# One start at a time per kit folder: a second double-click while the first window is still busy (an
# update, the first game build) gets $null. The mutex goes when its window closes.
function Enter-KitStartLock([string]$Kit) {
    $full = [System.IO.Path]::GetFullPath($Kit).TrimEnd('\').ToLowerInvariant()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $id = -join ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($full)) | Select-Object -First 8 | ForEach-Object { $_.ToString('x2') })
    $created = $false
    $m = New-Object System.Threading.Mutex($true, "Local\OoT-Coop-Start-$id", [ref]$created)
    if ($created) { return $m }
    try { if ($m.WaitOne(0)) { return $m } } catch [System.Threading.AbandonedMutexException] { return $m }
    $m.Dispose()
    return $null
}

# The kit folders started on this PC (one per line in ListFile), this one added. Returns the others still there.
function Register-KitFolder([string]$ListFile, [string]$Kit) {
    $full = [System.IO.Path]::GetFullPath($Kit).TrimEnd('\')
    $all = @()
    if (Test-Path -LiteralPath $ListFile) { $all = @(Get-Content -LiteralPath $ListFile | Where-Object { $_ }) }
    $others = @($all | Where-Object { $_ -ine $full } | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'VERSION.txt') } | Sort-Object -Unique)
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ListFile) | Out-Null
        [System.IO.File]::WriteAllLines($ListFile, [string[]](@($others) + $full))
    } catch { }
    return $others
}

# The kit's name line in VERSION.txt ("kit: oot-coop-<version> ..."), or ''.
function Get-KitName([string]$Kit) {
    $v = Join-Path $Kit 'VERSION.txt'
    if (-not (Test-Path -LiteralPath $v)) { return '' }
    $line = "$(Get-Content -LiteralPath $v -TotalCount 1)"
    if ($line -match '^kit:\s*(\S+)') { return $Matches[1] }
    return ''
}

# The game file another kit on this PC was built from, for a new kit that has none yet, so the new kit
# asks no question. Returns the path taken, or ''.
function Copy-KitGameFilesFrom([string]$Kit, [string[]]$Others) {
    $mine = Join-Path $Kit 'hookshot\game-files.txt'
    if (Test-Path -LiteralPath $mine) { return '' }
    foreach ($o in $Others) {
        $theirs = Join-Path $o 'hookshot\game-files.txt'
        if (-not (Test-Path -LiteralPath $theirs)) { continue }
        $g = "$(Get-Content -LiteralPath $theirs -TotalCount 1)".Trim()
        if ($g -and (Test-Path -LiteralPath $g)) {
            try {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $mine) | Out-Null
                [System.IO.File]::WriteAllText($mine, "$g`n")
                return $g
            } catch { return '' }
        }
    }
    return ''
}
