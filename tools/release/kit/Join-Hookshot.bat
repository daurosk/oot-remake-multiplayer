@echo off
rem Experimental: joins a game on Hookshot (made by make-hookshot.cmd): asks for the host's
rem address, then starts Hookshot connected to it. All on one line: an update may replace this
rem file while it runs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" join -Hookshot %* & (if errorlevel 1 pause) & exit /b
