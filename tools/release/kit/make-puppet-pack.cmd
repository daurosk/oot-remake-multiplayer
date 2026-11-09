@echo off
rem Builds the puppet Link packs from this PC's own game romfs into emulator\user.
rem Once per PC (again after a new kit). TESTING.md says how to dump the romfs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\puppet-pack.ps1" %*
pause
