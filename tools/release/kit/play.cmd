@echo off
rem Asks for the server address and your name the first time, writes
rem emulator\user\config\coop.ini and starts the emulator.
rem "play.cmd change" asks again.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" %*
if errorlevel 1 pause
