# Installs the WSL Linux runners (docs/ci-runner.md, "Linux runners in WSL"): 3 runners squadala-wsl-1..3 in the
# WSL distro squadala-ci (its own Ubuntu 24.04, made here if missing; the owner's Ubuntu-24.04 with the local-CI
# user lci is never touched), label squadala-wsl. Runs as the owner, NOT elevated, from a checkout of this repo:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\wsl\install-wsl.ps1
# Safe to run again (it skips registered runners). What it does:
#  1. checks gh is daurosk, %UserProfile%\.wslconfig caps WSL (memory <= 20GB, processors <= 12) and free commit >= 40 GB;
#  2. copies tools\ci-runner\wsl into the distro over stdin and runs setup.sh as root (packages, user squadala-ci
#     without sudo, wsl.conf without interop or Windows drives, hooks, runner folders, systemd units);
#  3. restarts the distro so wsl.conf applies, then checks /mnt/c is gone and Windows programs do not start;
#  4. registers each runner with a fresh registration token piped on stdin (never printed, never on a command line);
#  5. registers the logon task 'squadala-ci status feed' (status-feed.ps1) and starts it.
param(
    [string]$Distro = 'squadala-ci',
    [int]$Runners = 3,
    [string]$Repo = 'daurosk/oot-remake-multiplayer',
    [string]$Gh = 'C:\Program Files\GitHub CLI\gh.exe'
)
$ErrorActionPreference = 'Stop'
$wsl = Join-Path $env:WINDIR 'System32\wsl.exe'
$src = $PSScriptRoot

function Invoke-WslStdin([string]$Arguments, [byte[]]$Bytes) {
    # Binary-safe stdin (a PowerShell 5.1 pipe re-encodes bytes); output goes to the console.
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $wsl; $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true
    # The stdin writer takes Console.InputEncoding; with UTF-8 (BOM) there it put EF BB BF before the tar
    # (2026-11-27: 'tar: This does not look like a tar archive'). Start wsl.exe with a BOM-less encoding.
    $oldIn = $null
    try { $oldIn = [Console]::InputEncoding; [Console]::InputEncoding = New-Object Text.UTF8Encoding $false } catch { }
    try { $p = [Diagnostics.Process]::Start($psi) } finally { if ($oldIn) { try { [Console]::InputEncoding = $oldIn } catch { } } }
    $p.StandardInput.BaseStream.Write($Bytes, 0, $Bytes.Length)
    $p.StandardInput.Close()
    $p.WaitForExit()
    return $p.ExitCode
}

