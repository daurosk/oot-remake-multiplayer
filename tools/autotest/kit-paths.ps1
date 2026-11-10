<#
.SYNOPSIS
Shows (and with -Fix corrects) which nand, sdmc, load and dump folders each autotest kit's emulator uses.

.DESCRIPTION
A kit made by copying another kit keeps that kit's absolute folders in
emulator\user\config\qt-config.ini. On 2026-11-09 both auto-A and auto-B used obj-A's NAND, so
the two test games shared one set of saves: every autosave of one game (the game saves on each
scene change) became the other's Continue too. -Fix points each folder back at the kit's own
emulator\user\<name> (the old qt-config.ini is kept as qt-config.ini.bak-<time>). Close the
kit's emulator first: it writes qt-config.ini when it closes.

.EXAMPLE
tools\autotest\kit-paths.ps1
tools\autotest\kit-paths.ps1 -Fix
#>
param(
    # Kit folders (default: auto-A and auto-B in the first coop-kit folder found going up).
    [string[]]$Kit = @(),
    [switch]$Fix
)
$ErrorActionPreference = 'Stop'

if (-not $Kit.Count) {
    $root = $null
    for ($d = Get-Item $PSScriptRoot; $d; $d = $d.Parent) {
        if (Test-Path (Join-Path $d.FullName 'coop-kit')) { $root = Join-Path $d.FullName 'coop-kit'; break }
    }
    if (-not $root) { throw "no coop-kit folder found; pass -Kit" }
    $Kit = @("$root\auto-A", "$root\auto-B")
}

$names = 'nand', 'sdmc', 'load', 'dump'
$nands = @{}
foreach ($k in $Kit) {
    $k = [IO.Path]::GetFullPath($k)
    $user = Join-Path $k 'emulator\user'
    $ini = Join-Path $user 'config\qt-config.ini'
    if (-not (Test-Path $ini)) { Write-Host "${k}: no $ini (the emulator uses its own user folder)"; continue }
    $text = Get-Content -Raw $ini
    $changed = $false
    foreach ($n in $names) {
        $own = (Join-Path $user $n)
        $m = [regex]::Match($text, "(?m)^${n}_directory=(.*?)\r?$")
        $isDefault = $text -match "(?m)^${n}_directory\\default=true\r?$"
        $now = if (-not $isDefault -and $m.Success -and $m.Groups[1].Value) { [IO.Path]::GetFullPath($m.Groups[1].Value) } else { $own }
        $ok = $now -eq [IO.Path]::GetFullPath($own)
        Write-Host ("{0} {1,-4} {2} {3}" -f (Split-Path -Leaf $k), $n, $(if ($ok) { 'own  ' } else { 'OTHER' }), $now)
        if ($n -eq 'nand') { $nands[$k] = $(if ($Fix) { [IO.Path]::GetFullPath($own) } else { $now }) }
        if (-not $ok -and $Fix) {
            $want = $own -replace '\\', '/'
            if ($m.Success) { $text = $text.Remove($m.Groups[1].Index, $m.Groups[1].Length).Insert($m.Groups[1].Index, $want) }
            else { Write-Host "ERROR: $k has no ${n}_directory key; set it by hand in [Data%20Storage]"; continue }
            $text = [regex]::Replace($text, "(?m)^(${n}_directory\\default=)\w+", '${1}false')
            $changed = $true
        }
    }
    if ($changed) {
        if (Get-Process lichii -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$k\*" }) {
            throw "${k}: its emulator is running; close it first (it rewrites qt-config.ini when it closes)"
        }
        Copy-Item $ini "$ini.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
        [IO.File]::WriteAllText($ini, $text)
        Write-Host "${k}: qt-config.ini now uses the kit's own folders (old file kept as .bak)"
    }
}
$shared = $nands.GetEnumerator() | Group-Object Value | Where-Object Count -gt 1
foreach ($g in $shared) { Write-Host "WARNING: $($g.Group.Name -join ' and ') share the NAND $($g.Name)$(if (-not $Fix) { ': run with -Fix' })" }
