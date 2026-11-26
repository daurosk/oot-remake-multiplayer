@echo off
rem NVIDIA DLSS and DLAA for the co-op game (NVIDIA RTX graphics cards). Longshot has DLSS only in a game built with
rem NVIDIA's DLSS SDK, which nobody may pass on: this asks you to accept NVIDIA's license once, then builds the game
rem again with the SDK it downloads (much shorter than the first build). Every later build of this kit keeps DLSS.
rem Then: in the game, Graphics, Upscaler: NVIDIA DLSS. Never share the game folder afterwards (NVIDIA's files).
rem Off again: "NVIDIA DLSS.bat" -NoDlss
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\hookshot-build.ps1" -Dlss %*
pause
