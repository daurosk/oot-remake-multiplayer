# Tests for the hosting helpers (tools/release/kit/scripts/hosting.ps1): address kinds, the
# "give your friends" block, what Join.bat accepts, the join watch and the host self-check.
# No network and no firewall: every input is given as data.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\hosting_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\hosting.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Addr([string]$A, [string]$Alias) { [pscustomobject]@{ Address = $A; Alias = $Alias } }

# Address kinds.
$kinds = @{ '25.1.2.3' = 'hamachi'; '26.12.34.56' = 'radmin'; '100.64.0.1' = 'tailscale'; '100.127.255.1' = 'tailscale'
    '100.128.0.1' = 'public'; '192.168.1.20' = 'lan'; '10.0.0.5' = 'lan'; '172.16.0.1' = 'lan'; '172.32.0.1' = 'public'
    '127.0.0.1' = 'loopback'; '169.254.3.4' = 'link-local'; '8.8.8.8' = 'public'; '256.1.1.1' = ''; '26.1.2' = ''; 'abc' = '' }
foreach ($k in $kinds.Keys) { Check ((Get-AddressKind $k) -eq $kinds[$k]) "kind of $k is '$($kinds[$k])'" }

# The addresses to give: VPN first, then internet, then LAN; nothing else.
$local = @((Addr '192.168.1.20' 'Wi-Fi'), (Addr '127.0.0.1' 'Loopback'), (Addr '169.254.9.9' 'Ethernet 2'), (Addr '26.12.34.56' 'Radmin VPN'))
$list = Get-JoinAddresses $local '81.2.3.4'
Check (($list | ForEach-Object { $_.Kind }) -join ',' -eq 'radmin,public,lan') 'order: VPN, internet, home network'
Check ((Get-BestJoinAddress $list 27510) -eq '26.12.34.56') 'the VPN address is copied'
Check ((Get-BestJoinAddress $list 27600) -eq '26.12.34.56:27600') 'another port is added to the copied address'
$list = Get-JoinAddresses @((Addr '192.168.1.20' 'Wi-Fi')) '81.2.3.4'
Check ((Get-BestJoinAddress $list 27510) -eq '81.2.3.4') 'no VPN: the internet address is copied'
$list = Get-JoinAddresses @((Addr '192.168.1.20' 'Wi-Fi')) ''
Check ((Get-BestJoinAddress $list 27510) -eq '192.168.1.20') 'no VPN, no internet address: the LAN address'
$list = @(Get-JoinAddresses $local '81.2.3.4' '26.12.34.56')
Check ($list.Count -eq 1 -and $list[0].Address -eq '26.12.34.56') 'server_bind: only that address is shown'
Check ((Get-BestJoinAddress @() 27510) -eq '') 'no address: nothing copied'

# The block.
$block = (Format-JoinBlock (Get-JoinAddresses $local '81.2.3.4') 27510 $false '' '26.12.34.56') -join "`n"
Check ($block -match 'Give your friends ONE of these') 'block: the heading'
Check ($block -match '26\.12\.34\.56\s+Over Radmin VPN' -and $block -match '81\.2\.3\.4\s+Over the internet, ONLY if you forwarded' -and $block -match '192\.168\.1\.20\s+Same house') 'block: each line says when to use it'
Check ($block -match 'Copied to the clipboard: 26\.12\.34\.56') 'block: says what was copied'
Check ($block -match 'server_allow') 'block: internet address shown, no server_allow: says to set it'
Check ($block -match 'check-hosting\.cmd') 'block: names the self-check'
$block = (Format-JoinBlock (Get-JoinAddresses @((Addr '192.168.1.20' 'Wi-Fi')) '') 27510 $true '' '') -join "`n"
Check ($block -match 'internet address: not found' -and $block -match 'api\.ipify\.org') 'block: a failed lookup says how to find the address'
Check ($block -match 'Radmin VPN or Hamachi') 'block: no VPN: suggests one'
$block = (Format-JoinBlock (Get-JoinAddresses $local '') 27510 $false '26.1.1.1' '') -join "`n"
Check ($block -match 'Only these addresses may join \(server_allow\): 26\.1\.1\.1') 'block: names server_allow'

