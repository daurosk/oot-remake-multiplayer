# Joining through a relay with a join code (docs/play-online.md, "Relay with join codes").
# The host's server window shows "ABCD-EFGH@relay.example.net:27530"; Join.bat takes that as
# the address, sends the code to the relay once (that lets this PC's address in to that host
# for 30 minutes after its last packet) and points the game at the relay.
# Dot-sourced by play.ps1; tools/release/tests/hosting_test.ps1 tests the parts without a network.

$script:RelayCodeLetters = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$script:RelayDefaultPort = 27530
$script:RelayRoomPattern = '^[A-Za-z0-9_-]{3,24}$'

# 'abcd-efgh @ relay.example.net:27530' -> @{ Code = 'ABCDEFGH'; Relay = 'relay.example.net';
# Port = 27530 }, or $null when it is not a join code with a relay.
#
# A room name works the same way (issue 261): 'forest-gang@relay.example.net:27530', when the host
# set relay_room. Then Code is $null and Room is the name. A text that can be both (8 code letters,
# with or without a dash: 'lake-gang') gets both: the code is tried first, then the room.
function ConvertTo-RelayJoin([string]$Text) {
    if ($null -eq $Text) { return $null }
    $t = ($Text -replace '[\s"''<>]', '').TrimEnd('/', '.', ',', ';')
    if ($t -notmatch '^([A-Za-z0-9_-]{3,24})@([A-Za-z0-9._-]{1,253})(:(\d{1,5}))?$') { return $null }
    $what = $Matches[1]
    $relay = $Matches[2].TrimEnd('.')
    $port = if ($Matches[4]) { [int]$Matches[4] } else { $script:RelayDefaultPort }
    if ($port -lt 1 -or $port -gt 65535 -or -not $relay) { return $null }
    $code = ($what -replace '-', '').ToUpperInvariant()
    $isCode = $code.Length -eq 8 -and $what.Length -le 9 -and $what -notmatch '_' -and
        -not ($code.ToCharArray() | Where-Object { $script:RelayCodeLetters.IndexOf($_) -lt 0 })
    # A room name: 3 to 24 letters, digits, - or _ (server/relay_wire.hpp, normalize_room).
    $isRoom = $what -match $script:RelayRoomPattern
    if (-not $isCode -and -not $isRoom) { return $null }
    return @{ Code = $(if ($isCode) { $code } else { $null }); Room = $(if ($isRoom) { $what } else { $null });
        Relay = $relay; Port = $port }
}

# 'ABCDEFGH' -> 'ABCD-EFGH@relay:port', the way the host's window shows it; a room as typed.
function Format-RelayJoin($Join) {
    if ($Join.Code) {
        return '{0}-{1}@{2}:{3}' -f $Join.Code.Substring(0, 4), $Join.Code.Substring(4), $Join.Relay, $Join.Port
    }
    return '{0}@{1}:{2}' -f $Join.Room, $Join.Relay, $Join.Port
}

# The relay's lookup datagram for a code (server/relay_wire.hpp: magic 'OCR' 1, kind 0x10, code).
function Get-RelayLookupBytes([string]$Code) {
    return [byte[]](@(0x4F, 0x43, 0x52, 0x01, 0x10) + [System.Text.Encoding]::ASCII.GetBytes($Code))
}

# The same for a room name (kind 0x11, length, name). A relay from before room names answers nothing.
function Get-RelayRoomLookupBytes([string]$Room) {
    $name = [System.Text.Encoding]::ASCII.GetBytes($Room)
    return [byte[]](@(0x4F, 0x43, 0x52, 0x01, 0x11, $name.Length) + $name)
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
    return Send-RelayLookup $Address $Port (Get-RelayLookupBytes $Code)
}

# The same for a room name.
function Invoke-RelayRoomLookup([string]$Address, [int]$Port, [string]$Room) {
    return Send-RelayLookup $Address $Port (Get-RelayRoomLookupBytes $Room)
}

