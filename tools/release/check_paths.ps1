# Finds the build machine's user folder in files that are about to ship.
#
# A program compiled without trimmed paths carries its source folders (the emulator's
# log prints them), and some tools write where they ran into what they make. A player
# then reads the build machine's folders as if the file were someone else's. make_kit.ps1
# dot-sources this and refuses a kit with any hit; tests/paths_test.ps1 tests it.
#
#   . tools\release\check_paths.ps1
#   Find-UserPath -Root <kit> -Needles (Get-UserPathNeedles $env:USERPROFILE)

# What to look for: the user folder without its drive, with either slash
# ('\Users\Name' and '/Users/Name'), so 'C:\Users\Name', 'c:/users/name' and a
# '/c/Users/Name' of a Unix shell all match. Letter case does not matter.
function Get-UserPathNeedles([string]$UserFolder) {
    if (-not $UserFolder) { throw 'No user folder given.' }
    $path = $UserFolder.TrimEnd('\', '/') -replace '^[A-Za-z]:', ''
    $path = $path -replace '/', '\'
    if ($path.Length -lt 3 -or $path -notmatch '^\\[^\\]') { throw "Not a user folder: $UserFolder" }
    return @($path, ($path -replace '\\', '/'))
}

# Every file under Root (or the one file Root is) holding a needle, as text or in a
# program: one-byte text (ASCII, UTF-8, Windows code pages) and UTF-16 (Windows'
# own strings). Returns objects with File (full path) and Needle.
function Find-UserPath([string]$Root, [string[]]$Needles) {
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)  # byte n is char n
    $forms = foreach ($n in $Needles) {
        [pscustomobject]@{ Needle = $n; Text = $n }
        [pscustomobject]@{ Needle = $n; Text = (($n.ToCharArray() | ForEach-Object { "$_`0" }) -join '') }
    }
    $files = if (Test-Path -LiteralPath $Root -PathType Leaf) { @(Get-Item -LiteralPath $Root) }
             else { @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force) }
    foreach ($f in $files) {
        $text = $latin1.GetString([System.IO.File]::ReadAllBytes($f.FullName))
        foreach ($form in $forms) {
            if ($text.IndexOf($form.Text, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                [pscustomobject]@{ File = $f.FullName; Needle = $form.Needle }
                break
            }
        }
    }
}
