# Registers a Windows Scheduled Task that keeps the bug-report bot running.
# Run it yourself once (no admin needed):  powershell -ExecutionPolicy Bypass -File install-task.ps1
# Remove it again:                         powershell -ExecutionPolicy Bypass -File install-task.ps1 -Remove
# Triggers: at logon, and every 10 minutes for good (a dead loop or a killed process comes back by itself;
# MultipleInstances IgnoreNew means a running bot is never started twice). A start-at-boot trigger is added only
# when this window is elevated (it needs admin); otherwise it is skipped and the logon + repeat triggers cover it.
param([switch]$Remove, [string]$TaskName = 'Squadala report bot')
$ErrorActionPreference = 'Stop'
if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed task '$TaskName'."
    exit 0
}
$script = (Resolve-Path (Join-Path $PSScriptRoot 'run-bot.ps1')).Path
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script`"" `
    -WorkingDirectory $PSScriptRoot
$user = "$env:USERDOMAIN\$env:USERNAME"
$logon = New-ScheduledTaskTrigger -AtLogOn -User $user
$triggers = @($logon)
# Repeating trigger: starts now, repeats every 10 minutes indefinitely (an empty duration means forever).
$repeat = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) `
    -RepetitionInterval (New-TimeSpan -Minutes 10) -RepetitionDuration (New-TimeSpan -Days 9999)
$triggers += $repeat
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($admin) { $triggers += New-ScheduledTaskTrigger -AtStartup }
else { Write-Host "Not elevated: no start-at-boot trigger (needs admin); logon + every-10-minutes triggers are enough." }
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 5) `
    -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $triggers -Settings $settings -Force `
    -Description 'Squadala! Discord bug-report bot (tools/discord-bot)' | Out-Null
Write-Host "Registered task '$TaskName': $script at logon$(if ($admin) { ', at boot' }) and every 10 minutes (never twice at once). Start it now with: Start-ScheduledTask -TaskName '$TaskName'"
