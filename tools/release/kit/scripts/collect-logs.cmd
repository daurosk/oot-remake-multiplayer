@echo off
rem Zips the game log, coop.ini, the server logs and VERSION.txt into coop-logs-<date>.zip in the kit folder.
rem Addresses and names are masked in it; send it privately (Discord DM), not on a public issue.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-logs.ps1" %*
pause
