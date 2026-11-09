@echo off
rem Starts the co-op server. Usage: start-server.cmd [port]   (default @PORT@)
rem The output is also written to logs\server-<date>.log. Ctrl+C stops it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-server.ps1" %*
