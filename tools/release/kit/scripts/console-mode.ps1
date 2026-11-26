# --- The kit's path questions (player report 87, 2026-11-15: a pasted game path did not arrive) ----------------------
# A paste reaches Read-TimedLine as keys in both consoles (tested: Ctrl+V and Shift+Insert in conhost, Ctrl+V in
# Windows Terminal, a path with spaces, quotes and an accent, a trailing new line). What lost a path was around that:
# right-click pastes only with QuickEdit on, which the kit turns off (#68), so it is on while a question waits;
# a file copied in Explorer (Ctrl+C on it) holds no text, so the console pastes nothing: an empty answer takes such a
# copied file when it fits; the legacy console (or Ctrl key shortcuts off) hands Ctrl+V to us as a key: we paste.
# QuickEdit on (or off) in this console window; what it was before ($null when it could not be read or changed).
# (Disable-KitQuickEdit below stays as it is: update.ps1 carries a copy of it.)
function Set-KitQuickEdit([bool]$On) {
    try {
        if (-not $script:KitConsoleNative) {
            $name = New-Object System.Reflection.AssemblyName 'OotCoopConsoleQE'
            $asm = [System.Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, 'Run')
            $type = $asm.DefineDynamicModule('OotCoopConsoleQE').DefineType('OotCoopConsoleQE', 'Public, Class')
            foreach ($m in @(@('CreateFileW', [IntPtr], @([string], [uint32], [uint32], [IntPtr], [uint32], [uint32], [IntPtr])),
                    @('GetConsoleMode', [bool], @([IntPtr], [uint32].MakeByRefType())),
                    @('SetConsoleMode', [bool], @([IntPtr], [uint32])),
                    @('CloseHandle', [bool], @([IntPtr])))) {
                $type.DefinePInvokeMethod($m[0], 'kernel32.dll', 'Public, Static, PinvokeImpl', 'Standard', $m[1],
                    [Type[]]$m[2], 'Winapi', 'Unicode').SetImplementationFlags('PreserveSig')
            }
            $script:KitConsoleNative = $type.CreateType()
        }
        $native = $script:KitConsoleNative
        $in = $native::CreateFileW('CONIN$', [uint32]3221225472, [uint32]3, [IntPtr]::Zero, [uint32]3, [uint32]0, [IntPtr]::Zero)
        if ($in -eq [IntPtr]::Zero -or $in -eq [IntPtr]-1) { return $null }
        try {
            $mode = [uint32]0
            if (-not $native::GetConsoleMode($in, [ref]$mode)) { return $null }
            $was = [bool]($mode -band 0x40)
            $new = if ($On) { [uint32]($mode -bor 0xC0) } else { [uint32](($mode -band 0xFFFFFFBF) -bor 0x80) }
            if (-not $native::SetConsoleMode($in, $new)) { return $null }
            return $was
        } finally { [void]$native::CloseHandle($in) }
    } catch { return $null }
}

# What the clipboard holds as one path: its first line of text, else the one file or folder copied in Explorer.
# '' when neither, or no clipboard (PowerShell 7 has no -Format; a session with no desktop has no clipboard).
function Get-KitClipboardPath {
    $text = ''
    try { $text = "$(Get-Clipboard -Format Text -Raw -ErrorAction Stop)" } catch { }
    $line = @($text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    if ($line) { return "$($line[0])".Trim() }
    return (Get-KitClipboardFile)
}
function Get-KitClipboardFile {
    try {
        $files = @(Get-Clipboard -Format FileDropList -ErrorAction Stop)
        if ($files.Count -eq 1) { return "$($files[0].FullName)" }
    } catch { }
    return ''
}

# One key of a typed line: @(the new line, what to echo, the line done). Enter ends it, Backspace takes one back;
# Ctrl+V or Shift+Insert that reach us (the console pastes them itself unless it is the legacy one or its Ctrl key
# shortcuts are off) paste with $Paste; control characters (^V, Esc, Tab, a paste's line breaks) never go in.
function Step-KitLineKey([string]$Text, [ConsoleKey]$Key, [char]$Char, [ConsoleModifiers]$Mods, [scriptblock]$Paste) {
    if ($Key -eq [ConsoleKey]::Enter) { return @($Text, '', $true) }
    if ($Key -eq [ConsoleKey]::Backspace) {
        if ($Text) { return @($Text.Substring(0, $Text.Length - 1), "`b `b", $false) }
        return @($Text, '', $false)
    }
    $ctrl = ($Mods -band [ConsoleModifiers]::Control) -and -not ($Mods -band [ConsoleModifiers]::Alt)   # (AltGr = Ctrl+Alt types)
    $add = ''
    if (($Key -eq [ConsoleKey]::V -and $ctrl) -or ($Key -eq [ConsoleKey]::Insert -and ($Mods -band [ConsoleModifiers]::Shift))) {
        if ($Paste) { $add = @("$(& $Paste)" -split "`r?`n")[0] }
    } elseif ([int]$Char -ne 0) { $add = "$Char" }
    $add = -join @($add.ToCharArray() | Where-Object { -not [char]::IsControl($_) })
    return @(($Text + $add), $add, $false)
}

# Keys waiting in the window (typed ahead, an Enter left by a one-key question) taken out. Never fails.
function Clear-KitTypedAhead {
    try {
        if ([Console]::IsInputRedirected) { return 0 }
        $n = 0
        while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true); $n++ }
        return $n
    } catch { return 0 }
}

