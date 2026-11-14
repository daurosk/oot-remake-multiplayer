# Helpers for install.ps1 / uninstall.ps1 (docs/ci-runner.md). Dot-source only.

# Top-level folders Windows and installed programs need; oot-ci keeps the default
# access there (read and run; ProgramData also gets a no-create Deny on its root).
$script:OotCiSystemDirs = @('Windows', 'Program Files', 'Program Files (x86)', 'ProgramData', 'Users',
    '$Recycle.Bin', 'System Volume Information', 'Recovery', 'Documents and Settings', 'Config.Msi')

# Everything oot-ci must not reach, as "path|tree" (Deny full, inherited) or
# "path|root" (Deny creating files/folders in that folder itself only):
# every fixed drive root (root), ProgramData (root), every other top-level folder
# of a fixed drive (tree), and every user profile and Public (tree).
function Get-OotCiDenyTargets {
    param([string[]]$Skip = @(), [string]$User = 'oot-ci')
    $skipFull = $Skip | ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') }
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($drive in Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3') {
        $root = "$($drive.DeviceID)\"
        $out.Add("$root|root")
        foreach ($dir in Get-ChildItem $root -Directory -Force -ErrorAction SilentlyContinue) {
            if ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($OotCiSystemDirs -contains $dir.Name) { continue }
            if ($skipFull -contains $dir.FullName.TrimEnd('\')) { continue }
            $out.Add("$($dir.FullName)|tree")
        }
    }
    $out.Add("$env:ProgramData|root")
    $profiles = Get-CimInstance Win32_UserProfile | Where-Object { -not $_.Special -and $_.LocalPath } |
        ForEach-Object { $_.LocalPath }
    foreach ($p in @($profiles) + (Join-Path $env:SystemDrive 'Users\Public')) {
        if (-not (Test-Path $p)) { continue }
        if ((Split-Path $p -Leaf) -eq $User) { continue }
        $out.Add("$p|tree")
    }
    return $out | Select-Object -Unique
}

# Adds or removes oot-ci's SID on the deny-logon rights (interactive, RDP,
# network). -Remove also drops "Log on as a service", which config.cmd granted.
function Set-OotCiLogonRights {
    param([Parameter(Mandatory)][string]$Sid, [switch]$Add, [switch]$Remove)
    $rights = 'SeDenyInteractiveLogonRight', 'SeDenyRemoteInteractiveLogonRight', 'SeDenyNetworkLogonRight'
    if ($Remove) { $rights += 'SeServiceLogonRight' }
    $tmp = Join-Path $env:TEMP ("ootci-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $tmp | Out-Null
    try {
        & secedit /export /cfg "$tmp\now.inf" /areas USER_RIGHTS /quiet | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'secedit export failed.' }
        $now = Get-Content "$tmp\now.inf"
        $lines = foreach ($r in $rights) {
            $cur = $now | Where-Object { $_ -match "^$r\s*=" } | Select-Object -First 1
            $vals = @()
            if ($cur) { $vals = ($cur -split '=', 2)[1].Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ } }
            $vals = @($vals | Where-Object { $_ -ne "*$Sid" })
            if ($Add -and $r -ne 'SeServiceLogonRight') { $vals += "*$Sid" }
            "$r = $($vals -join ',')"
        }
        @('[Unicode]', 'Unicode=yes', '[Version]', 'signature="$CHICAGO$"', 'Revision=1', '[Privilege Rights]') + $lines |
            Set-Content -Encoding Unicode "$tmp\set.inf"
        & secedit /configure /db "$tmp\set.sdb" /cfg "$tmp\set.inf" /areas USER_RIGHTS /quiet | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'secedit configure failed.' }
    } finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
}
