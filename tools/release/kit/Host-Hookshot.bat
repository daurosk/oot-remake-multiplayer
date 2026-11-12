@echo off
rem Hosts a game on Hookshot: as Host.bat, but starts the co-op Hookshot game that
rem Build-Hookshot-Coop.bat built instead of the emulator. All on one line: an update may
rem replace this file while it runs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" host -Hookshot %* & (if errorlevel 1 pause) & exit /b
