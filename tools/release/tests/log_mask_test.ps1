# Tests for the log masking in collect-logs.ps1 (tools/release/kit/scripts/log-mask.ps1):
# every kind of address, ports kept, the same address the same tag, MAC addresses, the user
# name in paths, and version numbers left alone. Strings only; at the end the zip (Write-LogZip and a collect-logs.ps1 run on a small kit in %TEMP%).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\log_mask_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\log-mask.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Mask([string]$Text, $State = (New-MaskState)) { Protect-LogText $Text $State 'Testuser' 'TESTPC-01' }

# One line per kind.
$lines = [ordered]@{
    'server = 192.168.1.20'         = 'server = <lan-1>'
    'server = 10.0.0.5'             = 'server = <lan-1>'
    'server = 172.20.3.4'           = 'server = <lan-1>'
    'server = 100.101.102.103'      = 'server = <tailscale-1>'
    'server = 25.1.2.3'             = 'server = <hamachi-1>'
    'server = 26.12.34.56'          = 'server = <radmin-1>'
    'server = 127.0.0.1'            = 'server = <loopback-1>'
    'server = 8.8.8.8'              = 'server = <public-1>'
    'coop_server: invite code: ABCDE-FGHJK-LMNPQ-RSTUV' = 'coop_server: invite code: (removed)'
    'peer fe80::1c2b:3a4d:5e6f:7a8b%12 up' = 'peer <lan-1> up'
    'peer fd12:3456:789a::1 up'     = 'peer <lan-1> up'
    'peer fd7a:115c:a1e0::1234:5678 up' = 'peer <tailscale-1> up'
    'peer 2620:9b::1900:abcd up'    = 'peer <hamachi-1> up'
    'peer ::1 up'                   = 'peer <loopback-1> up'
    'peer 2001:db8:85a3::8a2e:370:7334 up' = 'peer <public-1> up'
}
foreach ($k in $lines.Keys) { $got = Mask $k; Check ($got -eq $lines[$k]) "'$k' -> '$($lines[$k])' (got '$got')" }

# The hook's startup line keeps its port; IPv6 in brackets keeps its port too.
$got = Mask "coop: on, server 203.0.113.5:9050, name 'Link'"
Check ($got -eq "coop: on, server <public-1>:9050, name 'Link'") "hook line keeps the port (got '$got')"
$got = Mask 'listening on [2001:db8::5]:27510'
Check ($got -eq 'listening on [<public-1>]:27510') "IPv6 with a port (got '$got')"

# The same address is the same tag everywhere (state shared across files), a new one the next.
$s = New-MaskState
$a = Mask 'join 203.0.113.5:9050' $s
$b = Mask 'from 198.51.100.7, again 203.0.113.5' $s
$c = Mask 'lan 192.168.0.2' $s
Check ($a -eq 'join <public-1>:9050' -and $b -eq 'from <public-2>, again <public-1>' -and $c -eq 'lan <lan-1>') "numbered tags across files (got '$a' / '$b' / '$c')"
$d = Mask 'peer 2001:DB8::5 and 2001:db8:0::5' $s
Check ($d -eq 'peer <public-3> and <public-3>') "IPv6 written two ways is one tag (got '$d')"

# MAC addresses, the user name and the PC name.
$got = Mask 'adapter 00-1A-2B-3C-4D-5E and 00:1a:2b:3c:4d:5f'
Check ($got -eq 'adapter <mac> and <mac>') "MAC addresses (got '$got')"
$got = Mask 'loaded C:\Users\Jane Doe\Desktop\kit\emulator\lichii.exe'
Check ($got -eq 'loaded C:\Users\<user>\Desktop\kit\emulator\lichii.exe') "user name in a path (got '$got')"
$got = Mask 'path c:/users/jdoe/AppData'
Check ($got -eq 'path c:/users/<user>/AppData') "user name in a forward-slash path (got '$got')"
$got = Mask 'D:\games\Testuser\kit on TESTPC-01'
Check ($got -eq 'D:\games\<user>\kit on <pc>') "user name and PC name on their own (got '$got')"