# What Join.bat accepts.
$cases = @(
    @(' 26.12.34.56 ', '26.12.34.56', ''), @('26.12.34.56.', '26.12.34.56', ''), @('26.12.34.56:27511', '26.12.34.56', '27511'),
    @('"25.1.2.3 : 27510"', '25.1.2.3', '27510'), @('udp://81.2.3.4/', '81.2.3.4', ''), @('localhost', '127.0.0.1', ''),
    @('my-pc.example.org', 'my-pc.example.org', ''), @("26.12.34.56`t", '26.12.34.56', ''))
foreach ($c in $cases) {
    $r = ConvertTo-JoinAddress $c[0]
    Check ($r -and $r.Server -eq $c[1] -and $r.Port -eq $c[2]) "join: '$($c[0])' -> $($c[1]) port '$($c[2])'"
}
foreach ($bad in @('', '26.1.2', '26.1.2.300', '1.2.3.4:0', '1.2.3.4:70000', 'a b;c', '1.2.3.4:x')) {
    Check ($null -eq (ConvertTo-JoinAddress $bad)) "join: '$bad' is refused"
}
$help = (Get-JoinFailureHelp '26.1.2.3') -join "`n"
Check ($help -match '1\. Wrong address' -and $help -match '2\. The host''s firewall' -and $help -match '3\. The VPN.*Radmin VPN') 'help: three causes, VPN for a 26.x address'
Check ((Get-JoinFailureHelp '81.2.3.4') -join "`n" -match 'forward UDP port') 'help: router forwarding for an internet address'
Check ((Get-JoinFailureHelp '192.168.1.20') -join "`n" -match 'inside the host''s house') 'help: same house for a LAN address'

# The join watch.
Check ((Get-JoinState '') -eq '') 'watch: no log yet'
Check ((Get-JoinState "x`ncoop: on, server 26.1.2.3:27510, name 'Saria'`n") -eq 'trying') 'watch: co-op started'
Check ((Get-JoinState "coop: on, server a:1, name 'S'`nI coop: welcomed as player 2 on a:1`n") -eq 'welcomed') 'watch: welcomed'
Check ((Get-JoinState "coop: on, server a:1, name 'S'`nW coop: server refused: name taken`r`n") -eq 'refused: name taken') 'watch: refused, with the reason'

# The self-check.
$exe = 'C:\Games\oot-coop\server\coop_server.exe'
function Rule([string]$Name, [bool]$Allow = $true, [string]$Program = $exe, [string]$Profile = 'Private', [string]$Remote = 'LocalSubnet', [string]$Proto = 'UDP', [string]$Port = '27510') {
    [pscustomobject]@{ Name = $Name; Enabled = $true; Inbound = $true; Allow = $Allow; Program = $Program; Protocol = $Proto; LocalPort = $Port; RemoteAddress = $Remote; Profile = $Profile }
}
function Findings($Rules, $Listen = @([pscustomobject]@{ LocalAddress = '0.0.0.0' }), $Local = @((Addr '192.168.1.20' 'Wi-Fi')), $Profiles = @(), $Access = @{ Allow = ''; Bind = '' }) {
    Get-HostingFindings $exe 27510 $Rules $Listen $Local $Profiles $Access
}
function Text($F, [string]$Level) { ($F | Where-Object { $_.Level -eq $Level } | ForEach-Object { $_.Lines }) -join "`n" }

