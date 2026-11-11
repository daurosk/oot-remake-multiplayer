# Masks what identifies a player in text that collect-logs.ps1 puts into the zip.
# Dot-source it. Each IPv4/IPv6 address becomes its kind with a number, so a reader can
# still see which lines mean the same machine: <lan-1>, <tailscale-1>, <hamachi-1>,
# <radmin-1>, <loopback-1>, <public-1>. Ports stay. MAC addresses become <mac>, the
# Windows user name (in C:\Users\<name> and on its own) becomes <user>, the PC name <pc>,
# and a set password (password = ..., server_password = ...) becomes (removed).
# Works on strings only; tools\release\tests\log_mask_test.ps1 checks it.

# A new state: one per zip, so the same address gets the same tag in every file.
function New-MaskState { @{ Tags = @{}; Counts = @{} } }

# The kind of an IPv4 address, or '' when it should stay (not an address, 0.0.0.0,
# 255.255.255.255).
function Get-MaskKind4([string]$Address) {
    if ($Address -notmatch '^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$') { return '' }
    $o = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
    foreach ($x in $o) { if ($x -gt 255) { return '' } }
    if ($Address -eq '0.0.0.0' -or $Address -eq '255.255.255.255') { return '' }
    if ($o[0] -eq 127) { return 'loopback' }
    if ($o[0] -eq 10 -or ($o[0] -eq 172 -and $o[1] -ge 16 -and $o[1] -le 31) -or
        ($o[0] -eq 192 -and $o[1] -eq 168) -or ($o[0] -eq 169 -and $o[1] -eq 254)) { return 'lan' }
    if ($o[0] -eq 100 -and $o[1] -ge 64 -and $o[1] -le 127) { return 'tailscale' }
    if ($o[0] -eq 25) { return 'hamachi' }
    if ($o[0] -eq 26) { return 'radmin' }
    return 'public'
}

# The kind of an IPv6 address, or '' when it is not one (or is :: alone).
function Get-MaskKind6([string]$Address) {
    $a = $Address -replace '%.*$', ''
    if ($a -notmatch ':' ) { return '' }
    $ip = $null
    if (-not [System.Net.IPAddress]::TryParse($a, [ref]$ip)) { return '' }
    if ($ip.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetworkV6) { return '' }
    $b = $ip.GetAddressBytes()
    if (($b | Measure-Object -Sum).Sum -eq 0) { return '' }
    if ([System.Net.IPAddress]::IsLoopback($ip)) { return 'loopback' }
    if ($b[0] -eq 0xfe -and ($b[1] -band 0xc0) -eq 0x80) { return 'lan' }
    if ($b[0] -eq 0xfd -and $b[1] -eq 0x7a -and $b[2] -eq 0x11 -and $b[3] -eq 0x5c -and $b[4] -eq 0xa1 -and $b[5] -eq 0xe0) { return 'tailscale' }
    if (($b[0] -band 0xfe) -eq 0xfc) { return 'lan' }
    if ($b[0] -eq 0x26 -and $b[1] -eq 0x20 -and $b[2] -eq 0x00 -and $b[3] -eq 0x9b -and
        (($b[4..11] | Measure-Object -Sum).Sum -eq 0)) { return 'hamachi' }
    return 'public'
}

function Get-MaskTag([hashtable]$State, [string]$Kind, [string]$Key) {
    $key = $Key.ToLowerInvariant()
    if (-not $State.Tags.ContainsKey($key)) {
        $n = 1 + [int]$State.Counts[$Kind]
        $State.Counts[$Kind] = $n
        $State.Tags[$key] = "<$Kind-$n>"
    }
    $State.Tags[$key]
}

# The masked text. $UserName and $PcName default to this PC's; tests pass their own.
function Protect-LogText([string]$Text, [hashtable]$State,
                         [string]$UserName = $env:USERNAME, [string]$PcName = $env:COMPUTERNAME) {
    if (-not $Text) { return $Text }
    $t = $Text
    # Passwords and other secrets in ini files (password = ..., server_password = ...):
    # a set value becomes (removed); an empty one stays empty, so a reader sees none was set.
    $t = [regex]::Replace($t, '(?im)^([ \t]*[\w.]*(?:password|secret|token)\w*[ \t]*=)[ \t]*[^\s][^\r\n]*', '$1 (removed)')
    # MAC addresses first (their colon form would otherwise look like IPv6 groups).
    $t = [regex]::Replace($t, '(?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}([:-]))(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:-])', '<mac>')
    # IPv6 (before IPv4, so ::ffff:1.2.3.4 is one address).
    $t = [regex]::Replace($t, '(?<![\w:.])(?:[0-9A-Fa-f]{0,4}:){2,7}(?:[0-9A-Fa-f]{0,4}|\d{1,3}(?:\.\d{1,3}){3})(?:%[\w.-]+)?(?![\w:])', {
        param($m)
        $kind = Get-MaskKind6 $m.Value
        if (-not $kind) { return $m.Value }
        $ip = [System.Net.IPAddress]::Parse(($m.Value -replace '%.*$', ''))
        Get-MaskTag $State $kind $ip.ToString()
    })
    # IPv4: four numbers, not part of a longer dotted number (a version like 1.1.1 or
    # 10.0.26200.0 stays).
    $t = [regex]::Replace($t, '(?<![\d.])\d{1,3}(?:\.\d{1,3}){3}(?![\d]|\.\d)', {
        param($m)
        $kind = Get-MaskKind4 $m.Value
        if (-not $kind) { return $m.Value }
        Get-MaskTag $State $kind $m.Value
    })
    # The user name in paths (C:\Users\name, C:/Users/name, \\?\C:\Users\name).
    $t = [regex]::Replace($t, '(?i)([A-Z]:[\\/]+Users[\\/]+)[^\\/\r\n"''<>|:*?]+', '$1<user>')
    # The user name and the PC name on their own (3 letters or more, whole words only).
    foreach ($p in @(@($UserName, '<user>'), @($PcName, '<pc>'))) {
        if ($p[0] -and $p[0].Length -ge 3) {
            $t = [regex]::Replace($t, '(?i)(?<![\w])' + [regex]::Escape($p[0]) + '(?![\w])', $p[1])
        }
    }
    $t
}
