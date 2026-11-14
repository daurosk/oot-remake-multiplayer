@echo off
rem Updates this co-op kit to the newest release (SETUP.md, "Updating"). Your saves, keys,
rem settings, puppet pack and coop-options.ini are kept. It must sit in the kit folder,
rem beside "Start OoT Co-op.bat" (Host.bat in an older emulator kit). This one file is all it needs: the PowerShell script that does the
rem work is the rest of this file, from the marker line below on (make_update_bat.ps1).
rem It is copied to a temp file and run from there, so it can replace this file.
rem Everything is on one line: cmd reads a .bat as it runs, and this one may be replaced.
set "OOT_COOP_UPDATE_BAT=%~f0"
set "OOT_COOP_UPDATE_PS1=%TEMP%\oot-coop-update-%RANDOM%%RANDOM%.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t = [IO.File]::ReadAllText($env:OOT_COOP_UPDATE_BAT); $m = [char]35 + 'UPDATE-PS1' + [char]35; $i = $t.IndexOf($m); if ($i -lt 0) { Write-Host 'Update.bat is damaged: download it again.'; exit 1 }; [IO.File]::WriteAllText($env:OOT_COOP_UPDATE_PS1, $t.Substring($i))" && powershell -NoProfile -ExecutionPolicy Bypass -File "%OOT_COOP_UPDATE_PS1%" -Kit "%~dp0." %* & (if errorlevel 1 (del "%OOT_COOP_UPDATE_PS1%" 2>nul & pause & exit /b 1)) & del "%OOT_COOP_UPDATE_PS1%" 2>nul & exit /b 0
