# Hosting and joining helpers, shared by start-server.ps1 (the "give your friends" block),
# play.ps1 (Join.bat's address check and its connection watch) and check-hosting.ps1 (the
# host's self-check). Dot-source it. Nothing here opens a port, changes the firewall or the
# router: it only reads, and prints commands for the player to run.
# The functions that decide take their input as arguments (addresses, rules, log text), so
# tools\release\tests\hosting_test.ps1 checks them without touching this PC's network.

# What kind of address this is: 'loopback', 'link-local', 'hamachi' (25.x), 'radmin' (26.x),
# 'tailscale' (100.64.0.0/10, also CGNAT), 'lan' (10.x, 172.16-31.x, 192.168.x), 'public',
# or '' when it is not a usable IPv4 address.
function Get-AddressKind([string]$Address) {
    if ($Address -notmatch '^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$') { return '' }
    $o = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
    foreach ($x in $o) { if ($x -gt 255) { return '' } }
    if ($o[0] -eq 127) { return 'loopback' }
    if ($o[0] -eq 169 -and $o[1] -eq 254) { return 'link-local' }
    if ($o[0] -eq 25) { return 'hamachi' }
    if ($o[0] -eq 26) { return 'radmin' }
    if ($o[0] -eq 100 -and $o[1] -ge 64 -and $o[1] -le 127) { return 'tailscale' }
    if ($o[0] -eq 10 -or ($o[0] -eq 172 -and $o[1] -ge 16 -and $o[1] -le 31) -or ($o[0] -eq 192 -and $o[1] -eq 168)) { return 'lan' }
    if ($o[0] -eq 0 -or $o[0] -ge 224) { return '' }
    return 'public'
}

# The VPN program an address kind belongs to, for the labels.
function Get-VpnName([string]$Kind) {
    switch ($Kind) {
        'hamachi' { 'Hamachi' }
        'radmin' { 'Radmin VPN' }
        'tailscale' { 'Tailscale' }
        default { '' }
    }
}

# This PC's IPv4 addresses as objects with Address and Alias (the adapter's name).
function Get-LocalAddresses {
    try {
        @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop | ForEach-Object {
            [pscustomobject]@{ Address = $_.IPAddress; Alias = $_.InterfaceAlias }
        })
    } catch { @() }
}

# This PC's address as the internet sees it, from one fixed, well-known service over HTTPS,
# or '' when the lookup fails or takes longer than $TimeoutMs (a slow name lookup included).
# coop-options.ini's public_address_lookup = no turns it off (start-server.ps1).
$script:PublicAddressService = 'https://api.ipify.org'
function Get-PublicAddress([int]$TimeoutMs = 4000) {
    try {
        Add-Type -AssemblyName System.Net.Http -ErrorAction Stop
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $client = [System.Net.Http.HttpClient]::new()
        try {
        $client.Timeout = [TimeSpan]::FromMilliseconds($TimeoutMs)
        $task = $client.GetStringAsync($script:PublicAddressService)
        if (-not $task.Wait($TimeoutMs)) { return '' }
        $text = $task.Result.Trim()
        if ((Get-AddressKind $text) -eq 'public') { return $text }
        } finally { $client.Dispose() }
    } catch { }
    return ''
}

# The addresses to give the others, best first: VPN, then internet, then home network. A list
# of objects with Address, Kind and Label (when to use it). $Local: Get-LocalAddresses;
# $Public: Get-PublicAddress ('' = none); $Bind: coop-options.ini's server_bind (then only
# that address works).
function Get-JoinAddresses($Local, [string]$Public, [string]$Bind = '') {
    $vpn = @(); $lan = @()
    foreach ($a in @($Local)) {
        if (-not $a) { continue }
        $kind = Get-AddressKind $a.Address
        if ($Bind -and $a.Address -ne $Bind) { continue }
        if ($kind -in @('hamachi', 'radmin', 'tailscale')) {
            $name = Get-VpnName $kind
            $vpn += [pscustomobject]@{ Address = $a.Address; Kind = $kind; Label = "Over $name (friends in your $name network)" }
        }
        elseif ($kind -eq 'lan') {
            $lan += [pscustomobject]@{ Address = $a.Address; Kind = 'lan'; Label = "Same house (friends on your home network or Wi-Fi)" }
        }
    }
    $out = @($vpn)
    if ($Public -and -not $Bind) {
        $out += [pscustomobject]@{ Address = $Public; Kind = 'public'; Label = 'Over the internet, ONLY if you forwarded the port on your router' }
    }
    return @($out + $lan)
}

