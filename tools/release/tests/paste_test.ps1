# Tests for the kit's path questions (tools/release/kit/scripts/console-mode.ps1; player report 87: a pasted game
# path did not arrive). The keys a paste or a drag turns into, Ctrl+V / Shift+Insert reaching the script, control
# characters, the quotes a drag or 'Copy as path' adds, and a whole question with its input redirected.
# No console is needed (CI has none).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\paste_test.ps1
$ErrorActionPreference = 'Stop'
$release = Split-Path -Parent $PSScriptRoot
$consoleMode = Join-Path $release 'kit\scripts\console-mode.ps1'
. $consoleMode

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
$none = [ConsoleModifiers]0
$clip = { 'C:\from clip\game.nup' }
# Feeds keys as Read-TimedLine does: @(Key, Char, Modifiers) each; the line when Enter came, else $null.
function Send-Keys([object[]]$Keys, [scriptblock]$Paste) {
    $text = ''
    foreach ($k in $Keys) {
        $r = Step-KitLineKey $text $k[0] $k[1] $k[2] $Paste
        $text = $r[0]
        if ($r[2]) { return $text }
    }
    return $null
}
# A string as the keys a paste makes (conhost sends each character with a key of its own; the character is what counts).
function Get-TypedKeys([string]$s) {
    $keys = @()
    foreach ($c in $s.ToCharArray()) {
        $key = [ConsoleKey]::Oem1
        if ($c -match '[A-Za-z]') { $key = [ConsoleKey]"$([char]::ToUpperInvariant($c))" }
        $mods = if ([char]::IsUpper($c) -or $c -eq ':') { [ConsoleModifiers]::Shift } else { $none }
        $keys += , @($key, $c, $mods)
    }
    return , $keys
}
$enter = @(, @([ConsoleKey]::Enter, [char]13, $none))

