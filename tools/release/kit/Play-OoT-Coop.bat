@echo off
rem OoT co-op on Hookshot: host or join (it asks), with the updates and the game build done by itself.
rem The first run builds the Hookshot game from your own game (it asks for your .nup once).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\play.ps1" choose -Hookshot %* & (if errorlevel 1 pause) & exit /b