# An address as the others type it: with :port only when the port is not the usual one.
function Format-JoinAddress([string]$Address, [int]$Port) {
    if ($Port -ne 27510) { return "${Address}:$Port" }
    return $Address
}

# The best address to copy: the first one Get-JoinAddresses gave, or ''.
function Get-BestJoinAddress($Addresses, [int]$Port) {
    $first = @($Addresses | Where-Object { $_ }) | Select-Object -First 1
    if (-not $first) { return '' }
    return Format-JoinAddress $first.Address $Port
}

# The block the server window prints when it starts: who gets which address, and which one
# was copied. $Addresses: Get-JoinAddresses. $PublicFailed: the public lookup was tried and
# failed. $Allow: server_allow ('' = anyone may join). $Copied: the address on the clipboard.
function Format-JoinBlock($Addresses, [int]$Port, [bool]$PublicFailed, [string]$Allow, [string]$Copied, [string]$Bind = '') {
    $list = @($Addresses | Where-Object { $_ })
    $lines = @('', '=====================================================================')
    $lines += 'Give your friends ONE of these (they type it in Join.bat):'
    $lines += ''
    foreach ($a in $list) { $lines += ('  {0,-21} {1}' -f (Format-JoinAddress $a.Address $Port), $a.Label) }
    if (-not $list.Count) { $lines += '  (no network address found: start your VPN program, or run ipconfig)' }
    if ($PublicFailed) {
        $lines += '  (internet address: not found. Open https://api.ipify.org in a browser to see it;'
        $lines += '   it only works if you forwarded the port on your router)'
    }
    # A server with server_bind hears only that address, not 127.0.0.1.
    $self = if ($Bind) { $Bind } else { '127.0.0.1' }
    $lines += ('  {0,-21} {1}' -f (Format-JoinAddress $self $Port), 'A second game on this same PC')
    $lines += ''
    if ($Copied) { $lines += "Copied to the clipboard: $Copied   (paste it to your friends with Ctrl+V)" }
    if (-not @($list | Where-Object { $_.Kind -in @('hamachi', 'radmin', 'tailscale') }).Count) {
        $lines += 'Friends outside your house: easiest and safest is one Radmin VPN or Hamachi network'
        $lines += 'with a password for all of you (docs\play-online.md).'
    }
    if ($Allow) { $lines += "Only these addresses may join (server_allow): $Allow" }
    elseif (@($list | Where-Object { $_.Kind -eq 'public' }).Count) {
        $lines += 'Port forwarding? Put your friends'' addresses in server_allow in coop-options.ini.'
    }
    $lines += 'Friends cannot connect? Run check-hosting.cmd while this window is open.'
    $lines += '====================================================================='
    $lines += ''
    return $lines
}

# What a player pasted for the host's address, made plain: spaces, quotes, a trailing dot,
# 'udp://' and a trailing '/' are dropped. Returns @{ Server; Port } (Port '' when none was
# given) or $null when it is not an address. 'localhost' becomes 127.0.0.1.
function ConvertTo-JoinAddress([string]$Text) {
    if ($null -eq $Text) { return $null }
    $t = ($Text -replace '[\s"''<>]', '')
    $t = $t -replace '^(?i)(udp|http|https)://', ''
    $t = $t.TrimEnd('/', '.', ',', ';')
    if ($t -notmatch '^([A-Za-z0-9._-]{1,253})(:(\d{1,5}))?$') { return $null }
    $server = $Matches[1].TrimEnd('.'); $port = $Matches[3]
    if (-not $server) { return $null }
    if ($port -and ([int]$port -lt 1 -or [int]$port -gt 65535)) { return $null }
    if ($server -ieq 'localhost') { $server = '127.0.0.1' }
    # Digits and dots only: it must be a whole IPv4 address (a typo such as 26.1.2 is caught).
    if ($server -match '^[\d.]+$' -and -not (Get-AddressKind $server)) { return $null }
    return @{ Server = $server; Port = "$port" }
}

