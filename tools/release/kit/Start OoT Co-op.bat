@echo off
rem OoT co-op (Hookshot): double-click this. It checks everything, builds the game from your own game file the first
rem time (it asks for the file once), updates itself and starts the game. Host and Join are in the game's menu.
rem All on one line: an update may replace this file while it runs.
if not exist "%~dp0scripts\start.ps1" (echo This kit is still inside its zip file, or its unzip did not finish. Right-click the zip, choose "Extract All...", then start "Start OoT Co-op.bat" in the folder it made.& pause & exit /b 1) else powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start.ps1" %* & (if errorlevel 1 pause) & exit /b
