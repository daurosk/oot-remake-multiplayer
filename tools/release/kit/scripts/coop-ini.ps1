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

# A join password: 1 to 128 characters with no spaces or control characters (spaces would
# meet coop.ini's "  # comment" rule; the emulator and the server allow at most 128 bytes).
function Test-CoopPassword([string]$Value) {
    if (-not $Value -or $Value -match '[\s\x00-\x1f\x7f]') { return $false }
    return ([System.Text.Encoding]::UTF8.GetByteCount($Value) -le 128)
}

# A player colour (coop-options.ini's color, the emulator's LICHII_COOP_COLOR): one of the
# names below (case does not matter) or any #RRGGBB / RRGGBB. The names are the emulator's
# (lichii/hook/coop_colors.cpp); keep the two lists the same.
$CoopColorNames = @('green', 'red', 'blue', 'purple', 'yellow', 'orange', 'cyan', 'pink', 'white', 'black', 'brown', 'gray', 'grey')
function Test-CoopColor([string]$Value) {
    $v = $Value.Trim()
    if ($CoopColorNames -contains $v.ToLowerInvariant()) { return $true }
    return ($v -match '^#?[0-9A-Fa-f]{6}$')
}

# Returns @{ server; port; name; password } from an existing coop.ini (missing keys are empty).
function Read-CoopIni([string]$Path) {
    $values = @{ server = ''; port = ''; name = ''; password = '' }
    if (Test-Path $Path) {
        foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
            if ($line -match '^\s*(server|port|name|password)\s*=\s*(.*?)\s*$') { $values[$Matches[1]] = $Matches[2] }
        }
    }
    return $values
}

# Writes coop.ini from the template. An empty server, name or password leaves that line
# commented out, so co-op is off without an error in the log.
function Write-CoopIni([string]$Template, [string]$Path, [string]$Server, [string]$Port, [string]$Name, [string]$Password = '') {
    $out = foreach ($line in [System.IO.File]::ReadAllLines($Template)) {
        foreach ($pair in @(@('@SERVER@', $Server), @('@PORT@', $Port), @('@NAME@', $Name), @('@PASSWORD@', $Password))) {
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
