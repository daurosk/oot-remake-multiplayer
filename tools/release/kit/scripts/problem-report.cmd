@echo off
rem Makes a problem report now: the co-op problems in your game logs since the last report, masked as collect-logs
rem masks them, in logs\coop-report-<date>.zip. Send it with /bugreport in the Squadala! Discord.
rem "problem-report.cmd on" (or off) turns the report at each start on or off instead; off also stops sending.
rem "problem-report.cmd send" (or nosend) turns sending them to the Squadala! team after a game with a problem on or off.
rem Nothing is sent from here (docs\problem-reports.md, "Sending").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0problem-report.ps1" -Now %*
pause