# Passwords in coop.ini and coop-options.ini; an empty one stays empty.
$got = Mask "[coop]`r`nserver = 25.1.2.3`r`npassword = hunter2 x`r`n"
Check ($got -eq "[coop]`r`nserver = <hamachi-1>`r`npassword = (removed)`r`n") "coop.ini password removed (got '$got')"
$got = Mask "server_password=s3cret`nserver_allow = 81.2.3.4`nserver_password =`n"
Check ($got -eq "server_password= (removed)`nserver_allow = <public-1>`nserver_password =`n") "coop-options.ini server_password removed, empty kept (got '$got')"

$got = Mask "Password : abc`n--password hunter2 --port 27510`n-password=xyz"
Check ($got -eq "Password : (removed)`n--password (removed) --port 27510`n-password (removed)") "other password forms removed (got '$got')"

# IPv4-mapped IPv6: no octet leaks, and it shares the plain IPv4's tag.
$s = New-MaskState
$a = Mask 'from ::ffff:203.0.113.9 and 203.0.113.9' $s
$b = Mask 'peer [::ffff:192.168.1.5]:9050' $s
$c = Mask 'nat64 64:ff9b::203.0.113.1 ok. end 2001:db8::5.' $s
Check ($a -eq 'from <public-1> and <public-1>' -and $b -eq 'peer [<lan-1>]:9050') "IPv4-mapped IPv6 (got '$a' / '$b')"
Check ($c -notmatch '113' -and $c -match 'end <public-\d>\.$') "embedded IPv4 and an address before a full stop (got '$c')"

# A server host name, the dotted MAC form, and a short or common user name left alone.
$got = Mask "server = myhost.duckdns.org`nserver = localhost`nserver_allow = 1.2.3.4"
Check ($got -eq "server = <host>`nserver = localhost`nserver_allow = <public-1>") "server host name (got '$got')"
$got = Mask 'mac aabb.ccdd.eeff'
Check ($got -eq 'mac <mac>') "dotted MAC (got '$got')"
$got = Protect-LogText 'Link spawned near Link_actor' (New-MaskState) 'Link' 'PC1'
Check ($got -eq 'Link spawned near Link_actor') "a common user name is not replaced on its own (got '$got')"
$got = Protect-LogText 'C:\Users\Link\kit' (New-MaskState) 'Link' 'PC1'
Check ($got -eq 'C:\Users\<user>\kit') "but it is replaced in a path (got '$got')"

# What must stay.
foreach ($keep in @('game 1.1.1 (v131072)', 'Microsoft Windows NT 10.0.26200.0', 'driver 32.0.15.6094',
                    'bind 0.0.0.0:27510', '[12:34:56.789] tick', 'Core::Frontend started', 'protocol 9, build v0.1.0', 'v1.2.3.4', 'join-password 191', 'server-password set','fritchii-v5.8.4')) {
    $got = Mask $keep
    Check ($got -eq $keep) "'$keep' stays (got '$got')"
}

