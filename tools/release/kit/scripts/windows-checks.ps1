# What Windows itself may block, checked before any other script of the kit runs. "Start OoT Co-op.bat" runs this
# file's text through Invoke-Expression (powershell -Command), not as a script file: a policy that refuses script
# files then still lets it say why. So it is written for PowerShell's Constrained Language mode too (Smart App
# Control and other application control put PowerShell in it): cmdlets and plain strings only, no .NET calls.
# A player turned Defender's real-time protection off and added the folder to its exclusions, and the kit was still
# blocked: none of these five is changed by those two settings.
#   1. Mark of the Web: Explorer's "Extract All" marks every file from a downloaded zip as "from the internet".
#      SmartScreen asks about each marked program ("Windows protected your PC"), and a RemoteSigned policy refuses
#      marked scripts. The kit's own files are unmarked here (Unblock-File), once.
#   2. Smart App Control (Windows 11): blocks programs and scripts without a known signature; Defender's settings
#      do not change it. Its blocks are in the Code Integrity log, which names the file.
#   3. Constrained Language mode (Smart App Control, WDAC or AppLocker): the kit's scripts cannot run at all.
#   4. An execution policy set by Group Policy (a work or school PC, or a tweak tool): it overrides the
#      -ExecutionPolicy Bypass the kit starts PowerShell with.
#   5. Controlled folder access: Defender stops unknown programs (the game build's compilers) writing in
#      Documents, Desktop, Pictures and the other folders it protects, even with real-time protection off.
# Every run writes what it found to logs\windows-checks.txt (collect-logs puts it in the zip).
# Exit 1 when the kit cannot run on this PC as it is (3, 4 AllSigned/Restricted); the others are said and the
# start goes on. Called with $k = the kit folder (with or without a trailing \). Tests: $OOT_COOP_WC_* stand-ins.

# (full and without the trailing \: collect-logs.cmd passes "<kit>\scripts\..")
function Get-WcKit([string]$Kit) {
    $r = Resolve-Path -LiteralPath $Kit -ErrorAction SilentlyContinue
    $full = if ($r) { "$($r.ProviderPath)" } else { $Kit }
    $full.TrimEnd('\')
}

# The kit's files that carry the "from the internet" mark. -All: every file; otherwise the few every kit has,
# enough to tell whether the unzip marked them (a marked unzip marks all).
function Get-WcMarked([string]$Kit, [switch]$All) {
    $files = @()
    if ($All) {
        # (not the built game, Longshot's tools or the build's work folder: the build made those here, unmarked, and
        # they are many thousands of files)
        $skip = @('hookshot\game', 'hookshot\work', 'hookshot\tools', 'hookshot\tools.new', 'hookshot\downloads', 'logs')
        $files = @(Get-ChildItem -LiteralPath $Kit -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object {
            $rel = $_.FullName.Substring($Kit.Length + 1)
            -not @($skip | Where-Object { $rel -like "$_\*" }).Count
        })
    } else {
        foreach ($n in 'Start OoT Co-op.bat', 'scripts\start.ps1', 'scripts\play.ps1', 'hookshot\hookshot_coop.dll', 'server\coop_server.exe') {
            $p = Join-Path $Kit $n
            if (Test-Path -LiteralPath $p) { $files += Get-Item -LiteralPath $p -Force }
        }
    }
    @($files | Where-Object { Get-Item -LiteralPath $_.FullName -Stream 'Zone.Identifier' -ErrorAction SilentlyContinue })
}

# Smart App Control: 'on', 'evaluation' (it only watches, and may turn itself on later) or 'off'.
function Get-WcSmartAppControl {
    if ($env:OOT_COOP_WC_SAC) { return $env:OOT_COOP_WC_SAC }
    $v = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name 'VerifiedAndReputablePolicyState' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
    switch ("$v") { '1' { 'on' } '2' { 'evaluation' } default { 'off' } }
}

# The kit's files Windows' application control (Smart App Control, WDAC) refused in the last 14 days, from the Code
# Integrity log (3033: not loaded, 3077: blocked). The log names files as \Device\HarddiskVolumeN\...: matched
# without the drive. Returns the kit-relative names, newest first.
function Get-WcBlockedFiles([string]$Kit) {
    $lines = @()
    if ($env:OOT_COOP_WC_CI) { $lines = @($env:OOT_COOP_WC_CI -split '\|') }
    else {
        try {
            $lines = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3033, 3077; StartTime = (Get-Date).AddDays(-14) } -MaxEvents 200 -ErrorAction Stop |
                ForEach-Object { $_.Message })
        } catch { }
    }
    $tail = $Kit.Substring(2).ToLower() + '\'   # ("D:\coop" -> "\coop\")
    $found = @()
    foreach ($m in $lines) {
        $low = "$m".ToLower()
        $at = $low.LastIndexOf($tail)
        if ($at -lt 0) { continue }
        $rest = "$m".Substring($at + $tail.Length)
        $end = $rest.IndexOfAny([char[]]@(' ', '"', ')', "`r", "`n"))
        $name = if ($end -gt 0) { $rest.Substring(0, $end) } else { $rest }
        # (the process that tried is named first in the message, and may be the kit's own (longshot.exe loading
        # hookshot_coop.dll): the last name in the kit's folder is the file refused)
        if ($found -notcontains $name) { $found += $name }
    }
    $found
}

