@echo off
rem Builds a co-op Hookshot game on this PC from your own game files (Hookshot: github.com/NotRexed/Hookshot,
rem the static recompilation; it needs a graphics card with mesh shaders). First run: 10 to 45 minutes and
rem about 60 GB free. After Update.bat, run it again: it only reinstalls co-op unless a new build is needed.
rem Options: -Game <your .nup>, -Out <game folder>, -Language en-US, -Rebuild (SETUP.md, "Play on Hookshot").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\hookshot-build.ps1" %*
pause