# The three most likely reasons a join does not connect, in plain words.
function Get-JoinFailureHelp([string]$Server) {
    $kind = Get-AddressKind ($Server -replace ':\d+$', '')
    $lines = @('Not connected yet. The three most likely reasons:')
    $lines += '  1. Wrong address. Ask the host to read it again from their server window'
    $lines += "     (the 'Give your friends ONE of these' block). You typed: $Server"
    $lines += '  2. The host''s firewall. The host runs check-hosting.cmd in their kit and does what it says.'
    if ($kind -in @('hamachi', 'radmin', 'tailscale')) {
        $lines += "  3. The VPN: both of you must be online (green) in the same $(Get-VpnName $kind) network."
    }
    elseif ($kind -eq 'public') {
        $lines += '  3. The host''s router does not forward UDP port 27510 to their PC. Easier: all of you'
        $lines += '     join one Radmin VPN or Hamachi network and use the VPN address (docs\play-online.md).'
    }
    else {
        $lines += '  3. Not the same network: this address only works inside the host''s house. Over the'
        $lines += '     internet use a Radmin VPN or Hamachi network (docs\play-online.md).'
    }
    $lines += 'Also: is the host''s server window still open? Does everyone have the same release (Update.bat)?'
    return $lines
}

# How far a join got, from the emulator's log text: 'welcomed', 'refused: <why>', 'trying'
# (co-op started, no answer yet) or '' (co-op has not started yet).
function Get-JoinState([string]$LogText) {
    if (-not $LogText) { return '' }
    $state = ''
    foreach ($line in ($LogText -split "`n")) {
        if ($line -match 'coop: on, server ') { $state = 'trying' }
        elseif ($line -match 'coop: welcomed as player') { $state = 'welcomed' }
        elseif ($line -match 'coop: server (refused: .*?)\s*$') { $state = $Matches[1] }
    }
    return $state
}

# --- The host's self-check (check-hosting.ps1) ---

# The firewall rules that matter for this kit's server, as objects with Name, Enabled,
# Inbound, Allow, Program ('' = any program), Protocol, LocalPort, RemoteAddress, Profile:
# every rule for this coop_server.exe, and rules named like the ones in our docs that have
# no program (too wide). $null when the rules cannot be read.
function Get-ServerFirewallRules([string]$ServerExe) {
    $want = [System.IO.Path]::GetFullPath($ServerExe)
    $out = @()
    try {
        $apps = @(Get-NetFirewallApplicationFilter -ErrorAction Stop | Where-Object {
            $p = $_.Program
            if (-not $p -or $p -eq 'Any') { return $false }
            try { [System.IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($p)) -ieq $want } catch { $false }
        })
        foreach ($app in $apps) { $out += ConvertTo-RuleInfo ($app | Get-NetFirewallRule) $app.Program }
        foreach ($r in @(Get-NetFirewallRule -DisplayName 'OoT co-op*' -ErrorAction SilentlyContinue)) {
            $app = $r | Get-NetFirewallApplicationFilter
            if (-not $app.Program -or $app.Program -eq 'Any') { $out += ConvertTo-RuleInfo $r '' }
        }
    } catch { return $null }
    return ,@($out)  # the comma keeps an empty list a list (not $null, which means unreadable)
}

function ConvertTo-RuleInfo($Rule, [string]$Program) {
    $port = $Rule | Get-NetFirewallPortFilter
    $addr = $Rule | Get-NetFirewallAddressFilter
    [pscustomobject]@{
        Name = $Rule.DisplayName
        Enabled = "$($Rule.Enabled)" -eq 'True'
        Inbound = "$($Rule.Direction)" -eq 'Inbound'
        Allow = "$($Rule.Action)" -eq 'Allow'
        Program = $Program
        Protocol = "$($port.Protocol)"
        LocalPort = (@($port.LocalPort) -join ',')
        RemoteAddress = (@($addr.RemoteAddress) -join ',')
        Profile = "$($Rule.Profile)"
    }
}

# Does a rule cover UDP on $Port? Any or empty counts.
function Test-RuleCoversPort($Rule, [int]$Port) {
    $protoOk = "$($Rule.Protocol)" -in @('', 'Any', 'UDP', '17')
    $portOk = $false
    foreach ($p in @("$($Rule.LocalPort)" -split ',' | ForEach-Object { $_.Trim() })) {
        if (-not $p -or $p -eq 'Any' -or $p -eq "$Port") { $portOk = $true }
        elseif ($p -match '^(\d+)-(\d+)$' -and $Port -ge [int]$Matches[1] -and $Port -le [int]$Matches[2]) { $portOk = $true }
    }
    return ($protoOk -and $portOk)
}