# A line typed, pasted or dragged in. Nothing typed for $Seconds: '' (0 or less: no limit). Once something is typed
# the limit is off. Input redirected (tests, CI): one line read. QuickEdit is on while it waits (right-click pastes).
function Read-TimedLine([int]$Seconds) {
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected } catch { }
    $script:KitLineTimedOut = $false
    if ($redirected) { return "$([Console]::In.ReadLine())" }
    # Keys typed before the question are not its answer: the Enter after a one-key Y/n question stayed in the window
    # and ended the next question empty (player report 87: 'No game files given' at every start).
    [void](Clear-KitTypedAhead)
    $qe = Set-KitQuickEdit $true
    try {
        $end = (Get-Date).AddSeconds($Seconds)
        $text = ''
        $paste = { Get-KitClipboardPath }
        while ($text -or $Seconds -le 0 -or (Get-Date) -lt $end) {
            if ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)
                $r = Step-KitLineKey $text $key.Key $key.KeyChar $key.Modifiers $paste
                $text = $r[0]
                if ($r[1]) { Write-Host -NoNewline $r[1] }
                if ($r[2]) { Write-Host ''; return $text }
            } else { Start-Sleep -Milliseconds 50 }
        }
        Write-Host ''
        Write-Host "  (Nothing typed in $Seconds seconds: going on without it.)"
        $script:KitLineTimedOut = $true
        return ''
    } finally { if ($qe -eq $false) { [void](Set-KitQuickEdit $false) } }
}

# The answer as a path: spaces and the quotes a drag or 'Copy as path' adds taken off. An empty answer with one fitting
# file or folder copied in Explorer (Ctrl+C on it: the console pastes no text from that) takes that one, and says so.
function ConvertTo-KitAnswerPath([string]$Answer) {
    $p = "$Answer".Trim()
    if ($p.Length -ge 2 -and (($p[0] -eq '"' -and $p[-1] -eq '"') -or ($p[0] -eq "'" -and $p[-1] -eq "'"))) { $p = $p.Substring(1, $p.Length - 2).Trim() }
    return $p.Trim('"').Trim()
}
# The game files given at the game question (or with -Game, or remembered): @{ Path; Why; Note }. Why is set when they
# cannot be taken, and says why and what to give instead (every refusal is said: a player saw the question come back
# with no reason, start after start). A folder holding one .nup gives that .nup (Note says so).
function Resolve-KitGameAnswer([string]$Answer) {
    $p = ConvertTo-KitAnswerPath $Answer
    $how = 'In Explorer: Shift+right-click the file > Copy as path, then paste it here with Ctrl+V or right-click; or drag the file into this window.'
    if (-not $p) { return @{ Path = ''; Why = "nothing arrived (the paste or drag did not reach this window). $how"; Note = '' } }
    $exists = $false
    try { $exists = Test-Path -LiteralPath $p } catch { }
    if (-not $exists) {
        $hint = if ($p -notmatch '^([A-Za-z]:\\|\\\\)') { ' It does not start with a drive (like C:\), so it is not a whole path.' } else { ' Check the drive is plugged in and the file was not moved or renamed.' }
        return @{ Path = ''; Why = "Windows finds nothing at $p.$hint $how"; Note = '' }
    }
    try { $item = Get-Item -LiteralPath $p -Force -ErrorAction Stop }
    catch { return @{ Path = ''; Why = "Windows cannot open $p ($($_.Exception.Message)). Move the file to a folder of yours (such as Downloads) and give it again."; Note = '' } }
    $note = ''
    if ($item.PSIsContainer) {
        $nups = @(Get-ChildItem -LiteralPath $item.FullName -File -Filter '*.nup' -ErrorAction SilentlyContinue)
        if ($nups.Count -gt 1) { return @{ Path = ''; Why = "the folder $($item.FullName) holds $($nups.Count) .nup files: give the one file of the game (program 0400486024DEC000)."; Note = '' } }
        if ($nups.Count -eq 1) { $item = $nups[0]; $note = "Took the .nup in that folder: $($item.FullName)" }
        else {
            $other = @(Get-ChildItem -LiteralPath $item.FullName -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.nsp', '.xci', '.nsz', '.xcz' })
            if ($other.Count -and -not (Test-Path -LiteralPath (Join-Path $item.FullName 'Pack'))) {
                return @{ Path = ''; Why = "the folder $($item.FullName) holds $($other[0].Name), which is not a .nup. This build needs your game as a .nup file, or its romfs and exefs folders (docs/setup-emulator.md, 'Which file do you have?')."; Note = '' }
            }
            return @{ Path = $item.FullName; Why = ''; Note = '' }   # (a romfs folder: its exefs is asked for next)
        }
    }
    $ext = $item.Extension.ToLowerInvariant()
    if ($ext -in '.nsp', '.xci', '.nsz', '.xcz') {
        return @{ Path = ''; Why = "$($item.Name) is a $ext file. This build needs your game as a .nup file, or its romfs and exefs folders (docs/setup-emulator.md, 'Which file do you have?')."; Note = '' }
    }
    if ($ext -ne '.nup') {
        $kind = if ($ext) { "it ends in $ext" } else { 'it has no file type' }
        return @{ Path = ''; Why = "$($item.Name) is not a .nup file ($kind). Give your game's .nup file, or its romfs folder."; Note = '' }
    }
    # (OneDrive and other cloud folders: a file shown in Explorer but not on this PC yet cannot be read by the build)
    if ([int]$item.Attributes -band 0x441000) {
        return @{ Path = ''; Why = "$($item.Name) is in a cloud folder (OneDrive) and not on this PC yet: right-click it > Always keep on this device, wait for it to download, then give it again."; Note = '' }
    }
    if ($item.Length -lt 1MB) { return @{ Path = ''; Why = "$($item.Name) is only $($item.Length) bytes: not a whole game. Give the full .nup file."; Note = '' } }
    return @{ Path = $item.FullName; Why = ''; Note = $note }
}

function Read-KitPath([int]$Seconds, [scriptblock]$Fits) {
    $p = ConvertTo-KitAnswerPath (Read-TimedLine $Seconds)
    if ($p) { return $p }
    # (only for Enter pressed by someone at the window: never for input from a file or a question that timed out)
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected } catch { }
    if ($redirected -or $script:KitLineTimedOut) { return '' }
    $copied = Get-KitClipboardFile
    if ($copied -and $Fits -and (& $Fits $copied)) { Write-Host "  Taken from what you copied: $copied"; return $copied }
    return ''
}

