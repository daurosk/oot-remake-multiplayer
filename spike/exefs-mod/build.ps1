# SPDX-License-Identifier: GPL-3.0-or-later
# exefs spike: build subsdk9 (an NSO) with open LLVM (clang + ld.lld, aarch64-none-elf) and elf2nso.py.
#   powershell -File spike\exefs\build.ps1 [-Out <folder>] [-Llvm <LLVM bin>] [-Python <python.exe>]
# Out gets subsdk9 and, for an emulator's mod folder, exefs\subsdk9 under coop-exefs\ (load\<title id>\coop-exefs).
param(
    [string]$Out = (Join-Path $PSScriptRoot 'out'),
    [string]$Llvm = 'C:\Program Files\LLVM\bin',
    [string]$Python = (Join-Path $PSScriptRoot '..\..\..\tools-venv\Scripts\python.exe')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$clang = Join-Path $Llvm 'clang.exe'
$lld = Join-Path $Llvm 'ld.lld.exe'
$cflags = @('--target=aarch64-none-elf', '-march=armv8-a', '-O2', '-fPIC', '-ffreestanding', '-fno-builtin', '-nostdlib',
    '-mgeneral-regs-only', '-fvisibility=hidden', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
    '-fno-unwind-tables', '-Wall', '-Wextra', '-Werror')
& $clang @cflags -c (Join-Path $PSScriptRoot 'module.c') -o (Join-Path $Out 'module.o'); if ($LASTEXITCODE) { exit 1 }
& $clang --target=aarch64-none-elf -c (Join-Path $PSScriptRoot 'crt0.s') -o (Join-Path $Out 'crt0.o'); if ($LASTEXITCODE) { exit 1 }
& $lld -shared -z now -z max-page-size=0x1000 -z noexecstack --hash-style=sysv --no-undefined-version `
    -init=coop_init -T (Join-Path $PSScriptRoot 'link.ld') (Join-Path $Out 'crt0.o') (Join-Path $Out 'module.o') `
    -o (Join-Path $Out 'subsdk9.elf'); if ($LASTEXITCODE) { exit 1 }
& $Python -I (Join-Path $PSScriptRoot 'elf2nso.py') (Join-Path $Out 'subsdk9.elf') (Join-Path $Out 'subsdk9'); if ($LASTEXITCODE) { exit 1 }
$mod = Join-Path $Out 'coop-exefs\exefs'
New-Item -ItemType Directory -Force $mod | Out-Null
Copy-Item -Force (Join-Path $Out 'subsdk9') (Join-Path $mod 'subsdk9')
Write-Host "mod folder: $(Join-Path $Out 'coop-exefs') (copy it to <emulator user>\load\0400486024DEC000\)"
