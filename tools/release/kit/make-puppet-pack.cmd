@echo off
rem Makes the puppet pack (how you see the other players) from your own game file.
rem Longshot kit ("Start Squadala.bat"): in the co-op game this kit built (hookshot-build.ps1 -PackOnly).
rem Emulator kit: into emulator\user, from the romfs or .nup (SETUP.md says how).
if exist "%~dp0Start Squadala.bat" (powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\hookshot-build.ps1" -PackOnly %*) else (powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\puppet-pack.ps1" %*)
pause