# The zip (Write-LogZip; player report: '\' names from Compress-Archive on a PC without tar.exe).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function Get-ZipNames([string]$Zip) {
    $z = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try { @($z.Entries | ForEach-Object { $_.FullName }) } finally { $z.Dispose() }
}
function Test-ZipNames([string[]]$Names, [string[]]$Want, [string]$What) {
    Check (@($Names | Where-Object { $_.Contains('\') }).Count -eq 0) "$What`: no '\' in any name [$($Names -join ', ')]"
    Check (@($Names | Where-Object { $_ -like './*' -or $_ -eq '.' -or $_.EndsWith('/') }).Count -eq 0) "$What`: no './' and no folder entries"
    $missing = @($Want | Where-Object { $Names -notcontains $_ })
    Check ($missing.Count -eq 0 -and $Names.Count -eq $Want.Count) "$What`: every file once, nothing else (missing: $($missing -join ', '))"
}
$zt = Join-Path ([System.IO.Path]::GetTempPath()) "coop-logzip-test-$PID"
try {
    $stage = Join-Path $zt 'stage'
    $want = @('VERSION.txt', 'hookshot/coop.ini', 'hookshot/build/build.log', 'emulator/log/eden.log', 'deep/a/b/c.txt')
    foreach ($w in $want) {
        $p = Join-Path $stage ($w.Replace('/', '\'))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $p) | Out-Null
        [System.IO.File]::WriteAllText($p, "text of $w")
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'empty') | Out-Null
    $zip = Join-Path $zt 'out.zip'
    [System.IO.File]::WriteAllText($zip, 'an old file')   # replaced, not appended to
    Write-LogZip "$stage\" $zip
    Test-ZipNames (Get-ZipNames $zip) $want 'log zip'
    $z = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $r = New-Object System.IO.StreamReader($z.GetEntry('hookshot/build/build.log').Open())
        Check ($r.ReadToEnd() -eq 'text of hookshot/build/build.log') 'log zip: the file''s text comes back'
        $r.Dispose()
    } finally { $z.Dispose() }
    # an error half-way leaves no zip behind
    $lock = [System.IO.File]::Open((Join-Path $stage 'VERSION.txt'), 'Open', 'Read', 'None')
    $threw = $false
    try { Write-LogZip $stage $zip } catch { $threw = $true } finally { $lock.Dispose() }
    Check ($threw -and -not (Test-Path -LiteralPath $zip)) 'log zip: a file that cannot be read -> an error, no half zip left'

    # collect-logs.ps1 itself on a small Longshot kit (no emulator): the zip it writes is standard.
    $kit = Join-Path $zt 'kit'
    $src = Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts'
    New-Item -ItemType Directory -Force -Path (Join-Path $kit 'scripts'), (Join-Path $kit 'logs'), (Join-Path $kit 'hookshot\game') | Out-Null
    foreach ($s in 'collect-logs.ps1', 'log-mask.ps1', 'kit-paths.ps1', 'console-mode.ps1') { Copy-Item -LiteralPath (Join-Path $src $s) -Destination (Join-Path $kit 'scripts') }
    foreach ($p in 'VERSION.txt', 'coop-options.ini', 'logs\coop_server.log', 'hookshot\build.log', 'hookshot\game\longshot.exe',
                   'hookshot\game\longshot_log.txt', 'hookshot\game\coop.ini') {
        [System.IO.File]::WriteAllText((Join-Path $kit $p), "x $p from 192.168.1.20")
    }
    $env:CI = '1'
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kit 'scripts\collect-logs.ps1') 2>&1 | Out-String
    $made = @(Get-ChildItem -LiteralPath $kit -Filter 'coop-logs-*.zip' -File)
    Check ($LASTEXITCODE -eq 0 -and $made.Count -eq 1) "collect-logs: ran and wrote one zip [$($out.Trim() -replace '\s+', ' ')]"
    # Issue #67: it says at once that it runs, names each file with its size, and ends with the zip's path.
    $outLines = @($out -split '\r?\n' | Where-Object { $_.Trim() })
    Check ($outLines[0] -eq 'Collecting your logs (this can take a few minutes)...') "collect-logs: the first line says it runs [$($outLines[0])]"
    Check ($out -match '(?m)^  hookshot\\longshot_log\.txt \(\d+ bytes\)\r?$' -and $out -match '(?m)^  server\\coop_server\.log \(') 'collect-logs: one line per file with its size'
    Check ($made.Count -eq 1 -and $outLines[-1] -eq "Send this zip: $($made[0].FullName)") "collect-logs: the last line is the zip's path [$($outLines[-1])]"
    Check ($out -match 'type /bugreport' -and $out -match 'discord\.gg/GwtRKAqng5' -and $out -cnotmatch '\bDM\b') 'collect-logs: says to attach the zip to /bugreport on the Discord, no DM'
    if ($made.Count -eq 1) {
        $names = Get-ZipNames $made[0].FullName
        Test-ZipNames $names @('about-this-pc.txt', 'VERSION.txt', 'coop-options.ini', 'server/coop_server.log', 'hookshot/build/build.log',
            'hookshot/longshot_log.txt', 'hookshot/coop.ini') 'collect-logs zip'
        $z = [System.IO.Compression.ZipFile]::OpenRead($made[0].FullName)
        try {
            $r = New-Object System.IO.StreamReader($z.GetEntry('hookshot/longshot_log.txt').Open())
            Check ($r.ReadToEnd() -match '<lan-1>') 'collect-logs zip: text still masked'
            $r.Dispose()
        } finally { $z.Dispose() }
    }
} finally { Remove-Item -LiteralPath $zt -Recurse -Force -ErrorAction SilentlyContinue }

if ($failed) { Write-Host "$failed failed"; exit 1 }
Write-Host 'all passed'
