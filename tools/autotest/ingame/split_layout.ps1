# Row 301's helper for test_split_fill.py: the kit's OWN local co-op window code (tools/release/kit/scripts/local-coop.ps1,
# dot-sourced, nothing copied) on the test display. Prints ONE JSON object on stdout and nothing else (C #132: every
# log line goes to stderr).
#   -Mode layout -Work l,t,r,b -Count N      the session's layout there: rects, DPI, frame, each window's client and
#                                            the game switches each game starts with (Get-KitLocalShapeEnv)
#   -Mode place -GamePid P -Rect x,y,w,h -Player N
#                                            game P's window to its place (Set-KitLocalWindowPlace), then what Windows
#                                            reports for it: window rect and client size, in physical pixels
param(
    [ValidateSet('layout', 'place')][string]$Mode = 'layout',
    [string]$Work = '',
    [int]$Count = 2,
    [int]$GamePid = 0,
    [string]$Rect = '',
    [int]$Player = 1,
    [string]$Kit = ''
)
$ErrorActionPreference = 'Stop'
function Out-Json($o) { [Console]::Out.WriteLine(($o | ConvertTo-Json -Depth 6 -Compress)) }
function Get-Ints([string]$t) { return , @($t -split ',' | ForEach-Object { [int]$_.Trim() }) }
try {
    if (-not $Kit) { $Kit = Join-Path $PSScriptRoot '..\..\release\kit\scripts\local-coop.ps1' }
    . $Kit
    Initialize-KitLocalWin32
    if ($Mode -eq 'layout') {
        $w = Get-Ints $Work
        if ($w.Count -ne 4) { throw "-Work needs l,t,r,b (got '$Work')" }
        $lay = Get-KitLocalLayout $Count $w
        $shapes = @()
        foreach ($c in $lay.Clients) { $shapes += , (Get-KitLocalShapeEnv $c $lay.Dpi) }
        Out-Json @{ ok = $true; work = $w; dpi = $lay.Dpi; frame = $lay.Frame; rects = $lay.Rects; clients = $lay.Clients; shapes = $shapes }
    } else {
        $r = Get-Ints $Rect
        if ($r.Count -ne 4 -or $GamePid -le 0) { throw "-Mode place needs -GamePid and -Rect x,y,w,h" }
        $h = [KitLocalWin32]::MainWindow([uint32]$GamePid)
        if ($h -eq [IntPtr]::Zero) { throw "game pid $GamePid has no window" }
        Set-KitLocalWindowPlace $h $r $Player 6>&1 | ForEach-Object { [Console]::Error.WriteLine("$_") }
        $wr = [KitLocalWin32]::Rect($h); $cr = [KitLocalWin32]::Client($h)
        Out-Json @{ ok = $true; window = $wr; client = @(($cr[2] - $cr[0]), ($cr[3] - $cr[1])) }
    }
} catch {
    Out-Json @{ ok = $false; error = "$($_.Exception.Message)" }
    exit 1
}
