# Joining through a relay with a join code (docs/play-online.md, "Relay with join codes").
# The host's server window shows "ABCD-EFGH@relay.example.net:27530"; Join.bat takes that as
# the address, sends the code to the relay once (that lets this PC's address in to that host
# for 30 minutes after its last packet) and points the game at the relay.
# Dot-sourced by play.ps1; tools/release/tests/hosting_test.ps1 tests the parts without a network.

$script:RelayCodeLetters = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$script:RelayDefaultPort = 27530

# 'abcd-efgh @ relay.example.net:27530' -> @{ Code = 'ABCDEFGH'; Relay = 'relay.example.net';
# Port = 27530 }, or $null when it is not a join code with a relay.
function ConvertTo-RelayJoin([string]$Text) {
    if ($null -eq $Text) { return $null }
    $t = ($Text -replace '[\s"''<>]', '').TrimEnd('/', '.', ',', ';')
    if ($t -notmatch '^([A-Za-z0-9-]{8,9})@([A-Za-z0-9._-]{1,253})(:(\d{1,5}))?$') { return $null }
    $code = ($Matches[1] -replace '-', '').ToUpperInvariant()
    $relay = $Matches[2].TrimEnd('.')
    $port = if ($Matches[4]) { [int]$Matches[4] } else { $script:RelayDefaultPort }
    if ($code.Length -ne 8 -or $port -lt 1 -or $port -gt 65535 -or -not $relay) { return $null }
    foreach ($c in $code.ToCharArray()) { if ($script:RelayCodeLetters.IndexOf($c) -lt 0) { return $null } }
    return @{ Code = $code; Relay = $relay; Port = $port }
}

# 'ABCDEFGH' -> 'ABCD-EFGH@relay:port', the way the host's window shows it.
function Format-RelayJoin($Join) {
    return '{0}-{1}@{2}:{3}' -f $Join.Code.Substring(0, 4), $Join.Code.Substring(4), $Join.Relay, $Join.Port
}

# The relay's lookup datagram for a code (server/relay_wire.hpp: magic 'OCR' 1, kind 0x10, code).
function Get-RelayLookupBytes([string]$Code) {
    return [byte[]](@(0x4F, 0x43, 0x52, 0x01, 0x10) + [System.Text.Encoding]::ASCII.GetBytes($Code))
}

# The relay's answer: 'ok', 'unknown', 'slow-down', 'full', or $null when it is not one.
function ConvertFrom-RelayFound([byte[]]$Bytes) {
    if (-not $Bytes -or $Bytes.Length -ne 6) { return $null }
    if ($Bytes[0] -ne 0x4F -or $Bytes[1] -ne 0x43 -or $Bytes[2] -ne 0x52 -or $Bytes[3] -ne 0x01 -or $Bytes[4] -ne 0x90) { return $null }
    switch ($Bytes[5]) { 0 { return 'ok' } 1 { return 'unknown' } 2 { return 'slow-down' } 3 { return 'full' } }
    return $null
}

# The relay's IPv4 address (so the lookup and the game go to the same one), or ''.
function Resolve-RelayAddress([string]$Relay) {
    if ($Relay -match '^\d{1,3}(\.\d{1,3}){3}$') { return $Relay }
    try {
        foreach ($a in [System.Net.Dns]::GetHostAddresses($Relay)) {
            if ($a.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork) { return $a.ToString() }
        }
    } catch { }
    return ''
}

# Sends the code (three tries, a second each). 'ok', 'unknown', 'slow-down', 'full' or 'no-answer'.
function Invoke-RelayLookup([string]$Address, [int]$Port, [string]$Code) {
    $udp = $null
    try {
        $udp = New-Object System.Net.Sockets.UdpClient
        $udp.Client.ReceiveTimeout = 1000
        $bytes = Get-RelayLookupBytes $Code
        for ($i = 0; $i -lt 3; $i++) {
            [void]$udp.Send($bytes, $bytes.Length, $Address, $Port)
            try {
                $from = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Any, 0)
                $answer = ConvertFrom-RelayFound ($udp.Receive([ref]$from))
                if ($answer) { return $answer }
            } catch { }
        }
    } catch { }
    finally { if ($udp) { $udp.Close() } }
    return 'no-answer'
}

