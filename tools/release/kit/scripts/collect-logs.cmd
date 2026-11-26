@echo off
rem Zips the game log, coop.ini, the server logs and VERSION.txt into coop-logs-<date>.zip in the kit folder.
rem Addresses and names are masked in it; attach it to /bugreport on the Squadala! Discord, not to a public issue.
rem First what Windows may block (windows-checks.ps1, run as text: it works where script files are refused). When
rem collect-logs.ps1 cannot run (Smart App Control, a policy), what windows-checks found is zipped by Windows' tar.
rem Says at once that it runs (issue #67: a blank window for minutes looked dead) and turns QuickEdit off (issue #68:
rem a click in the window paused it; console-mode.ps1).
echo Collecting your logs (this can take a few minutes)...
echo Checking what Windows may block...
set "OOT_COOP_COLLECT_SAID=1"
set "OOT_COOP_WC_KIT=%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -Command "$k=$env:OOT_COOP_WC_KIT; try { . (Join-Path $k 'scripts\console-mode.ps1'); [void](Disable-KitQuickEdit) } catch {}; iex (Get-Content -Raw -LiteralPath (Join-Path $k 'scripts\windows-checks.ps1')); exit 0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-logs.ps1" %*
if errorlevel 1 if exist "%~dp0..\logs\windows-checks.txt" (
  "%SystemRoot%\System32\tar.exe" -a -c -f "%~dp0..\coop-logs-windows-checks.zip" -C "%~dp0..\logs" windows-checks.txt
  echo The full log zip could not be made. Wrote coop-logs-windows-checks.zip in the kit folder instead: attach that to /bugreport.
)
pause