# Turns QuickEdit off in the kit's own console window (issue #68). With QuickEdit on, one click in the window starts
# a text selection, and Windows then pauses every program that writes to that window until Enter or Esc: players
# saw the start, the update and collect-logs 'stuck'. The setting belongs to the window, so it stays off for the
# rest of the .bat (and ends with the window); the player's other console windows are not changed.
# The window's input is opened as CONIN$ (not the standard input, which a start may have redirected), so it works
# however PowerShell was started. No Add-Type (it runs the C# compiler and writes a DLL, which Smart App Control
# may block): the kernel32 calls are declared in memory with Reflection.Emit. Never fails the caller: no console
# window, Constrained Language mode, PowerShell 7 or any other error just leaves the window as it is ($false).
# update.ps1 carries its own copy of this function (it runs alone from a temp file): keep the two the same
# (update_test.ps1 checks it).
function Disable-KitQuickEdit {
    try {
        $name = New-Object System.Reflection.AssemblyName 'OotCoopConsoleMode'
        $asm = [System.Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, 'Run')
        $type = $asm.DefineDynamicModule('OotCoopConsoleMode').DefineType('OotCoopConsoleMode', 'Public, Class')
        foreach ($m in @(@('CreateFileW', [IntPtr], @([string], [uint32], [uint32], [IntPtr], [uint32], [uint32], [IntPtr])),
                @('GetConsoleMode', [bool], @([IntPtr], [uint32].MakeByRefType())),
                @('SetConsoleMode', [bool], @([IntPtr], [uint32])),
                @('CloseHandle', [bool], @([IntPtr])))) {
            $type.DefinePInvokeMethod($m[0], 'kernel32.dll', 'Public, Static, PinvokeImpl', 'Standard', $m[1],
                [Type[]]$m[2], 'Winapi', 'Unicode').SetImplementationFlags('PreserveSig')   # (else the result is lost)
        }
        $native = $type.CreateType()
        # GENERIC_READ | GENERIC_WRITE, shared for reading and writing, OPEN_EXISTING
        $in = $native::CreateFileW('CONIN$', [uint32]3221225472, [uint32]3, [IntPtr]::Zero, [uint32]3, [uint32]0, [IntPtr]::Zero)
        if ($in -eq [IntPtr]::Zero -or $in -eq [IntPtr]-1) { return $false }
        try {
            $mode = [uint32]0
            if (-not $native::GetConsoleMode($in, [ref]$mode)) { return $false }
            # ENABLE_QUICK_EDIT_MODE (0x40) off; ENABLE_EXTENDED_FLAGS (0x80) on, without it the change is ignored.
            $new = [uint32](($mode -band 0xFFFFFFBF) -bor 0x80)
            return [bool]$native::SetConsoleMode($in, $new)
        } finally { [void]$native::CloseHandle($in) }
    } catch { return $false }
}
