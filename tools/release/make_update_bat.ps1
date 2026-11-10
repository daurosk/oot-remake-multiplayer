# Writes Update.bat: kit\update-bat-head.cmd, a marker line, then kit\scripts\update.ps1.
# One file, so it also works copied into an old kit that has no scripts\update.ps1.
# make_kit.ps1 puts it in every kit; the release step attaches the same file to the release.
#   make_update_bat.ps1 -Out <path\Update.bat>
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $PSScriptRoot 'kit'
$head = [System.IO.File]::ReadAllText((Join-Path $kit 'update-bat-head.cmd')).Replace("`r`n", "`n").TrimEnd("`n")
$body = [System.IO.File]::ReadAllText((Join-Path $kit 'scripts\update.ps1')).Replace("`r`n", "`n")
$text = $head + "`n#UPDATE-PS1#`n" + $body
if ($text -match '[^\x00-\x7F]') { throw 'Update.bat must be plain ASCII (cmd reads it in the console code page).' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent ([System.IO.Path]::GetFullPath($Out))) | Out-Null
[System.IO.File]::WriteAllText($Out, $text.Replace("`n", "`r`n"), [System.Text.Encoding]::ASCII)
