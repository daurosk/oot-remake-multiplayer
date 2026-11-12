@echo off
rem Experimental: hosts a game on Hookshot (made by make-hookshot.cmd): starts the co-op server
rem from this kit in its own window, then Hookshot connected to it. Emulator and Hookshot players
rem can be in one room. All on one line: an update may replace this file while it runs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" host -Hookshot %* & (if errorlevel 1 pause) & exit /b
