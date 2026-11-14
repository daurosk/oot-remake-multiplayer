@echo off
rem OoT co-op (Hookshot): double-click this. It checks everything, builds the game from your own game file the first
rem time (it asks for the file once), updates itself and starts the game. Host and Join are in the game's menu.
rem First what Windows itself may block (scripts\windows-checks.ps1, run as text so a policy that refuses script
rem files still lets it say so; the folder goes in OOT_COOP_WC_KIT, never into its code: a ' in the path). All on one line: an update may replace this file while it runs.
if not exist "%~dp0scripts\start.ps1" (echo This kit is still inside its zip file, or its unzip did not finish. Right-click the zip, choose "Extract All...", then start "Start OoT Co-op.bat" in the folder it made.& pause & exit /b 1) else set "OOT_COOP_WC_KIT=%~dp0" & powershell -NoProfile -ExecutionPolicy Bypass -Command "$k=$env:OOT_COOP_WC_KIT; iex (Get-Content -Raw -LiteralPath (Join-Path $k 'scripts\windows-checks.ps1')); exit 0" & (if errorlevel 1 (pause & exit /b 1)) & powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start.ps1" %* & (if errorlevel 1 (pause & exit /b 1)) & exit /b 0
