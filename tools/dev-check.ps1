# Build, run the unit tests and the content check, as CI does. Run from anywhere.
# Usage: powershell -File tools/dev-check.ps1 [-BuildDir build]
param([string]$BuildDir = "build")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)
function Step([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Write-Host "dev-check: failed: $exe $arguments"; exit $LASTEXITCODE }
}
$py = "python"
if (-not (Get-Command $py -ErrorAction SilentlyContinue)) { $py = "py" }
Step "cmake" @("-S", ".", "-B", $BuildDir, "-DCOOP_BUILD_TESTS=ON")
Step "cmake" @("--build", $BuildDir, "--config", "Release", "--parallel")
Step "ctest" @("--test-dir", $BuildDir, "-C", "Release", "--output-on-failure")
Step $py @("tools/check_content.py")
Write-Host "dev-check: all passed"