function Send-RelayLookup([string]$Address, [int]$Port, [byte[]]$Bytes) {
    $udp = $null
    try {
        $udp = New-Object System.Net.Sockets.UdpClient
        $udp.Client.ReceiveTimeout = 1000
        $bytes = $Bytes
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
    $room = -not $Join.Code
    switch ($Result) {
        'ok' {
            if ($room) { return @("Room '$($Join.Room)' found on the relay $($Join.Relay). The game connects through it.") }
            return @("Join code accepted by the relay $($Join.Relay). The game connects through it.")
        }
        'unknown' {
            if ($Join.Code -and $Join.Room) {
                return @("The relay knows no join code or room '$($Join.Room)' right now. Check it with the host: a join code changes",
                    'every time the host starts the server, and a room is free again 30 seconds after the host stops.')
            }
            if ($Join.Room) {
                return @("The relay has no room '$($Join.Room)' right now. Check the name with the host: their server window",
                    'says room "...@relay" is yours while it runs, and a room is free again 30 seconds after the host stops.')
            }
            return @('The relay does not know this join code. Check it with the host: the code changes every time',
                'the host starts the server, and ends when the host''s server window closes.')
        }
        'slow-down' { return @('The relay had too many wrong codes or room names from your address: wait 10 minutes, then try again.') }
        'full' { return @('That host already has the most players the relay lets in.') }
    }
    $lines = @("No answer from the relay $($Join.Relay) (port $($Join.Port)). Is the address right? Is the relay running?",
        'Ask the host; their server window says whether it reaches the relay.')
    if ($room) { $lines += 'A relay from before room names does not answer room names: then ask the host for the join code.' }
    return $lines
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
    $result = 'unknown'
    if ($Join.Code) { $result = Invoke-RelayLookup $address $Join.Port $Join.Code }
    if ($result -eq 'unknown' -and $Join.Room) {
        # Not a code the relay knows (or no code at all): the room name.
        $result = Invoke-RelayRoomLookup $address $Join.Port $Join.Room
        if ($result -eq 'ok') { $Join.Code = $null }
    }
    foreach ($l in (Get-RelayLookupHelp $result $Join)) { Write-Host $l }
    if ($result -eq 'ok') { return $address }
    return ''
}

# The host's side: coop_server arguments and the lines to show for coop-options.ini's upnp,
# invite, stun_servers, relay, relay_key and relay_room. UPnP and the invite code need the join password
# (server_allow is not enough: the password is the lock). @{ Args = @(...); Notes = @(...) }. The relay key goes in the
# environment variable OOT_COOP_RELAY_KEY (start-server.ps1 sets it), never on the command line.
function Get-ServerHostingArgs($Access, [string]$Password, [string]$Allow) {
    $list = @()
    $notes = @()
    if ($Access.Upnp) {
        if (-not $Password) {
            $notes += 'The router forward (UPnP) is NOT used: it opens your PC to the whole internet while the server'
            $notes += 'runs, so it needs the join password, and none was given (Host.bat always makes one).'
        }
        else {
            $list += '--upnp'
            $notes += 'UPnP: asking your router to forward the game''s port while the server runs. The server''s lines'
            $notes += 'below say whether it did; the forward ends when this window closes (upnp = no turns it off).'
        }
    }
    if ($Access.Invite) {
        if (-not $Password) {
            $notes += 'No invite code: it carries the join password, and none was given (Host.bat always makes one).'
        }
        else {
            $list += '--invite'
            foreach ($s in @($Access.Stun -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
                if ($s -match '^[A-Za-z0-9._-]{1,253}(:\d{1,5})?$') { $list += @('--stun', $s) }
                else { $notes += "stun_servers: '$s' is not a name:port; not used." }
            }
            $notes += 'INVITE CODE: shows below in a few seconds and is copied to the clipboard. Share it in DMs only'
            $notes += '(never in a public channel: it holds your internet address and the join password); friends paste it into'
            $notes += 'Join.bat. It stops working when this window closes. If a friend''s Join.bat shows an ANSWER CODE,'
            $notes += 'paste it into this window and press Enter.'
        }
    }
    if ($Access.Relay) {
        if ($Access.Relay -notmatch '^[A-Za-z0-9._-]{1,253}(:\d{1,5})?$') {
            $notes += "relay = '$($Access.Relay)' is not a relay address (a name or IPv4 address, optional :port); not used."
        }
        elseif (-not $Password) {
            # server_allow is not enough: every guest through the relay comes from 127.0.0.1.
            $notes += 'relay = ... is NOT used: the join code only finds your server, the join password keeps'
            $notes += 'strangers out, and none was given (Host.bat always makes one; server_allow is not enough).'
        }
        else {
            $list += @('--relay-via', $Access.Relay)
            if ($Access.RelayKey) { $list += @('--relay-key-env', 'OOT_COOP_RELAY_KEY') }
            $room = "$($Access.RelayRoom)".Trim()
            if ($room -and $room -notmatch $script:RelayRoomPattern) {
                $notes += "relay_room = '$room' is not a room name (3 to 24 letters, digits, - or _); not used."
                $room = ''
            }
            if ($room) {
                $list += @('--relay-room', $room)
                $notes += "Relay (relay = $($Access.Relay)), room '$room': once the relay says the room is yours, your friends"
                $notes += "type $room@$($Access.Relay) into Join.bat (copied to the clipboard). The join code works too."
            }
            else {
                $notes += "Relay (relay = $($Access.Relay)): your join code shows below in a few seconds and is copied to"
                $notes += 'the clipboard. Your friends paste all of it (CODE@relay) into Join.bat; the code changes each time.'
            }
        }
    }
    if ($Access.SameNetworkOpen) {
        $list += '--same-network-open'
        $notes += 'Players on your own network (behind the same router) join without the join password'
        $notes += '(same_network_password = on: they need it too).'
    }
    if ($Access.ApproveJoins) {
        $list += '--approve-joins'
        $notes += 'approve_joins = on: each new player waits until you type /allow or /deny in your game''s chat'
        $notes += '(60 seconds, then they are turned away). Add "always" to remember the answer until the server stops.'
    }
    return @{ Args = $list; Notes = $notes }
}
