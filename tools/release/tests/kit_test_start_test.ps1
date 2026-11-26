# Tests for the kit's test-mode game start (tools/release/kit/scripts/play.ps1, Start-KitTestGame): the autotest's kit
# mode (OOT_COOP_TEST_KIT) starts the game without activating its window, so a test game opening never takes the focus
# from the window being worked in. The player's own start stays Start-Process.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\kit_test_start_test.ps1
$ErrorActionPreference = 'Stop'
$release = Split-Path -Parent $PSScriptRoot
$playText = [IO.File]::ReadAllText((Join-Path $release 'kit\scripts\play.ps1'))

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

# The function alone (play.ps1 runs a whole start when dot-sourced): from its comment to the next function's comment.
$m = [regex]::Match($playText, '(?s)function Start-KitTestGame.*?(?=\r?\n# Switches from coop-options\.ini)')
Check $m.Success 'play.ps1 has Start-KitTestGame'
Invoke-Expression $m.Value

# Wiring: kit mode uses it, the player's start does not.
Check ($playText -match '(?s)if \(\$testKit\) \{[^}]*Start-KitTestGame \$exe') 'kit mode starts the game with Start-KitTestGame'
Check ($playText -match '(?s)\} else \{\s*\$game = Start-Process -FilePath \$exe -WorkingDirectory') "the player's start is still Start-Process"
Check ($m.Value -match 'dwFlags = 1;' -and $m.Value -match 'wShowWindow = 4;') 'STARTF_USESHOWWINDOW with SW_SHOWNOACTIVATE'

if ($env:OS -eq 'Windows_NT') {
    # A real start: the id comes back. The program is one that ends by itself (where.exe with no argument prints its
    # usage and exits).
    $dir = Join-Path ([IO.Path]::GetTempPath()) ("kit-test-start-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $dir | Out-Null
    try {
        $prog = Join-Path $env:SystemRoot 'System32\where.exe'
        $id = Start-KitTestGame $prog $dir
        Check ($id -is [int] -and $id -gt 0 -and $id -ne $PID) 'a process id comes back'
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        $ended = (-not $p) -or $p.WaitForExit(10000)   # (no process left, or it ended within 10 s)
        Check $ended 'the started program ran and ended'
        $threw = $false
        try { [void](Start-KitTestGame (Join-Path $dir 'missing.exe') $dir) } catch { $threw = $true }
        Check $threw 'a program that cannot start is an error, not a silent 0'
    } finally {
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    }
} else {
    Write-Host 'skip the real start (not Windows)'
}

if ($failed) { Write-Host "$failed FAILED"; exit 1 }
Write-Host 'all passed'
