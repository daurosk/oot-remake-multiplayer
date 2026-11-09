@echo off
rem Hosts a game: starts the co-op server from this kit in its own window, then the
rem emulator connected to it. The first time it asks for your player name.
rem Other players use Join.bat with this PC's address (SETUP.md).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" host %*
if errorlevel 1 pause
