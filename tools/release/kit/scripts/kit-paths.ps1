# Keeps a kit copy pointed at its own folders. Dot-source it.
#
# The emulator saves its NAND, SD, load (mods), dump and TAS folders in
# user\config\qt-config.ini as absolute paths. A kit folder copied after its first run
# (local-A copied to local-B, or a kit unzipped over an old one) keeps reading the
# first copy's saves and mods. Repair-KitPaths points every such line that names a
# kit's emulator\user folder at this copy's own; a folder the player chose elsewhere
# is left as it is.

function Repair-KitPaths([string]$User) {
    $ini = Join-Path $User 'config\qt-config.ini'
    if (-not (Test-Path -LiteralPath $ini)) { return @() }
    $ours = ([System.IO.Path]::GetFullPath($User)).Replace('\', '/').TrimEnd('/')
    $fixed = @()
    $lines = [System.IO.File]::ReadAllLines($ini)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch '^(\w+_directory)=(.+?)/emulator/user(/[^/]*)?/?$') { continue }
        $key = $Matches[1]; $sub = $Matches[3]
        $want = "$ours$sub"
        $now = $lines[$i].Substring($key.Length + 1)
        if ($now -ne $want) {
            $lines[$i] = "$key=$want"
            $fixed += $key
        }
    }
    if ($fixed.Count) {
        # The emulator writes this file without a byte order mark; keep it so.
        [System.IO.File]::WriteAllText($ini, (($lines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
    }
    return $fixed
}

# The mods folder the emulator reads: qt-config.ini's load_directory, else user\load.
function Get-KitLoadDir([string]$User) {
    $ini = Join-Path $User 'config\qt-config.ini'
    if (Test-Path -LiteralPath $ini) {
        foreach ($line in [System.IO.File]::ReadAllLines($ini)) {
            if ($line -match '^load_directory=(.+)$') { return [System.IO.Path]::GetFullPath($Matches[1]) }
        }
    }
    return Join-Path $User 'load'
}

# Reads coop-options.ini: NAME=value lines for LICHII_COOP_* switches. Blank lines and
# lines starting with # or ; are skipped. Returns an ordered table.
function Read-KitOptions([string]$Path) {
    $values = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Path)) { return $values }
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#') -or $t.StartsWith(';')) { continue }
        if ($t -match '^(LICHII_COOP_[A-Z0-9_]+)\s*=\s*(.*)$') { $values[$Matches[1]] = $Matches[2] }
        else { Write-Host "coop-options.ini: skipped '$t' (only LICHII_COOP_NAME=value lines)" }
    }
    return $values
}
