@echo off
rem Runs a relay with join codes for hosts who cannot open a port (docs\play-online.md,
rem "Relay with join codes"). Only useful on a PC whose UDP port 27530 is reachable from the
rem internet (a forwarded port or a rented server). It forwards the games of the hosts that
rem use it and sees their traffic. Ctrl+C or closing this window stops it.
rem Usage: start-relay.cmd [--port N] [--relay-key-env NAME]
rem QuickEdit off (#239): a click in this window would pause the server's writes, and so the relay.
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { . '%~dp0scripts\console-mode.ps1'; [void](Disable-KitQuickEdit) } catch {}"
"%~dp0server\coop_server.exe" --relay %*
pause
