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
