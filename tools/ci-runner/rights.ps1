# Helpers for install.ps1 / deny.ps1 / uninstall.ps1 (docs/ci-runner.md). Dot-source only.
. (Join-Path $PSScriptRoot 'job-check.ps1')

# Everything oot-ci must not reach, as "path|tree" (Deny full, inherited) or
# "path|root" (Deny creating files/folders in that folder itself only): every
# fixed drive root and ProgramData (root), and every guarded folder (tree): all
# non-system top-level folders and every profile under Users, Public included.
# The hook (job-started.ps1) refuses jobs while any guarded folder is open.
function Get-OotCiDenyTargets {
    param([string[]]$Skip = @(), [string]$User = 'oot-ci')
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($d in [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady }) {
        $out.Add("$($d.RootDirectory.FullName)|root")
    }
    $out.Add("$env:ProgramData|root")
    foreach ($p in Get-OotCiGuardedFolders -Skip $Skip -User $User) { $out.Add("$p|tree") }
    return $out | Select-Object -Unique
}

# True when the account $MemberSid is a direct member of the local group $GroupSid.
# ADSI, not Get-LocalGroupMember: on Windows PowerShell 5.1 that throws on a group
# holding a deleted account or an Entra ID member. Group names are looked up from
# the SID, so this also works on non-English Windows.
function Test-OotCiInLocalGroup {
    param([Parameter(Mandatory)][string]$GroupSid, [Parameter(Mandatory)][string]$MemberSid)
    $name = (New-Object Security.Principal.SecurityIdentifier $GroupSid).Translate([Security.Principal.NTAccount]).Value.Split('\')[-1]
    $group = [ADSI]"WinNT://$env:COMPUTERNAME/$name,group"
    foreach ($m in @($group.psbase.Invoke('Members'))) {
        $bytes = $m.GetType().InvokeMember('objectSid', 'GetProperty', $null, $m, $null)
        if ($bytes -and (New-Object Security.Principal.SecurityIdentifier ([byte[]]$bytes), 0).Value -eq $MemberSid) { return $true }
    }
    return $false
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