# The rule this kit suggests for one kind of network: only coop_server.exe, only UDP on the
# port, only from that network's addresses. VPN adapters are Public networks to Windows, so
# their rules use profile=any, kept narrow by program= and remoteip=.
function Format-AddRuleCommand([string]$ServerExe, [int]$Port, [string]$Kind, [string]$Allow) {
    $scope = switch ($Kind) {
        'hamachi' { @{ Remote = '25.0.0.0/8'; Profile = 'any' } }
        'radmin' { @{ Remote = '26.0.0.0/8'; Profile = 'any' } }
        'tailscale' { @{ Remote = '100.64.0.0/10'; Profile = 'any' } }
        'public' {
            # Only from server_allow, checked as play.ps1 does; no line opens the port to every address.
            if (-not $Allow -or $Allow -notmatch '^[0-9./, ]+$') {
                return $(if ($Allow) { "server_allow '$Allow' in coop-options.ini is not a list of addresses (25.1.2.3, 26.0.0.0/8): fix it first" } else { 'Set server_allow in coop-options.ini to your friends'' addresses first; then this check prints the line to run.' })
            }
            @{ Remote = ($Allow -replace '\s', ''); Profile = 'private' }
        }
        default { @{ Remote = 'localsubnet'; Profile = 'private' } }
    }
    $cmd = "netsh advfirewall firewall add rule name=`"OoT co-op server UDP $Port`" dir=in action=allow program=`"$ServerExe`" protocol=UDP localport=$Port profile=$($scope.Profile)"
    if ($scope.Remote) { $cmd += " remoteip=$($scope.Remote)" }
    return $cmd
}

