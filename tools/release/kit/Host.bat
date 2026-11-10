@echo off
rem Hosts a game: starts the co-op server from this kit in its own window, then the
rem emulator connected to it. The first time it asks for your player name.
rem Other players use Join.bat with this PC's address (SETUP.md).
rem All on one line: an update may replace this file while it runs, and cmd reads a
rem .bat file line by line as it goes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" host %* & (if errorlevel 1 pause) & exit /b
