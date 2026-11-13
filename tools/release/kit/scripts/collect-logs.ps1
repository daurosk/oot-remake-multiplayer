# Zips what is needed to see what went wrong: the emulator logs, coop.ini, the server
# logs, VERSION.txt and a short note about this PC. Crash dumps only with -WithCrashDumps
# (they are memory images and cannot be masked).
# Every text file is masked in the copy (log-mask.ps1): IP addresses become their kind
# (<lan-1>, <public-1>, ...), MAC addresses <mac>, the Windows user name <user>, the PC
# name <pc>. The originals are not changed.
# Files still open (a running emulator or server) are copied as they are.
param([switch]$WithCrashDumps)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'log-mask.ps1')
$mask = New-MaskState
$kit = Split-Path -Parent $PSScriptRoot
$user = Join-Path $kit 'emulator\user'
$stamp = '{0:yyyyMMdd-HHmmss}' -f (Get-Date)
$zip = Join-Path $kit "coop-logs-$stamp.zip"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) "coop-logs-$stamp-$PID"
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$utf8 = New-Object System.Text.UTF8Encoding($false)
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
$picked += Get-ChildItem (Join-Path $user 'log') -File -ErrorAction SilentlyContinue | ForEach-Object { @($_.FullName, "emulator\log\$($_.Name)") }
$picked += @((Join-Path $user 'config\coop.ini'), 'emulator\coop.ini')
$picked += Get-ChildItem (Join-Path $kit 'logs') -Filter '*.log' -File -ErrorAction SilentlyContinue | ForEach-Object { @($_.FullName, "server\$($_.Name)") }
$picked += @((Join-Path $kit 'VERSION.txt'), 'VERSION.txt')
$picked += @((Join-Path $kit 'coop-options.ini'), 'coop-options.ini')
# What the puppet pack was built from (file names and hashes only, no game data).
$picked += @((Join-Path $user 'load\0400486024DEC000\coop-puppet\coop_puppet.json'), 'emulator\coop_puppet.json')
# The Hookshot game (Hookshot kits): its log, coop.ini and pack manifest beside hookshot.exe, and the build's logs.
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$hsExe = Get-KitHookshotExe (Get-KitHookshotGame $kit)
if ($hsExe) {
    $hsDir = Split-Path -Parent $hsExe
    $picked += Get-ChildItem $hsDir -Filter 'hookshot_log*.txt' -File -ErrorAction SilentlyContinue | ForEach-Object { @($_.FullName, "hookshot\$($_.Name)") }
    $picked += @((Join-Path $hsDir 'coop.ini'), 'hookshot\coop.ini')
    $picked += @((Join-Path $hsDir 'mods\contents\0400486024DEC000\coop-puppet\coop_puppet.json'), 'hookshot\coop_puppet.json')
}
$picked += Get-ChildItem (Join-Path $kit 'hookshot') -Filter '*.log' -File -ErrorAction SilentlyContinue | ForEach-Object { @($_.FullName, "hookshot\build\$($_.Name)") }
if ($WithCrashDumps) {
    $picked += Get-ChildItem (Join-Path $user 'crash_dumps') -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 2 | ForEach-Object { @($_.FullName, "emulator\crash_dumps\$($_.Name)") }
}

$found = 0
for ($i = 0; $i -lt $picked.Count; $i += 2) {
    if (Test-Path -LiteralPath $picked[$i]) {
        Copy-Shared $picked[$i] (Join-Path $stage $picked[$i + 1]) ($picked[$i + 1] -like 'emulator\crash_dumps\*')
        $found++
    } else {
        Write-Host "Missing: $(Protect-LogText $picked[$i] $mask)"
    }
}

# No PC name and no addresses: only each adapter's name and the kind of its address.
$about = @(
    "collected: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC",
    "windows: $([System.Environment]::OSVersion.VersionString)"
)
try { $about += Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { "gpu: $($_.Name), driver $($_.DriverVersion)" } } catch {}
# For low frame rates (docs/setup.md, "Low frame rate"): the CPU, memory, battery and power plan.
try { $about += Get-CimInstance Win32_Processor -ErrorAction Stop | ForEach-Object { "cpu: $($_.Name.Trim()), $($_.NumberOfCores) cores, $($_.NumberOfLogicalProcessors) threads" } } catch {}
try { $about += Get-CimInstance Win32_ComputerSystem -ErrorAction Stop | ForEach-Object { 'memory: {0:N1} GB' -f ($_.TotalPhysicalMemory / 1GB) } } catch {}
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

$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (Test-Path $tar) {
    # Forward slashes in the zip, which every unzip tool reads (Compress-Archive in
    # Windows PowerShell 5.1 writes backslashes).
    & $tar -a -c -f $zip -C $stage .
    if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
} else {
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
}
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host "Wrote $zip ($($found + 1) files)."
Write-Host 'IP addresses, MAC addresses, passwords, your Windows user name and PC name are masked in it.'
Write-Host 'Send it privately (a Discord DM to the maintainer). Do not attach it to a public issue.'
