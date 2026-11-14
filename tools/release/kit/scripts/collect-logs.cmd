@echo off
rem Zips the game log, coop.ini, the server logs and VERSION.txt into coop-logs-<date>.zip in the kit folder.
rem Addresses and names are masked in it; send it privately (Discord DM), not on a public issue.
rem First what Windows may block (windows-checks.ps1, run as text: it works where script files are refused). When
rem collect-logs.ps1 cannot run (Smart App Control, a policy), what windows-checks found is zipped by Windows' tar.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$k='%~dp0..'; iex (Get-Content -Raw -LiteralPath (Join-Path $k 'scripts\windows-checks.ps1')); exit 0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-logs.ps1" %*
if errorlevel 1 if exist "%~dp0..\logs\windows-checks.txt" (
  "%SystemRoot%\System32\tar.exe" -a -c -f "%~dp0..\coop-logs-windows-checks.zip" -C "%~dp0..\logs" windows-checks.txt
  echo The full log zip could not be made. Wrote coop-logs-windows-checks.zip in the kit folder instead: send that.
)
pause
