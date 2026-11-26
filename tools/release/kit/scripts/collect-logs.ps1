# Zips what is needed to see what went wrong: the emulator logs, coop.ini, the server
# logs, VERSION.txt and a short note about this PC. Crash dumps only with -WithCrashDumps
# (they are memory images and cannot be masked).
# Every text file is masked in the copy (log-mask.ps1): IP addresses become their kind
# (<lan-1>, <public-1>, ...), MAC addresses <mac>, the Windows user name <user>, the PC
# name <pc>. The originals are not changed.
# Files still open (a running emulator or server) are copied as they are.
param([switch]$WithCrashDumps)
$ErrorActionPreference = 'Stop'
# Says at once that it runs, then one line per step and file: masking a 30 MB game log takes a while, and a blank
# window looked dead to players (issue #67). collect-logs.cmd says the first line itself (before windows-checks).
if (-not $env:OOT_COOP_COLLECT_SAID) { Write-Host 'Collecting your logs (this can take a few minutes)...' }
# QuickEdit off first: a click in this window would pause the collecting (issue #68; console-mode.ps1, never fails).
$consoleMode = Join-Path $PSScriptRoot 'console-mode.ps1'
if (Test-Path -LiteralPath $consoleMode) { . $consoleMode; [void](Disable-KitQuickEdit) }
. (Join-Path $PSScriptRoot 'log-mask.ps1')
$mask = New-MaskState
$kit = Split-Path -Parent $PSScriptRoot
$user = Join-Path $kit 'emulator\user'
$stamp = '{0:yyyyMMdd-HHmmss}' -f (Get-Date)
$zip = Join-Path $kit "coop-logs-$stamp.zip"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) "coop-logs-$stamp-$PID"
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$utf8 = New-Object System.Text.UTF8Encoding($false)
function Format-KitSize([long]$Bytes) {
    if ($Bytes -ge 1MB) { return '{0:N1} MB' -f ($Bytes / 1MB) }
    if ($Bytes -ge 1KB) { return '{0:N0} KB' -f ($Bytes / 1KB) }
    return "$Bytes bytes"
}
function Copy-Shared([string]$From, [string]$To, [bool]$Binary) {
    # Opens with read/write sharing so a log the emulator still writes can be read.
    # Text is masked on the way; only the copy is written.
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
    $in = [System.IO.File]::Open($From, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        if ($Binary) {
            $out = [System.IO.File]::Create($To)
            try { $in.CopyTo($out) } finally { $out.Dispose() }
        } else {
            $reader = New-Object System.IO.StreamReader($in, $utf8, $true)
            $text = $reader.ReadToEnd()
            [System.IO.File]::WriteAllText($To, (Protect-LogText $text $mask), $utf8)
        }
    } finally { $in.Dispose() }
}