# --- Keys of a paste or a drag (what conhost and Windows Terminal hand a script for Ctrl+V, right-click, a drop)
$path = 'C:\Users\Player\My Games\OoT ' + [char]0xE9 + ' [EU]\game.nup'
Check ((Send-Keys ((Get-TypedKeys $path) + $enter) $clip) -ceq $path) 'a pasted path with spaces, brackets and an accent arrives whole'
Check ((Send-Keys ((Get-TypedKeys "`"$path`"") + $enter) $clip) -ceq "`"$path`"") 'a dragged path keeps its quotes in the line (taken off later)'
$bs = @(, @([ConsoleKey]::Backspace, [char]8, $none))
Check ((Send-Keys ((Get-TypedKeys 'C:\ab') + $bs + (Get-TypedKeys 'c') + $enter) $clip) -ceq 'C:\ac') 'Backspace takes one character back'
Check ((Send-Keys ($bs + $enter) $clip) -ceq '') 'Backspace on an empty line does nothing'
Check ($null -eq (Send-Keys (Get-TypedKeys 'C:\x') $clip)) 'no Enter: the line is not done'
$r = Step-KitLineKey 'C:' ([ConsoleKey]::Oem5) ([char]'\') $none $null
Check ($r[0] -ceq 'C:\' -and $r[1] -ceq '\' -and -not $r[2]) 'a key echoes its character'

# --- Ctrl+V / Shift+Insert reaching the script (legacy console, Ctrl key shortcuts off): pasted by the script
$ctrlV = @(, @([ConsoleKey]::V, [char]0x16, [ConsoleModifiers]::Control))
Check ((Send-Keys ($ctrlV + $enter) $clip) -ceq 'C:\from clip\game.nup') 'Ctrl+V as a key pastes the clipboard'
$shIns = @(, @([ConsoleKey]::Insert, [char]0, [ConsoleModifiers]::Shift))
Check ((Send-Keys ($shIns + $enter) $clip) -ceq 'C:\from clip\game.nup') 'Shift+Insert as a key pastes the clipboard'
Check ((Send-Keys ($ctrlV + $enter) { "C:\two\lines.nup`r`nsecond line" }) -ceq 'C:\two\lines.nup') 'a pasted clipboard gives its first line only'
Check ((Send-Keys ($ctrlV + $enter) { "C:\tab`there.nup" }) -ceq 'C:\tabhere.nup') 'control characters in a clipboard are dropped'
Check ((Send-Keys ($ctrlV + $enter) $null) -ceq '') 'Ctrl+V with no paste source adds nothing'
$altGrV = @(, @([ConsoleKey]::V, [char]'@', ([ConsoleModifiers]::Control -bor [ConsoleModifiers]::Alt)))
Check ((Send-Keys ($altGrV + $enter) $clip) -ceq '@') 'AltGr+V (Ctrl+Alt) types its character, no paste'

# --- Control characters never go in
foreach ($c in @(@([ConsoleKey]::Escape, [char]27), @([ConsoleKey]::Tab, [char]9), @([ConsoleKey]::V, [char]0x16))) {
    $r = Step-KitLineKey 'C:' $c[0] $c[1] $none $null
    Check ($r[0] -ceq 'C:' -and -not $r[1]) "control character $([int]$c[1]) is not added"
}
$r = Step-KitLineKey 'C:' ([ConsoleKey]::F5) ([char]0) $none $null
Check ($r[0] -ceq 'C:') 'a key without a character adds nothing'

# --- The answer as a path
Check ((ConvertTo-KitAnswerPath "  `"$path`"  ") -ceq $path) 'quotes and spaces around a dragged / copied path go'
Check ((ConvertTo-KitAnswerPath "'C:\a b\x.nup'") -ceq 'C:\a b\x.nup') 'single quotes around a path go'
Check ((ConvertTo-KitAnswerPath "C:\it's\x.nup") -ceq "C:\it's\x.nup") 'an apostrophe inside a path stays'
Check ((ConvertTo-KitAnswerPath '') -ceq '') 'an empty answer stays empty'

# --- The game files: every refusal says why (player report 87: refused with no reason, asked again at every start)
$g = Join-Path ([IO.Path]::GetTempPath()) "kit-paste-game-$PID"
New-Item -ItemType Directory -Force -Path $g | Out-Null
function New-Sized([string]$p, [long]$n) { $fs = [IO.File]::Create($p); $fs.SetLength($n); $fs.Close() }
try {
    $nup = Join-Path $g ('My Games ' + [char]0xE9 + '\OoT [EU].nup')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $nup) | Out-Null
    New-Sized $nup 2MB
    $a = Resolve-KitGameAnswer "`"$nup`""
    Check ($a.Path -ceq $nup -and -not $a.Why) 'a .nup with spaces, an accent and brackets, in quotes: taken'
    $a = Resolve-KitGameAnswer ''
    Check (-not $a.Path -and $a.Why -match 'nothing arrived' -and $a.Why -match 'Copy as path') 'an empty answer: says nothing arrived and how to paste'
    $a = Resolve-KitGameAnswer (Join-Path $g 'gone.nup')
    Check (-not $a.Path -and $a.Why -match 'finds nothing at' -and $a.Why -match 'plugged in') 'a missing file: says Windows finds nothing there'
    $a = Resolve-KitGameAnswer 'OoT.nup'
    Check ($a.Why -match 'not a whole path' -or $a.Why -match 'finds nothing') 'a bare file name: says it is not a whole path'
    $nsp = Join-Path $g 'game.nsp'; New-Sized $nsp 2MB
    $a = Resolve-KitGameAnswer $nsp
    Check (-not $a.Path -and $a.Why -match '\.nsp file' -and $a.Why -match '\.nup') 'a .nsp: says it is a .nsp and a .nup is needed'
    $txt = Join-Path $g 'notes.txt'; New-Sized $txt 10
    $a = Resolve-KitGameAnswer $txt
    Check (-not $a.Path -and $a.Why -match 'not a \.nup file \(it ends in \.txt\)') 'another file: says its type'
    $tiny = Join-Path $g 'tiny.nup'; New-Sized $tiny 100
    $a = Resolve-KitGameAnswer $tiny
    Check (-not $a.Path -and $a.Why -match 'not a whole game') 'a tiny .nup: says it is not a whole game'
    Remove-Item -LiteralPath $tiny, $txt -Force
    $a = Resolve-KitGameAnswer (Split-Path -Parent $nup)
    Check ($a.Path -ceq $nup -and $a.Note -match 'Took the \.nup') 'a folder with one .nup: that .nup, said'
    $two = Join-Path $g 'two'; New-Item -ItemType Directory -Force -Path $two | Out-Null
    New-Sized (Join-Path $two 'a.nup') 2MB; New-Sized (Join-Path $two 'b.nup') 2MB
    $a = Resolve-KitGameAnswer $two
    Check (-not $a.Path -and $a.Why -match 'holds 2 \.nup files') 'a folder with two .nup files: says so'
    $nspDir = Join-Path $g 'nspdir'; New-Item -ItemType Directory -Force -Path $nspDir | Out-Null
    New-Sized (Join-Path $nspDir 'game.xci') 2MB
    $a = Resolve-KitGameAnswer $nspDir
    Check (-not $a.Path -and $a.Why -match 'game\.xci, which is not a \.nup') 'a folder with an .xci: says it is no .nup'
    $romfs = Join-Path $g 'romfs'; New-Item -ItemType Directory -Force -Path (Join-Path $romfs 'Pack') | Out-Null
    $a = Resolve-KitGameAnswer $romfs
    Check ($a.Path -ceq $romfs -and -not $a.Why) 'a romfs folder: taken (its exefs is asked next)'
    $cloud = Join-Path $g 'cloud.nup'; New-Sized $cloud 2MB
    [IO.File]::SetAttributes($cloud, [IO.FileAttributes]::Offline)
    $a = Resolve-KitGameAnswer $cloud
    Check (-not $a.Path -and $a.Why -match 'OneDrive') 'a cloud-only .nup: says to keep it on this device'
    [IO.File]::SetAttributes($cloud, [IO.FileAttributes]::Normal)
} finally { Remove-Item -LiteralPath $g -Recurse -Force -ErrorAction SilentlyContinue }

# --- Keys typed ahead (the Enter left after a one-key Y/n question) are no answer
Check ((Clear-KitTypedAhead) -is [int]) 'Clear-KitTypedAhead never throws'
$cm = [IO.File]::ReadAllText($consoleMode)
$rtl = $cm.Substring($cm.IndexOf('function Read-TimedLine'))
Check ($rtl.IndexOf('Clear-KitTypedAhead') -gt 0 -and $rtl.IndexOf('Clear-KitTypedAhead') -lt $rtl.IndexOf('ReadKey')) 'Read-TimedLine clears keys typed ahead before it reads'
Check ($rtl.Contains('[void](Clear-KitTypedAhead)')) 'the count of cleared keys does not leak into the answer (a real-console run gave "1 C:\...")'
$play = [IO.File]::ReadAllText((Join-Path $release 'kit\scripts\play.ps1'))
Check ($play -match "Write-Host 'y'; Clear-KeysTypedAhead; return \`$true" -and $play -match "Write-Host 'n'; Clear-KeysTypedAhead; return \`$false") 'play.ps1 Read-YesNo takes out the Enter after its one key'

# --- Whole questions with the input redirected (as CI, a pipe or a test runs them)
$ps = (Get-Process -Id $PID).Path
$tmp = Join-Path ([IO.Path]::GetTempPath()) "kit-paste-test-$PID"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    $ask = Join-Path $tmp 'ask.ps1'
    $body = ". '$consoleMode'`n" + '$a = Read-KitPath 5 { param($p) $p -like ''*.nup'' }' + "`n" + '[Console]::Out.Write("[$a]")' + "`n"
    [IO.File]::WriteAllText($ask, $body)
    $out = 'C:\a b\x.nup' | & $ps -NoProfile -ExecutionPolicy Bypass -File $ask
    Check ("$out".EndsWith('[C:\a b\x.nup]')) "a redirected answer arrives [$out]"
    $out = '"C:\a b\x.nup"' | & $ps -NoProfile -ExecutionPolicy Bypass -File $ask
    Check ("$out".EndsWith('[C:\a b\x.nup]')) "a redirected answer loses its quotes [$out]"
    $out = '' | & $ps -NoProfile -ExecutionPolicy Bypass -File $ask
    Check ("$out".EndsWith('[]')) "an empty redirected answer: empty (never the clipboard) [$out]"
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }

# --- Clipboard and QuickEdit never throw (CI: no clipboard, no console window)
Check ((Get-KitClipboardPath) -is [string]) 'Get-KitClipboardPath gives a string'
Check ((Get-KitClipboardFile) -is [string]) 'Get-KitClipboardFile gives a string'
$cmd = ". '$consoleMode'; " + '$a = Set-KitQuickEdit $true; $b = Set-KitQuickEdit $false; $c = Set-KitQuickEdit $false; [string]::Join(''/'', @($a, $b, $c))'
$qe = & $ps -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1
Check ($LASTEXITCODE -eq 0 -and ("$qe" -in '//', 'True/True/False', 'False/True/False')) "Set-KitQuickEdit: no throw, on then off reads back [$qe]"

# --- hookshot-build.ps1 asks through Read-KitPath, with the paste hint
$hb = [IO.File]::ReadAllText((Join-Path $release 'kit\scripts\hookshot-build.ps1'))
Check (-not $hb.Contains('function Read-TimedLine') -and $hb.Contains("'console-mode.ps1'")) 'hookshot-build.ps1 uses console-mode.ps1''s questions'
Check (-not ($hb -match 'Read-Host\)')) 'hookshot-build.ps1 asks no path with a bare Read-Host'
Check (([regex]::Matches($hb, 'Read-KitPath \d+ \{')).Count -eq 4) 'all four path questions use Read-KitPath'
Check ($hb.Contains('Resolve-KitGameAnswer') -and $hb.Contains('Save-KitGameRejected') -and $hb.Contains("Find-KitGameNup '' `$kit")) 'hookshot-build.ps1 says why game files are refused, remembers it, looks in the usual places'
Check (-not $hb.Contains("Fail 'No game files given")) 'an empty game answer is asked again, not a stop'
$fail = $hb.Substring($hb.IndexOf('Save-KitGameRejected $Game $why'))
$fail = $fail.Substring(0, $fail.IndexOf('Fail "'))
Check ($fail.Contains('Remove-Item -LiteralPath $remembered')) 'a check refusal forgets the remembered game file (no loop without a question)'
Check ($hb -match '\$refused -ieq \$remembered') 'a remembered file refused before is not taken again'
Check ($cm.Contains('if ($redirected -or $script:KitLineTimedOut) { return '''' }')) 'no clipboard file on redirected input or a timed-out question'
Check (([regex]::Matches($hb, 'Ctrl\+V or right-click')).Count -ge 4) 'every path question says Ctrl+V or right-click, or drag'

if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all paste checks passed'