# The execution policy a Group Policy sets ('' when none): it wins over -ExecutionPolicy Bypass.
function Get-WcPolicy {
    if ($env:OOT_COOP_WC_POLICY) { return $env:OOT_COOP_WC_POLICY }
    foreach ($s in 'MachinePolicy', 'UserPolicy') {
        $p = "$(Get-ExecutionPolicy -Scope $s)"
        if ($p -and $p -ne 'Undefined') { return $p }
    }
    return ''
}

# The folder controlled folder access protects that holds the kit, or '' (off, or the kit is elsewhere).
function Get-WcProtectedFolder([string]$Kit) {
    $on = $env:OOT_COOP_WC_CFA
    $extra = @()
    if (-not $on) {
        try {
            $mp = Get-MpPreference -ErrorAction Stop
            $on = "$($mp.EnableControlledFolderAccess)"
            $extra = @($mp.ControlledFolderAccessProtectedFolders | Where-Object { $_ })
        } catch { return '' }
    }
    if ($on -ne '1') { return '' }
    $folders = @($extra)
    foreach ($f in 'Documents', 'Desktop', 'Pictures', 'Videos', 'Music', 'Favorites') {
        $folders += Join-Path $env:USERPROFILE $f
        if ($env:OneDrive) { $folders += Join-Path $env:OneDrive $f }
    }
    if ($env:PUBLIC) { foreach ($f in 'Documents', 'Desktop', 'Pictures', 'Videos', 'Music') { $folders += Join-Path $env:PUBLIC $f } }
    foreach ($f in $folders) {
        $t = "$f".TrimEnd('\')
        if ($t -and ($Kit -ieq $t -or $Kit -like "$t\*")) { return $t }
    }
    return ''
}

# Controlled folder access: '1' on, '2' audit only, '0' or '' off or not known.
function Get-WcCfa {
    if ($env:OOT_COOP_WC_CFA) { return $env:OOT_COOP_WC_CFA }
    try { return "$((Get-MpPreference -ErrorAction Stop).EnableControlledFolderAccess)" } catch { return '' }
}

# '' when PowerShell can make a file in the kit's logs folder, else Windows' answer ("Access to the path ... is denied").
# (a player's start stopped on "access denied" with real-time protection off: controlled folder access, most likely)
function Test-WcWrite([string]$Kit) {
    if ($env:OOT_COOP_WC_WRITE) { return $env:OOT_COOP_WC_WRITE }
    $probe = Join-Path $Kit "logs\write-test-$PID.tmp"
    try {
        New-Item -ItemType Directory -Force -Path (Join-Path $Kit 'logs') -ErrorAction Stop | Out-Null
        Set-Content -LiteralPath $probe -Value 'ok' -ErrorAction Stop
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        return ''
    } catch { return "$($_.Exception.Message)" }
}

# All the checks. Returns @{ Lines = what to say; Log = what to write; Stop = $true when the kit cannot run }.
function Invoke-WcChecks([string]$Kit) {
    $Kit = Get-WcKit $Kit
    $say = @(); $log = @(); $stop = $false
    $mode = if ($env:OOT_COOP_WC_MODE) { $env:OOT_COOP_WC_MODE } else { "$($ExecutionContext.SessionState.LanguageMode)" }
    $sac = Get-WcSmartAppControl
    $policy = Get-WcPolicy
    $log += "language mode: $mode"
    $log += "smart app control: $sac"
    $log += "execution policy (group policy): $(if ($policy) { $policy } else { 'none' })"
    try { $log += 'execution policies: ' + ((Get-ExecutionPolicy -List | ForEach-Object { "$($_.Scope)=$($_.ExecutionPolicy)" }) -join ', ') } catch { }
    $cfaOn = Get-WcCfa
    $log += "controlled folder access: $(switch ($cfaOn) { '1' { 'on' } '2' { 'audit' } '' { 'not known' } default { 'off' } })"

    # 1. The mark, taken off the kit's own files once (a marked unzip marks them all).
    $marked = @(Get-WcMarked $Kit)
    if ($marked.Count) {
        $all = @(Get-WcMarked $Kit -All)
        $all | ForEach-Object { Unblock-File -LiteralPath $_.FullName -ErrorAction SilentlyContinue }
        $left = @($all | Where-Object { Get-Item -LiteralPath $_.FullName -Stream 'Zone.Identifier' -ErrorAction SilentlyContinue })
        $log += "internet mark: $($all.Count) file(s), $($left.Count) still marked"
        if ($left.Count) {
            $say += "Windows marks this kit's files as downloaded from the internet, and the mark could not be taken off ($($left[0].Name))."
            $say += '  If Windows asks "Windows protected your PC": More info, then Run anyway. Or: right-click the kit''s zip,'
            $say += '  Properties, tick "Unblock", OK, and unzip it again.'
        } else {
            $say += "Took Windows' 'downloaded from the internet' mark off this kit's $($all.Count) files (once), so Windows does not ask about each one."
        }
    } else { $log += 'internet mark: none' }

    # 3. Scripts locked: nothing else of the kit can run.
    if ($mode -ne 'FullLanguage') {
        $stop = $true
        if ($sac -eq 'on') {
            $say += 'Windows Smart App Control is blocking this kit''s PowerShell scripts (PowerShell runs in "Constrained Language" mode).'
            $say += '  Defender''s exclusions and real-time protection do not change Smart App Control. To use the kit, turn it off:'
            $say += '  Windows Security > App & browser control > Smart App Control settings > Off.'
            $say += '  Note: Windows cannot turn Smart App Control back on without a reset of Windows; that is your choice to make.'
        } else {
            $say += "PowerShell on this PC runs in ""$mode"" mode: an application-control policy (AppLocker or WDAC, usually a work or"
            $say += '  school PC) stops this kit''s scripts. The kit cannot run here unless the PC''s owner allows it.'
        }
    }
    # 4. A Group Policy execution policy.
    if ($policy -in @('AllSigned', 'Restricted')) {
        $stop = $true
        $say += "A Group Policy on this PC sets PowerShell's execution policy to $policy (work or school PCs, or a privacy or tweak tool),"
        $say += '  and it wins over the kit''s own setting: the kit''s scripts (not signed) cannot run. Ask the PC''s owner, or (your own PC)'
        $say += '  set "Turn on Script Execution" back to "Not configured" in gpedit.msc > Computer/User Configuration > Administrative'
        $say += '  Templates > Windows Components > Windows PowerShell.'
    }
    # 2. Smart App Control on while scripts still run: what it refused, by name.
    $blocked = @(Get-WcBlockedFiles $Kit)
    if ($blocked.Count) { $log += "blocked by code integrity: $($blocked -join ', ')" } else { $log += 'blocked by code integrity: none found' }
    if ($sac -eq 'on' -and -not $stop) {
        if ($blocked.Count) {
            $say += "Windows Smart App Control blocked $($blocked[0]) of this kit$(if ($blocked.Count -gt 1) { " (and $($blocked.Count - 1) more)" })."
        } else {
            $say += 'Windows Smart App Control is on. It may block the kit''s programs (they are not signed), whatever Defender''s settings.'
        }
        $say += '  If the kit stops with a "blocked" message: Windows Security > App & browser control > Smart App Control settings > Off'
        $say += '  (Windows cannot turn it back on without a reset of Windows).'
    } elseif ($blocked.Count -and -not $stop) {
        $say += "Windows' application control blocked $($blocked[0]) of this kit (the Code Integrity log says so). On a work or school PC, ask its owner."
    }
    # 6. Can PowerShell write here at all? ("access denied": controlled folder access, another antivirus, a read-only folder)
    $denied = Test-WcWrite $Kit
    # (Windows' answer names the path, and so the user: only whether it was denied goes in the log)
    $log += "write test in the kit folder: $(if (-not $denied) { 'ok' } elseif ($denied -match '(?i)denied') { 'access denied' } else { 'failed' })"
    if ($denied) {
        $stop = $true
        $say += 'Windows refused to let PowerShell write in this kit''s folder (access denied). Nothing of the kit can work without that.'
        if ($cfaOn -eq '1') {
            $say += '  Controlled folder access (ransomware protection) is on, and it blocks this even with real-time protection off.'
            $say += '  Windows Security > Virus & threat protection > Ransomware protection > "Allow an app through Controlled folder access" >'
            $say += '  Add an allowed app > All recent blocked apps: allow powershell.exe (and, later, the build''s python.exe and clang.exe).'
            $say += '  Or turn Controlled folder access off there, or move the kit to a folder it does not protect.'
        } else {
            $say += '  Another antivirus, or the folder''s permissions, may do this. Move the kit to a short folder of your own such as C:\OoT-Coop,'
            $say += '  or allow powershell.exe in your antivirus. Windows Security > Protection history says what was blocked.'
        }
    }
    # 5. Controlled folder access over the kit's folder.
    $cfa = if ($denied) { '' } else { Get-WcProtectedFolder $Kit }
    # (the folder's own name only: the log holds no user name)
    $log += "controlled folder access over the kit: $(if ($cfa) { Split-Path -Leaf $cfa } else { 'no' })"
    if ($cfa) {
        $say += "Controlled folder access (Windows Security) protects $cfa, where this kit is: it stops the game build writing there,"
        $say += '  even with real-time protection off. Move the kit to a short folder such as C:\OoT-Coop and start it there.'
    }
    @{ Lines = $say; Log = $log; Stop = $stop }
}

if ($k) {
    $wc = Invoke-WcChecks $k
    foreach ($l in $wc.Lines) { Write-Host $l -ForegroundColor Yellow }
    $text = @("checked: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC") + $wc.Log + @($wc.Lines | ForEach-Object { "said: $_" })
    # (no user name in it: collect-logs' fallback zip sends it as it is)
    if ($env:USERPROFILE) { $text = @($text | ForEach-Object { "$_".Replace($env:USERPROFILE, '%USERPROFILE%') }) }
    # (when the kit's folder takes no file: %LOCALAPPDATA%\OoT-Coop, where collect-logs looks too)
    $put = ''
    foreach ($logs in @((Join-Path (Get-WcKit $k) 'logs'), $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'OoT-Coop' }))) {
        if (-not $logs) { continue }
        try {
            New-Item -ItemType Directory -Force -Path $logs -ErrorAction Stop | Out-Null
            $text | Set-Content -LiteralPath (Join-Path $logs 'windows-checks.txt') -Encoding UTF8 -ErrorAction Stop
            $put = Join-Path $logs 'windows-checks.txt'
            break
        } catch { }
    }
    if ($wc.Stop -and $put) { Write-Host "  (what was found: $put; send it with your report)" }
    if ($wc.Stop) { exit 1 }
}