$f = Findings @((Rule 'OoT co-op server UDP 27510'))
Check (-not (Text $f 'problem')) 'check: a LAN rule for the program, server running: no problem'
Check ((Text $f 'ok') -match 'every network' -and (Text $f 'ok') -match '192\.168\.1\.20') 'check: lists the networks it listens on'
Check ((Text $f 'note') -match 'server_allow is not set') 'check: says server_allow is not set'
$f = Findings @()
Check ((Text $f 'problem') -match 'No firewall rule' -and (Text $f 'problem') -match [regex]::Escape("program=`"$exe`" protocol=UDP localport=27510 profile=private remoteip=localsubnet")) 'check: no rule: prints the program-scoped LAN rule'
$f = Findings @() -Local @((Addr '192.168.1.20' 'Wi-Fi'), (Addr '25.4.5.6' 'Hamachi'))
Check ((Text $f 'problem') -match 'Over Hamachi:' -and (Text $f 'problem') -match 'profile=any remoteip=25\.0\.0\.0/8') 'check: on Hamachi: prints the Hamachi rule'
$f = Findings @() -Access @{ Allow = '81.9.9.9, 82.1.1.1'; Bind = '' }
Check ((Text $f 'problem') -match 'profile=private remoteip=81\.9\.9\.9,82\.1\.1\.1') 'check: port forwarding rule takes server_allow as remoteip'
Check ((Text $f 'ok') -match 'server_allow\): 81\.9\.9\.9') 'check: names server_allow'
$f = Findings @((Rule 'coop_server.exe' $false), (Rule 'OoT co-op server UDP 27510'))
Check ((Text $f 'problem') -match 'BLOCKS' -and (Text $f 'problem') -match 'delete rule name=all dir=in program=') 'check: a block rule is found, with the command to remove it'
$f = Findings @((Rule 'OoT co-op server UDP 27510' $true ''))
Check ((Text $f 'problem') -match 'any program' -and (Text $f 'problem') -match 'delete rule name="OoT co-op server UDP 27510"') 'check: a rule without program= is called too wide'
$f = Findings @((Rule 'r' $true $exe 'Private' 'LocalSubnet' 'TCP'))
Check ((Text $f 'problem') -match 'No firewall rule') 'check: a TCP-only rule does not count'
$f = Findings @((Rule 'r' $true $exe 'Private' 'Any' 'UDP' '27000-28000'))
Check (-not (Text $f 'problem')) 'check: a port range covering the port counts'
$f = Findings @((Rule 'r')) -Local @((Addr '26.1.2.3' 'Radmin VPN')) -Profiles @([pscustomobject]@{ Alias = 'Radmin VPN'; Category = 'Public' })
Check ((Text $f 'problem') -match 'Radmin VPN adapter a Public network' -and (Text $f 'problem') -match 'remoteip=26\.0\.0\.0/8') 'check: VPN on a Public network, rule only Private: says so'
$f = Findings @((Rule 'r')) -Profiles @([pscustomobject]@{ Alias = 'Wi-Fi'; Category = 'Public' })
Check ((Text $f 'problem') -match "home network 'Wi-Fi' Public" -and (Text $f 'problem') -match 'Private network') 'check: home network called Public: says to make it Private'
$f = Findings @((Rule 'r')) -Profiles @([pscustomobject]@{ Alias = 'Wi-Fi'; Category = 'Private' })
Check (-not (Text $f 'problem')) 'check: home network Private: no problem'
$f = Findings @((Rule 'r' $true $exe 'Any' 'Any'))
Check ((Text $f 'note') -match 'any Public network') 'check: an open rule on Public networks is noted'
$f = Findings @((Rule 'r')) -Listen @()
Check ((Text $f 'problem') -match 'not running') 'check: server not running'
$f = Findings @((Rule 'r')) -Listen @([pscustomobject]@{ LocalAddress = '25.4.5.6' })
Check ((Text $f 'ok') -match 'only on 25\.4\.5\.6') 'check: server_bind: names the one address'
$f = Findings $null
Check ((Text $f 'note') -match 'Could not read the Windows Firewall') 'check: unreadable rules are a note, not a crash'

if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
