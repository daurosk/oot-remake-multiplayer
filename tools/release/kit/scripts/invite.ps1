# Invite codes (docs/play-online.md, "Host.bat and an invite code"): the host's server makes
# them (server/nat_codes.cpp), Join.bat reads them here. Same letters and bytes: version and
# kind, the host's internet address and port, the join password, a 4-byte SHA-256 checksum.
# tools/release/tests/hosting_test.ps1 checks this reader against the server's own vectors.

$script:InviteLetters = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'

# A join password for this session: 20 letters, 100 random bits from the system's
# cryptographic generator (256 is a multiple of 32, so every letter is as likely).
function New-JoinPassword {
    $bytes = New-Object byte[] 20
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return -join ($bytes | ForEach-Object { $script:InviteLetters[$_ -band 31] })
}

# What was pasted as bare letters (upper case, no spaces, dashes or dots), or $null.
function Get-InviteLetters([string]$Text) {
    if (-not $Text) { return $null }
    $t = ($Text -replace '[\s\-\.''"]', '').ToUpperInvariant()
    if (-not $t -or $t -notmatch '^[A-HJ-NP-Z2-9]+$') { return $null }
    return $t
}

# An invite (or answer) code rather than an address or a relay join code.
function Test-InviteCodeText([string]$Text) {
    $l = Get-InviteLetters $Text
    return [bool]($l -and $l.Length -ge 30)
}

function Test-InvitePublic([byte[]]$Ip) {
    $a = $Ip[0]; $b = $Ip[1]
    if ($a -eq 0 -or $a -eq 10 -or $a -eq 127 -or $a -ge 224) { return $false }
    if ($a -eq 100 -and ($b -band 0xC0) -eq 64) { return $false }
    if ($a -eq 169 -and $b -eq 254) { return $false }
    if ($a -eq 172 -and ($b -band 0xF0) -eq 16) { return $false }
    if ($a -eq 192 -and $b -eq 168) { return $false }
    if ($a -eq 192 -and $b -eq 0 -and $Ip[2] -eq 0) { return $false }
    if ($a -eq 198 -and ($b -band 0xFE) -eq 18) { return $false }
    return $true
}

# @{ Ok = $true; Address = '203.0.113.5'; Port = 27510; Password = '...' } or
# @{ Ok = $false; Error = 'one line for the player' }.
function ConvertFrom-InviteCode([string]$Text) {
    $fail = { param($why) @{ Ok = $false; Error = $why } }
    $letters = Get-InviteLetters $Text
    if (-not $letters -or $letters.Length -lt 30) { return & $fail 'this is not an invite code (letters A-Z and 2-9 in groups of five; paste all of it)' }
    $n = [int][math]::Floor($letters.Length * 5 / 8)
    if ([math]::Floor(($n * 8 + 4) / 5) -ne $letters.Length) { return & $fail 'a letter of the invite code is wrong or missing; ask the host to copy it again' }
    $bits = New-Object System.Collections.Generic.List[int]
    foreach ($c in $letters.ToCharArray()) {
        $v = $script:InviteLetters.IndexOf($c)
        for ($i = 4; $i -ge 0; $i--) { $bits.Add(($v -shr $i) -band 1) }
    }
    for ($i = $n * 8; $i -lt $bits.Count; $i++) {
        if ($bits[$i]) { return & $fail 'a letter of the invite code is wrong or missing; ask the host to copy it again' }
    }
    $bytes = New-Object byte[] $n
    for ($i = 0; $i -lt $n; $i++) {
        $v = 0
        for ($j = 0; $j -lt 8; $j++) { $v = ($v -shl 1) -bor $bits[$i * 8 + $j] }
        $bytes[$i] = [byte]$v
    }
    if ($n -lt 12) { return & $fail 'a letter of the invite code is wrong or missing; ask the host to copy it again' }
    $body = $n - 4
    if (($bytes[0] -shr 4) -eq 2) { return & $fail 'this is an ANSWER code: it goes into the host''s server window, not into Join.bat' }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash($bytes, 0, $body) } finally { $sha.Dispose() }
    for ($i = 0; $i -lt 4; $i++) {
        if ($digest[$i] -ne $bytes[$body + $i]) { return & $fail 'a letter of the invite code is wrong or missing; ask the host to copy it again' }
    }
    $kind = $bytes[0]
    if ($kind -ne 0x11 -and $kind -ne 0x12) { return & $fail 'the invite code is from a newer co-op version; everyone runs Update.bat' }
    $bad = 'the invite code does not make sense; ask the host for a new one'
    $ip = [byte[]]@($bytes[1], $bytes[2], $bytes[3], $bytes[4])
    $port = [int]$bytes[5] * 256 + [int]$bytes[6]
    if ($kind -eq 0x11) {
        if ($body -ne 20 -or ($bytes[19] -band 0x0F)) { return & $fail $bad }
        $password = ''
        for ($k = 0; $k -lt 20; $k++) {
            $v = 0
            for ($j = 0; $j -lt 5; $j++) { $v = ($v -shl 1) -bor $bits[56 + $k * 5 + $j] }
            $password += $script:InviteLetters[$v]
        }
    }
    else {
        $length = [int]$bytes[7]
        if ($body -ne 8 + $length -or $length -lt 1) { return & $fail $bad }
        $password = [System.Text.Encoding]::UTF8.GetString($bytes, 8, $length)
    }
    if ($port -lt 1 -or $password -match '\s') { return & $fail $bad }
    if (-not (Test-InvitePublic $ip)) { return & $fail 'the address in the invite code is not an internet address; ask the host for a new one' }
    return @{ Ok = $true; Address = ($ip -join '.'); Port = $port; Password = $password }
}