# 1. Checks.
$login = (& $Gh api user --jq .login)
if ($login -ne 'daurosk') { throw "gh is signed in as '$login', not daurosk." }
$cfg = Get-Content -Raw (Join-Path $env:USERPROFILE '.wslconfig') -ErrorAction SilentlyContinue
if (-not ($cfg -match '(?m)^\s*memory\s*=\s*(\d+)GB' -and [int]$Matches[1] -le 20)) { throw '.wslconfig must cap memory at <= 20GB (docs/ci-runner.md).' }
if (-not ($cfg -match '(?m)^\s*processors\s*=\s*(\d+)' -and [int]$Matches[1] -le 12)) { throw '.wslconfig must cap processors at <= 12 (of 32: the test-lane games keep the rest).' }
$free = [math]::Floor((Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory / 1MB)
if ($free -lt 40) { throw "free commit is $free GB (< 40 GB): a game test is running; run this later (jobs keep their own 75 GB guard)." }
if (-not ((& $wsl -l -q) -replace "`0", '' | Where-Object { $_.Trim() -eq $Distro })) {
    # A distro of its own: wsl.conf below turns off interop and drives, which the local-CI distro needs.
    $loc = Join-Path $env:LOCALAPPDATA 'squadala-ci\wsl'
    New-Item -ItemType Directory -Force $loc | Out-Null
    $p = Start-Process -FilePath $wsl -ArgumentList "--install Ubuntu-24.04 --name $Distro --location `"$loc`" --no-launch" -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "wsl --install of $Distro failed ($($p.ExitCode))." }
}

# 2. Copy the scripts in and run setup.sh.
$tar = Join-Path $env:TEMP "squadala-ci-wsl-$PID.tar"
& (Join-Path $env:WINDIR 'System32\tar.exe') -C $src -cf $tar --exclude '*.ps1' .
if ($LASTEXITCODE -ne 0) { throw 'tar failed.' }
$bytes = [IO.File]::ReadAllBytes($tar); Remove-Item $tar
$code = Invoke-WslStdin "-d $Distro -u root -- sh -c `"rm -rf /opt/squadala-ci/src && mkdir -p /opt/squadala-ci/src && tar -C /opt/squadala-ci/src -xf - && sed -i 's/\r$//' /opt/squadala-ci/src/*.sh /opt/squadala-ci/src/hooks/*.sh`"" $bytes
if ($code -ne 0) { throw "copy into $Distro failed ($code)." }
& $wsl -d $Distro -u root -- env "RUNNERS=$Runners" bash /opt/squadala-ci/src/setup.sh
if ($LASTEXITCODE -ne 0) { throw "setup.sh failed ($LASTEXITCODE)." }

# 3. Restart the distro so wsl.conf (systemd, no interop, no drives) applies; check it did.
Get-ScheduledTask -TaskName 'squadala-ci status feed' -ErrorAction SilentlyContinue | Stop-ScheduledTask
& $wsl --terminate $Distro | Out-Null
$check = (& $wsl -d $Distro -u root -- sh -c 'test -e /mnt/c/Windows && echo DRIVE; command -v cmd.exe >/dev/null && echo PATH; (/init /bin/true 2>/dev/null; true); systemctl is-system-running 2>/dev/null || true') -join ' '
if ($check -match 'DRIVE|PATH') { throw "wsl.conf did not apply ($check): Windows drives or programs still reachable." }
# Public job logs print the machine name: it must be squadala-ci, never the PC (owner's) name.
$hn = ((& $wsl -d $Distro -u root -- hostname) -join '').Trim()
if ($hn -ne 'squadala-ci' -or $hn -ieq $env:COMPUTERNAME) { throw "distro hostname is '$hn', not squadala-ci." }
"distro hostname: $hn"
"distro restarted: no Windows drives or programs; systemd: $check"

# 4. Register the runners (token piped on stdin only).
for ($i = 1; $i -le $Runners; $i++) {
    $token = (& $Gh api -X POST "repos/$Repo/actions/runners/registration-token" --jq .token)
    if (-not $token) { throw 'No registration token.' }
    $code = Invoke-WslStdin "-d $Distro -u root -- /opt/squadala-ci/register.sh $i" ([Text.Encoding]::ASCII.GetBytes("$token`n"))
    $token = $null
    if ($code -ne 0) { throw "register.sh $i failed ($code)." }
}

# 5. Status feed at every logon (it also keeps the distro up), started now.
$dir = Join-Path $env:LOCALAPPDATA 'squadala-ci'
# The folder that holds oot-remake-multiplayer (lane locks) and coord-multiplayer (TEST-TURN): status-feed.ps1 reads
# the lanes' state there. From a worktree (..\wt-*), its parent is that folder too.
$ootRoot = Split-Path (Split-Path (Split-Path (Split-Path $src)))
if (-not (Test-Path -LiteralPath (Join-Path $ootRoot 'coord-multiplayer\TEST-TURN'))) { throw "no coord-multiplayer\TEST-TURN next to the checkout ($ootRoot)." }
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item (Join-Path $src 'status-feed.ps1') (Join-Path $dir 'status-feed.ps1') -Force
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$dir\status-feed.ps1`" -Distro $Distro -Root `"$ootRoot`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName 'squadala-ci status feed' -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'squadala-ci status feed'
Start-Sleep -Seconds 40
& $wsl -d $Distro -u root -- sh -c 'echo status time/commit GB/C: GB/lanes busy: $(cat /run/squadala-ci/status); systemctl is-active squadala-ci-runner@1 squadala-ci-runner@2 squadala-ci-runner@3'
# (no quotes inside the jq filter: Windows PowerShell 5.1 splits a native argument at embedded quotes)
& $Gh api "repos/$Repo/actions/runners" --jq '.runners[] | [.name, .status] | @tsv'
"WSL runners installed. Linux jobs use them only while the repo variable COOP_SELF_HOSTED_LINUX is on (docs/ci-runner.md)."
