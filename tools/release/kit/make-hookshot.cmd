@echo off
rem Experimental: builds Hookshot (a PC port of the game) with co-op from your own game file,
rem into the hookshot folder. Needs a mesh-shader graphics card and about 60 GB free.
rem Then play with Host-Hookshot.bat or Join-Hookshot.bat. SETUP.md, "On Hookshot".
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\hookshot-build.ps1" %*
pause
