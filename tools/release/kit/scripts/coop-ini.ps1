# Reading and writing coop.ini, shared by play.ps1 and make_kit.ps1. Dot-source it.
# The rules match the emulator's own (lichii/hook/coop_settings.cpp), so a value this
# accepts is never turned down by the emulator.

function Test-CoopServer([string]$Value) {
    if ($Value -notmatch '^([A-Za-z0-9._-]{1,253})(:(\d{1,5}))?$') { return $false }
    if ($Matches[3]) { $p = [int]$Matches[3]; return ($p -ge 1 -and $p -le 65535) }
    return $true
}

function Test-CoopName([string]$Value) {
    if ($Value -match '[\x00-\x1f\x7f]') { return $false }
    $chars = [System.Globalization.StringInfo]::new($Value).LengthInTextElements
    # The emulator counts UTF-8 code points; text elements are never more than those.
    $points = 0
    for ($i = 0; $i -lt $Value.Length; $i++) {
        if (-not [char]::IsLowSurrogate($Value[$i])) { $points++ }
    }
    return ($chars -ge 1 -and $points -le 16)
}

# Returns @{ server; port; name } from an existing coop.ini (missing keys are empty).
function Read-CoopIni([string]$Path) {
    $values = @{ server = ''; port = ''; name = '' }
    if (Test-Path $Path) {
        foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
            if ($line -match '^\s*(server|port|name)\s*=\s*(.*?)\s*$') { $values[$Matches[1]] = $Matches[2] }
        }
    }
    return $values
}

# Writes coop.ini from the template. An empty server or name leaves that line
# commented out, so co-op is off without an error in the log.
function Write-CoopIni([string]$Template, [string]$Path, [string]$Server, [string]$Port, [string]$Name) {
    $out = foreach ($line in [System.IO.File]::ReadAllLines($Template)) {
        foreach ($pair in @(@('@SERVER@', $Server), @('@PORT@', $Port), @('@NAME@', $Name))) {
            if ($line.Contains($pair[0])) {
                $line = if ($pair[1]) { $line.Replace($pair[0], $pair[1]) } else { '# ' + $line.Replace($pair[0], '').TrimEnd() }
            }
        }
        $line
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    # No byte order mark: the emulator reads the first line as it is.
    [System.IO.File]::WriteAllText($Path, (($out -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
}
