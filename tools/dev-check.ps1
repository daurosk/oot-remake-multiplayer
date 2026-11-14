# Build, run the unit tests and the content check, as CI does. Run from anywhere.
# Usage: powershell -File tools/dev-check.ps1 [-BuildDir build]
param([string]$BuildDir = "build")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)
function Step([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Write-Host "dev-check: failed: $exe $arguments (needs CMake 3.24+, a C++20 compiler and Python 3.10+)"; exit $LASTEXITCODE }
}
# The Python launcher first: "python" can be the Windows Store stub, which is found but does not run.
$py = "py"
if (-not (Get-Command $py -ErrorAction SilentlyContinue)) { $py = "python" }
Step "cmake" @("-S", ".", "-B", $BuildDir, "-DCOOP_BUILD_TESTS=ON")
Step "cmake" @("--build", $BuildDir, "--config", "Release", "--parallel")
Step "ctest" @("--test-dir", $BuildDir, "-C", "Release", "--output-on-failure")
Step $py @("tools/check_content.py")
Write-Host "dev-check: all passed"
