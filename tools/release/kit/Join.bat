@echo off
rem Joins a game: asks for the host's address (Enter keeps the last one), then starts
rem the emulator connected to it. The first time it also asks for your player name.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" join %*
if errorlevel 1 pause