# The check itself, from facts gathered by check-hosting.ps1 (or a test):
#   $Rules: Get-ServerFirewallRules ($null = could not be read)
#   $Listen: objects with LocalAddress, one per UDP socket this coop_server.exe has on the port
#            (none = not running)
#   $Local: Get-LocalAddresses; $Profiles: objects with Alias and Category (Public, Private,
#            DomainAuthenticated)
#   $Access: Read-KitServerAccess (Allow, Bind)
# Returns objects with Level ('ok', 'problem', 'note') and Lines (text; commands indented).
function Get-HostingFindings([string]$ServerExe, [int]$Port, $Rules, $Listen, $Local, $Profiles, $Access) {
    $found = [System.Collections.ArrayList]::new()
    function Add-Finding([string]$Level, [string[]]$Lines) { [void]$found.Add([pscustomobject]@{ Level = $Level; Lines = $Lines }) }

    # The networks the others come from: the VPNs this PC is on, and the home network.
    $vpnKinds = @()
    foreach ($a in @($Local | Where-Object { $_ })) {
        $k = Get-AddressKind $a.Address
        if ($k -in @('hamachi', 'radmin', 'tailscale') -and $k -notin $vpnKinds) { $vpnKinds += $k }
    }

    # 1. The server: running, and on which addresses.
    $listen = @($Listen | Where-Object { $_ })
    if (-not $listen.Count) {
        Add-Finding 'problem' @("The server is not running on UDP port $Port.",
            '  Start Host.bat first, keep the server window open, then run check-hosting.cmd again.')
    }
    else {
        $where = @($listen | ForEach-Object { "$($_.LocalAddress)" } | Sort-Object -Unique)
        if ($where -contains '0.0.0.0') {
            $names = @($Local | Where-Object { $_ -and (Get-AddressKind $_.Address) -notin @('loopback', 'link-local', '') } |
                ForEach-Object { "    $($_.Address)  ($($_.Alias))" })
            Add-Finding 'ok' (@("The server is running on UDP port $Port, on every network of this PC:") + $names)
        }
        else { Add-Finding 'ok' @("The server is running on UDP port $Port, only on $($where -join ', ') (server_bind).") }
    }

    # 2. Who may join.
    if ($Access.Allow) { Add-Finding 'ok' @("Only these addresses may join (server_allow): $($Access.Allow)") }
    else {
        Add-Finding 'note' @('Anyone who can reach the port may join (server_allow is not set).',
            '  To let in only your friends: in coop-options.ini remove the # in front of server_allow, put in',
            '  their addresses (their VPN addresses, or their public ones with port forwarding), restart Host.bat.')
    }

    # 3. The firewall.
    if ($null -eq $Rules) {
        Add-Finding 'note' @('Could not read the Windows Firewall rules (another firewall program, or no permission).',
            "  Make sure your firewall lets coop_server.exe receive UDP on port $Port.")
    }
    else {
        $rules = @($Rules | Where-Object { $_ -and $_.Enabled -and $_.Inbound })
        $blocks = @($rules | Where-Object { -not $_.Allow -and $_.Program })
        $allows = @($rules | Where-Object { $_.Allow -and $_.Program -and (Test-RuleCoversPort $_ $Port) })
        $wide = @($rules | Where-Object { $_.Allow -and -not $_.Program })
        if ($blocks.Count) {
            Add-Finding 'problem' @("The firewall BLOCKS the server: $($blocks.Count) rule(s), often from clicking Cancel when Windows asked.",
                '  Fix: open Command Prompt as administrator and run this (it removes every rule for this',
                '  coop_server.exe, then add the allow rule below):',
                "    netsh advfirewall firewall delete rule name=all dir=in program=`"$ServerExe`"")
        }
        foreach ($w in $wide) {
            Add-Finding 'problem' @("The rule '$($w.Name)' lets in any program on that port, not only the co-op server.",
                '  Fix: Command Prompt as administrator:',
                "    netsh advfirewall firewall delete rule name=`"$($w.Name)`"",
                '  then add the rule below, which allows only coop_server.exe.')
        }
        if (-not $allows.Count -or $blocks.Count -or $wide.Count) {
            $need = @($vpnKinds) + @('lan')
            $cmds = @($(if (-not $allows.Count) { "No firewall rule lets other PCs reach this kit's coop_server.exe." } else { 'Add a rule that allows only this kit''s coop_server.exe:' }))
            $cmds += '  Fix: open Command Prompt as administrator and run the line for the way you play:'
            foreach ($k in $need) {
                $label = if ($k -eq 'lan') { 'Same house (home network):' } else { "Over $(Get-VpnName $k):" }
                $cmds += "    $label"
                $cmds += '      ' + (Format-AddRuleCommand $ServerExe $Port $k $Access.Allow)
            }
            $cmds += '    Over the internet with port forwarding (no VPN):'
            $cmds += '      ' + (Format-AddRuleCommand $ServerExe $Port 'public' $Access.Allow)
            Add-Finding 'problem' $cmds
        }
        else {
            foreach ($r in $allows) {
                $remote = if ($r.RemoteAddress -and $r.RemoteAddress -ne 'Any') { $r.RemoteAddress } else { 'any address' }
                Add-Finding 'ok' @("Firewall rule '$($r.Name)' allows only coop_server.exe, UDP, from $remote, on $($r.Profile) networks.")
            }
            # Windows calls a VPN adapter a Public network; a rule for Private only does not cover it.
            foreach ($k in $vpnKinds) {
                $alias = @($Local | Where-Object { $_ -and (Get-AddressKind $_.Address) -eq $k } | ForEach-Object { $_.Alias }) | Select-Object -First 1
                $cat = "$(@($Profiles | Where-Object { $_ -and $_.Alias -eq $alias } | ForEach-Object { $_.Category }) | Select-Object -First 1)"
                if ($cat -ne 'Public') { continue }
                if (-not @($allows | Where-Object { $_.Profile -match 'Any|Public' }).Count) {
                    Add-Finding 'problem' @("Windows calls your $(Get-VpnName $k) adapter a Public network, and the rule above covers only $($allows[0].Profile) networks.",
                        '  Fix: Command Prompt as administrator:',
                        '    ' + (Format-AddRuleCommand $ServerExe $Port $k $Access.Allow))
                }
            }
            # The home network called Public: the rule for Private networks does not apply there.
            # The fix is to call it Private, not to widen the rule.
            foreach ($a in @($Local | Where-Object { $_ -and (Get-AddressKind $_.Address) -eq 'lan' })) {
                $cat = "$(@($Profiles | Where-Object { $_ -and $_.Alias -eq $a.Alias } | ForEach-Object { $_.Category }) | Select-Object -First 1)"
                if ($cat -eq 'Public' -and -not @($allows | Where-Object { $_.Profile -match 'Any|Public' }).Count) {
                    Add-Finding 'problem' @("Windows calls your home network '$($a.Alias)' Public, so the rule for Private networks does not apply there.",
                        "  Fix: Settings > Network & internet > $($a.Alias) > Network profile type: Private network.",
                        '  (Only at home. Do not widen the rule to Public networks instead.)')
                }
            }
            if (@($allows | Where-Object { (-not $_.RemoteAddress -or $_.RemoteAddress -eq 'Any') -and $_.Profile -match 'Any|Public' }).Count) {
                Add-Finding 'note' @('A rule lets anyone on any Public network (a cafe or hotel Wi-Fi too) reach the server.',
                    '  Safer: limit it to your VPN''s addresses (remoteip=25.0.0.0/8 Hamachi, 26.0.0.0/8 Radmin VPN).')
            }
        }
    }
    if (-not $vpnKinds.Count) {
        Add-Finding 'note' @('No Hamachi, Radmin VPN or Tailscale network on this PC. Friends outside your house need a VPN',
            'network (easiest and safest) or a port forward on your router: docs\play-online.md.')
    }
    return @($found)
}
