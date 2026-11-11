# Zips what is needed to see what went wrong: the emulator logs, coop.ini, the server
# logs, VERSION.txt, the newest crash dumps and a short note about this PC.
# Files still open (a running emulator or server) are copied as they are.
$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent $PSScriptRoot
$user = Join-Path $kit 'emulator\user'
$stamp = '{0:yyyyMMdd-HHmmss}' -f (Get-Date)
$zip = Join-Path $kit "coop-logs-$($env:COMPUTERNAME)-$stamp.zip"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) "coop-logs-$stamp-$PID"
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Copy-Shared([string]$From, [string]$To) {
    # Opens with read/write sharing so a log the emulator still writes can be read.
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
    $in = [System.IO.File]::Open($From, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        $out = [System.IO.File]::Create($To)
        try { $in.CopyTo($out) } finally { $out.Dispose() }
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
$picked += Get-ChildItem (Join-Path $user 'crash_dumps') -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 2 | ForEach-Object { @($_.FullName, "emulator\crash_dumps\$($_.Name)") }

$found = 0
for ($i = 0; $i -lt $picked.Count; $i += 2) {
    if (Test-Path -LiteralPath $picked[$i]) {
        Copy-Shared $picked[$i] (Join-Path $stage $picked[$i + 1])
        $found++
    } else {
        Write-Host "Missing: $($picked[$i])"
    }
}

$about = @(
    "pc: $($env:COMPUTERNAME)",
    "collected: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
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
        Where-Object { $_.IPAddress -notlike '169.254.*' } | ForEach-Object { "ip: $($_.IPAddress) ($($_.InterfaceAlias))" }
} catch {}
$about += Get-Process lichii, coop_server -ErrorAction SilentlyContinue | ForEach-Object { "running: $($_.ProcessName) $($_.Id) $($_.Path)" }
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
Write-Host "Wrote $zip ($($found + 1) files). Send this file back."
