@echo off
rem Zips the emulator log, coop.ini, the server logs and VERSION.txt into
rem coop-logs-<pc>-<date>.zip in this folder, to send back.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\collect-logs.ps1" %*
pause
