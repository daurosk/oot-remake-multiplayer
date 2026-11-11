@echo off
rem Checks this PC for hosting: the server running, the firewall rule, who may join.
rem It only reads and prints what is wrong and the exact command to fix it; it changes nothing.
rem Run it while Host.bat's server window is open.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\check-hosting.ps1" %*
pause
