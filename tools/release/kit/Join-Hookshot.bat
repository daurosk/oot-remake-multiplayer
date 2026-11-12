@echo off
rem Joins a game on Hookshot: as Join.bat, but starts the co-op Hookshot game that
rem Build-Hookshot-Coop.bat built instead of the emulator. All on one line: an update may
rem replace this file while it runs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" join -Hookshot %* & (if errorlevel 1 pause) & exit /b
