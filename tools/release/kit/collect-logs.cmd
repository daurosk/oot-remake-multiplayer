@echo off
rem Zips the emulator log, coop.ini, the server logs and VERSION.txt into
rem coop-logs-<date>.zip in this folder. Addresses and names are masked in it;
rem send it privately (Discord DM), not on a public issue.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\collect-logs.ps1" %*
pause