# What to tell the player for a lookup result.
function Get-RelayLookupHelp([string]$Result, $Join) {
    switch ($Result) {
        'ok' { return @("Join code accepted by the relay $($Join.Relay). The game connects through it.") }
        'unknown' { return @('The relay does not know this join code. Check it with the host: the code changes every time',
                'the host starts the server, and ends when the host''s server window closes.') }
        'slow-down' { return @('The relay had too many wrong codes from your address: wait 10 minutes, then try again.') }
        'full' { return @('That host already has the most players the relay lets in.') }
    }
    return @("No answer from the relay $($Join.Relay) (port $($Join.Port)). Is the address right? Is the relay running?",
        'Ask the host; their server window says whether it reaches the relay.')
}

# The last join through a relay, so Join.bat can offer it again (Enter sends the code again):
# relay-join.txt in the kit folder, two lines: the join text and the relay's address.
function Save-RelayJoin([string]$Kit, $Join, [string]$Address) {
    try { [System.IO.File]::WriteAllLines((Join-Path $Kit 'relay-join.txt'), [string[]]@((Format-RelayJoin $Join), $Address)) } catch { }
}
function Read-RelayJoin([string]$Kit) {
    $path = Join-Path $Kit 'relay-join.txt'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $lines = @([System.IO.File]::ReadAllLines($path))
    if ($lines.Count -lt 2 -or -not (ConvertTo-RelayJoin $lines[0])) { return $null }
    return @{ Text = $lines[0]; Address = $lines[1].Trim() }
}

# Sends the code to the relay and says what happened. The relay's address when it took the
# code, '' when not.
function Connect-RelayJoin($Join) {
    $address = Resolve-RelayAddress $Join.Relay
    if (-not $address) {
        Write-Host "Cannot find the relay '$($Join.Relay)'. Is the name right, and is this PC online?"
        return ''
    }
    $result = Invoke-RelayLookup $address $Join.Port $Join.Code
    foreach ($l in (Get-RelayLookupHelp $result $Join)) { Write-Host $l }
    if ($result -eq 'ok') { return $address }
    return ''
}

# The host's side: coop_server arguments and the lines to show for coop-options.ini's upnp,
# relay and relay_key. @{ Args = @(...); Notes = @(...) }. The relay key goes in the
# environment variable OOT_COOP_RELAY_KEY (start-server.ps1 sets it), never on the command line.
function Get-ServerHostingArgs($Access, [string]$Password, [string]$Allow) {
    $list = @()
    $notes = @()
    if ($Access.Upnp) {
        if (-not $Password -and -not $Allow) {
            $notes += 'upnp = yes is NOT used: it opens your PC to the whole internet while the server runs, so it'
            $notes += 'needs a join password (server_password) or server_allow in coop-options.ini as well.'
        }
        else {
            $list += '--upnp'
            $notes += 'UPnP (upnp = yes): asking your router to forward the port while the server runs. The'
            $notes += 'server''s lines below say whether the router did it; the forward ends when this window closes.'
        }
    }
    if ($Access.Relay) {
        if ($Access.Relay -notmatch '^[A-Za-z0-9._-]{1,253}(:\d{1,5})?$') {
            $notes += "relay = '$($Access.Relay)' is not a relay address (a name or IPv4 address, optional :port); not used."
        }
        elseif (-not $Password) {
            # server_allow is not enough: every guest through the relay comes from 127.0.0.1.
            $notes += 'relay = ... is NOT used: the join code only finds your server, the join password keeps'
            $notes += 'strangers out, so set server_password in coop-options.ini as well (server_allow is not enough).'
        }
        else {
            $list += @('--relay-via', $Access.Relay)
            if ($Access.RelayKey) { $list += @('--relay-key-env', 'OOT_COOP_RELAY_KEY') }
            $notes += "Relay (relay = $($Access.Relay)): your join code shows below in a few seconds and is copied to"
            $notes += 'the clipboard. Your friends paste all of it (CODE@relay) into Join.bat; the code changes each time.'
        }
    }
    return @{ Args = $list; Notes = $notes }
}
