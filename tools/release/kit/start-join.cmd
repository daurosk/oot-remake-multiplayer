@echo off
rem Join.bat starts this for an invite code: the helper the game connects to on 127.0.0.1
rem (docs\play-online.md, "Host.bat and an invite code"). Keep its window open while you play.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-join.ps1" %*
