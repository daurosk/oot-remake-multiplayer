# Tests for the kit putting its Nucleus Co-op handler over Nucleus's copy (Update-KitNucleusHandler, kit-paths.ps1).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\nucleus_handler_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\kit-paths.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-nucleus-test-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    $kit = Join-Path $tmp 'kit'; $game = Join-Path $tmp 'game'
    $nuc = Join-Path $tmp 'Nucleus'; $other = Join-Path $tmp 'Other Nucleus'; $bare = Join-Path $tmp 'Bare Nucleus'
    foreach ($d in @("$kit\nucleus", "$game\nucleus-players", "$nuc\handlers", "$nuc\content\OoTCoopLongshot\Instance0",
                     "$other\handlers", "$bare\handlers")) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    Set-Content -LiteralPath "$kit\nucleus\OoTCoop.js" -Value 'new handler' -NoNewline
    Set-Content -LiteralPath "$nuc\handlers\OoTCoop.js" -Value 'old handler' -NoNewline
    Set-Content -LiteralPath "$other\handlers\OoTCoop.js" -Value 'new handler' -NoNewline
    Set-Content -LiteralPath "$game\nucleus-players\instances.txt" -Value "Navi=$nuc\content\OoTCoopLongshot\Instance0`r`nbroken line`r`n"

    $done = @(Update-KitNucleusHandler $kit $game @($other, $bare))
    Check ((Get-Content -Raw -LiteralPath "$nuc\handlers\OoTCoop.js") -eq 'new handler') 'the Nucleus of a noted instance gets the kit''s handler'
    Check ($done.Count -eq 1 -and $done[0] -eq "$nuc\handlers\OoTCoop.js") 'only the changed copy is reported'
    Check (-not (Test-Path -LiteralPath "$bare\handlers\OoTCoop.js")) 'a Nucleus without the handler is left alone'
    Check (@(Update-KitNucleusHandler $kit $game @($other)).Count -eq 0) 'a second run changes nothing'
    Check (@(Update-KitNucleusHandler (Join-Path $tmp 'nokit') $game @()).Count -eq 0) 'a kit without the handler changes nothing'
} finally {
    Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