$picked = @()
$hasEmulator = Test-Path -LiteralPath $user   # (a Longshot-only kit has none: its files are not "missing")
if ($hasEmulator) {
    $picked += Get-ChildItem (Join-Path $user 'log') -File -ErrorAction SilentlyContinue | ForEach-Object { @($_.FullName, "emulator\log\$($_.Name)") }
    $picked += @((Join-Path $user 'config\coop.ini'), 'emulator\coop.ini')
}
$picked += Get-ChildItem (Join-Path $kit 'logs') -Filter '*.log' -File -ErrorAction SilentlyContinue | ForEach-Object { @($_.FullName, "server\$($_.Name)") }
# What Windows itself may block (windows-checks.ps1, written by every start): Smart App Control, the internet mark, policies.
foreach ($wcLog in @((Join-Path $kit 'logs\windows-checks.txt'), $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'OoT-Coop\windows-checks.txt' }))) {
    # (the second when the kit's folder took no file)
    if ($wcLog -and (Test-Path -LiteralPath $wcLog)) { $picked += @($wcLog, 'windows-checks.txt'); break }
}
$picked += @((Join-Path $kit 'VERSION.txt'), 'VERSION.txt')
$picked += @((Join-Path $kit 'coop-options.ini'), 'coop-options.ini')
# What Start Squadala's window and the update said, this start and the one before (kit-paths.ps1 Start-KitStartLog,
# update.ps1 Start-KitUpdateLog; player report 266: a message on the start window was not in the zip).
foreach ($startLog in 'start-log.txt', 'start-log.1.txt') {
    $startPath = Join-Path $kit "hookshot\logs\$startLog"
    if (Test-Path -LiteralPath $startPath) { $picked += @($startPath, $startLog) }
}
# What the puppet pack was built from (file names and hashes only, no game data).
if ($hasEmulator) {
    $picked += @((Join-Path $user 'load\0400486024DEC000\coop-puppet\coop_puppet.json'), 'emulator\coop_puppet.json')
}
# The Longshot game: its logs, coop.ini and pack manifest, and the build's logs. A kit-built game has longshot.exe in
# bin\ and Longshot writes its log and coop.ini into the game folder above it (Get-KitHookshotLogDirs): both looked in.
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$hsGame = Get-KitHookshotGame $kit
# The start's game first, then the other places a kit's game can be (Get-KitLogGameExes): the first with a log.
$hsExes = @(Get-KitLogGameExes $kit)
$hsExe = $hsExes | Where-Object { @(Get-KitHookshotLogDirs $_ | ForEach-Object { Get-KitHookshotLogFiles $_ } | Where-Object { $_.File.Name -like 'longshot_log*.txt' }).Count } | Select-Object -First 1
if (-not $hsExe) { $hsExe = $hsExes | Select-Object -First 1 }
$gameLogs = 0
# Logs a player copied into the kit's logs folder (what to do when the game is not found, below).
$copied = @(Get-ChildItem (Join-Path $kit 'logs') -Filter 'longshot_log*.txt' -File -ErrorAction SilentlyContinue)
$gameLogs += $copied.Count
$picked += $copied | ForEach-Object { @($_.FullName, "hookshot\copied\$($_.Name)") }
if ($hsExe) {
    $hsDirs = @(Get-KitHookshotLogDirs $hsExe)
    foreach ($hsDir in $hsDirs) {
        $to = if ($hsDir -ne $hsDirs[0]) { 'hookshot\bin' } else { 'hookshot' }
        # (Longshot rc.4 writes its log into logs\, older games beside the exe: Get-KitHookshotLogFiles has both)
        $logs = @(Get-KitHookshotLogFiles $hsDir)
        $gameLogs += $logs.Count
        $picked += $logs | ForEach-Object { @($_.File.FullName, $(if ($_.Sub) { "$to\$($_.Sub)\$($_.File.Name)" } else { "$to\$($_.File.Name)" })) }
        # (last-build.txt: OOTRR's output when a randomizer seed failed; masked like every text file)
        # (tools\ootrr-setup.txt: how the kit's last OOTRR setup went; tools\ootrr\coop-ootrr.txt: the OOTRR in place)
        foreach ($f in 'coop.ini', 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json', 'mods\oot-remake-randomizer\last-build.txt',
                'tools\ootrr-setup.txt', 'tools\ootrr\coop-ootrr.txt') {
            $p = Join-Path $hsDir $f
            if (Test-Path -LiteralPath $p) { $picked += @($p, "$to\$(Split-Path -Leaf $p)") }
        }
    }
}
# Split screen with Nucleus Co-op (tools\nucleus\OoTCoop.js): each player's game runs from its own instance folder in
# Nucleus's folder and keeps its log and coop.ini there. The handler lists them in nucleus-players\instances.txt beside
# the game ("<player>=<folder>"); without that file (an older handler) Nucleus's usual C:\Nucleus\content\OoTCoopLongshot.
# Nothing is added when Nucleus is not used.
$nucInst = @()
foreach ($hsExe2 in $hsExes) {
    foreach ($d in (Get-KitHookshotLogDirs $hsExe2)) {
        $list = Join-Path $d 'nucleus-players\instances.txt'
        if (Test-Path -LiteralPath $list) {
            $nucInst += @(Get-Content -LiteralPath $list -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\d+=(.+)$' } | ForEach-Object { $Matches[1].Trim() })
        }
    }
}
if (-not $nucInst) { $nucInst = @(Get-ChildItem 'C:\Nucleus\content\OoTCoopLongshot' -Directory -Filter 'Instance*' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }) }
foreach ($inst in @($nucInst | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) } | Select-Object -Unique)) {
    $to = "nucleus\$(Split-Path -Leaf $inst)"
    $logs = @(Get-KitHookshotLogFiles $inst)
    # (an instance made by an older handler has logs\ linked to the game's own: that log is the kit's, zipped above)
    $logsDir = Join-Path $inst 'logs'
    if ((Test-Path -LiteralPath $logsDir) -and ((Get-Item -LiteralPath $logsDir -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        $logs = @($logs | Where-Object { $_.Sub -ne 'logs' })
    }
    $gameLogs += $logs.Count
    $picked += $logs | ForEach-Object { @($_.File.FullName, $(if ($_.Sub) { "$to\$($_.Sub)\$($_.File.Name)" } else { "$to\$($_.File.Name)" })) }
    foreach ($f in 'coop.ini', 'coop-options.ini', 'coop-player.ini') {
        $picked += @((Join-Path $inst $f), "$to\$f")
    }
}
# Local co-op (Start Squadala with 2 to 4 players on this PC, scripts\local-coop.ps1): games 2 to 4 run from
# <game>\local-players\playerN, each with its own logs and coop.ini / coop-player.ini (game 1 is the kit's own game, above).
foreach ($hsExe2 in $hsExes) {
    foreach ($d in (Get-KitHookshotLogDirs $hsExe2)) {
        foreach ($inst in @(Get-ChildItem (Join-Path $d 'local-players') -Directory -Filter 'player*' -ErrorAction SilentlyContinue)) {
            $to = "local-players\$($inst.Name)"
            $logs = @(Get-KitHookshotLogFiles $inst.FullName)
            $gameLogs += $logs.Count
            $picked += $logs | ForEach-Object { @($_.File.FullName, $(if ($_.Sub) { "$to\$($_.Sub)\$($_.File.Name)" } else { "$to\$($_.File.Name)" })) }
            foreach ($f in 'coop.ini', 'coop-player.ini') {
                $picked += @((Join-Path $inst.FullName $f), "$to\$f")
            }
        }
    }
}
$logHelp = @()
if ($gameLogs -eq 0 -and ($hsExe -or -not $hasEmulator)) {
    # The log we need most: say where it is and how to add it.
    $where = if ($hsExe) { (@(Get-KitHookshotLogDirs $hsExe | ForEach-Object { $_; Join-Path $_ 'logs' }) -join ' or ') } else { $hsGame }
    $logHelp += "The game's log (longshot_log.txt) is not in this zip: none in $where."
    $logHelp += $(if ($hsExe) { '  Start the game once (it writes the log when it starts), close it, then run collect-logs.cmd again.' }
        else { '  The game was not found. Its log is in the co-op game''s folder, in its logs folder or beside longshot.exe (normally hookshot\game in this kit).' })
    $logHelp += "  Found it somewhere else? Copy longshot_log.txt into $(Join-Path $kit 'logs') and run collect-logs.cmd again,"
    $logHelp += '  or send it with this zip.'
}
$picked += Get-ChildItem (Join-Path $kit 'hookshot') -Filter '*.log' -File -ErrorAction SilentlyContinue | ForEach-Object { @($_.FullName, "hookshot\build\$($_.Name)") }
if ($WithCrashDumps) {
    $picked += Get-ChildItem (Join-Path $user 'crash_dumps') -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 2 | ForEach-Object { @($_.FullName, "emulator\crash_dumps\$($_.Name)") }
}

$found = 0
Write-Host 'Copying the files (addresses and names are masked in each):'
for ($i = 0; $i -lt $picked.Count; $i += 2) {
    if (Test-Path -LiteralPath $picked[$i]) {
        $size = try { (Get-Item -LiteralPath $picked[$i] -Force).Length } catch { 0 }
        Write-Host "  $($picked[$i + 1]) ($(Format-KitSize $size))"
        Copy-Shared $picked[$i] (Join-Path $stage $picked[$i + 1]) ($picked[$i + 1] -like 'emulator\crash_dumps\*')
        $found++
    } else {
        Write-Host "Missing: $(Protect-LogText $picked[$i] $mask)"
    }
}

Write-Host 'Noting this PC (Windows version, graphics, CPU, memory and what uses it, network adapters)...'
# No PC name and no addresses: only each adapter's name and the kind of its address.
$about = @(
    "collected: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC",
    "windows: $([System.Environment]::OSVersion.VersionString)"
)
try { $about += Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { "gpu: $($_.Name), driver $($_.DriverVersion)" } } catch {}
# For low frame rates (docs/setup.md, "Low frame rate"): the CPU, memory, battery and power plan.
try { $about += Get-CimInstance Win32_Processor -ErrorAction Stop | ForEach-Object { "cpu: $($_.Name.Trim()), $($_.NumberOfCores) cores, $($_.NumberOfLogicalProcessors) threads" } } catch {}
try { $about += Get-CimInstance Win32_ComputerSystem -ErrorAction Stop | ForEach-Object { 'memory: {0:N1} GB' -f ($_.TotalPhysicalMemory / 1GB) } } catch {}
# Memory now (issue 85: a host's game crashed with the PC's commit limit nearly used up). RAM,
# the commit charge and its limit, the page files' sizes, and the 10 programs holding the most
# private (committed) memory, by name only (no paths); each name is masked like the rest.
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    $about += 'memory now: RAM {0:N1} GB free of {1:N1} GB; commit {2:N1} GB used of {3:N1} GB limit' -f (
        $os.FreePhysicalMemory / 1MB), ($os.TotalVisibleMemorySize / 1MB),
        (($os.TotalVirtualMemorySize - $os.FreeVirtualMemory) / 1MB), ($os.TotalVirtualMemorySize / 1MB)
} catch {}
try {
    $auto = (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).AutomaticManagedPagefile
    $about += "page file: $(if ($auto) { 'managed by Windows' } else { 'set by hand' })"
    $about += Get-CimInstance Win32_PageFileUsage -ErrorAction Stop | ForEach-Object {
        'page file: drive {0}, {1:N1} GB, {2:N1} GB in use, peak {3:N1} GB' -f $_.Name.Substring(0, 1),
            ($_.AllocatedBaseSize / 1KB), ($_.CurrentUsage / 1KB), ($_.PeakUsage / 1KB)
    }
} catch {}
try {
    $about += Get-Process -ErrorAction Stop | Group-Object ProcessName | ForEach-Object {
        [pscustomobject]@{ Name = $_.Name; Count = $_.Count; Bytes = ($_.Group | Measure-Object PrivateMemorySize64 -Sum).Sum }
    } | Sort-Object Bytes -Descending | Select-Object -First 10 | ForEach-Object {
        'top memory: {0} ({1} running), {2:N2} GB private' -f (Protect-LogText $_.Name $mask), $_.Count, ($_.Bytes / 1GB)
    }
} catch {}
try {
    $about += Get-CimInstance Win32_Battery -ErrorAction Stop | ForEach-Object {
        "battery: $($_.EstimatedChargeRemaining)%, $(if ($_.BatteryStatus -eq 2) { 'plugged in' } else { 'NOT plugged in' })"
    }
} catch {}
try { $plan = (& powercfg /getactivescheme) -join ' '; if ($plan) { $about += "power plan: $($plan.Trim())" } } catch {}
# The emulator's graphics and speed settings: only these keys from qt-config.ini, never the
# whole file (it can hold an account token).
$qtConfig = Join-Path $user 'config\qt-config.ini'
if (Test-Path -LiteralPath $qtConfig) {
    $keys = 'backend|vulkan_device|resolution_setup|gpu_accuracy|use_vsync|oot_30fps|use_asynchronous_shaders|use_disk_shader_cache|scaling_filter|anti_aliasing|oot_anti_aliasing|use_speed_limit|speed_limit|cpu_accuracy|use_multi_core|astc_recompression|nvidia_dlss|frame_gen|frame_gen_multiplier'
    try {
        $about += [System.IO.File]::ReadAllLines($qtConfig) | Where-Object { $_ -match "^($keys)=" } | ForEach-Object { "setting: $_" }
    } catch {}
}
try {
    $about += Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
        Where-Object { $_.IPAddress -notlike '169.254.*' } | ForEach-Object { "ip: $(Protect-LogText $_.IPAddress $mask) ($(Protect-LogText $_.InterfaceAlias $mask))" }
} catch {}
$about += Get-Process lichii, coop_server -ErrorAction SilentlyContinue | ForEach-Object { "running: $($_.ProcessName) $(Protect-LogText $_.Path $mask)" }
[System.IO.File]::WriteAllLines((Join-Path $stage 'about-this-pc.txt'), [string[]]$about)

# A standard zip on every PC ('/' names, one entry per file; log-mask.ps1). Not tar.exe or
# Compress-Archive: a player's PC without tar.exe got Compress-Archive's '\' names.
Write-Host 'Making the zip...'
Write-LogZip $stage $zip
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host ''
Write-Host "Wrote $zip"
Write-Host "  ($($found + 1) files, $gameLogs of them game logs; the folder opens with the zip selected)"
if (-not $env:CI) { try { Start-Process explorer.exe "/select,`"$zip`"" } catch {} }
Write-Host 'IP addresses, MAC addresses, passwords, your Windows user name and PC name are masked in it.'
if ($logHelp) { Write-Host ''; foreach ($l in $logHelp) { Write-Host (Protect-LogText $l $mask) -ForegroundColor Yellow } }
Write-Host ''
Write-Host 'To report it: on the Squadala! Discord (https://discord.gg/GwtRKAqng5) type /bugreport, say what happened'
Write-Host 'and attach this zip. Do not attach it to a public issue.'
Write-Host "Send this zip: $zip" -ForegroundColor Green
