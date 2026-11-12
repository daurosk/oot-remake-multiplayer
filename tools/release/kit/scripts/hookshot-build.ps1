# Builds Hookshot (NotRexed's PC port of the game, GPL-3.0) with co-op, from this PC's own game file, into the
# kit's hookshot folder. Experimental: needs a graphics card with Vulkan 1.3 and mesh shaders (NVIDIA RTX 20 or
# newer, AMD RX 6000 or newer). Nothing from the game is in the kit: Hookshot is built here, from your .nup, and
# stays on this PC.
#   make-hookshot.cmd                     finds the .nup in the kit folder or asks for it
#   make-hookshot.cmd -Nup <file.nup>     no questions
#   make-hookshot.cmd -Jobs 8             at most 8 compilers at once (default: Hookshot's choice)
# Steps: download Hookshot's builder parts listed in hookshot-kit\TOOLS (each checked against its SHA-256),
# apply co-op's patch (hookshot-kit\patches), run Hookshot's own build (nx2\make_game.py: about 25 to 60 minutes
# the first time, 60 GB free needed), then put the co-op module and the puppet pack into the game folder.
param([string]$Nup = '', [int]$Jobs = 0, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$kit = Split-Path -Parent $PSScriptRoot
$pieces = Join-Path $kit 'hookshot-kit'
$work = Join-Path $kit 'hookshot-build'
$game = Join-Path $kit 'hookshot'
foreach ($need in 'TOOLS', 'apply_patch.py', 'hookshot_coop.dll', 'patches') {
    if (-not (Test-Path -LiteralPath (Join-Path $pieces $need))) { Write-Host "Not found: $pieces\$need (an incomplete kit)"; exit 1 }
}

# The game file.
if (-not $Nup) {
    $found = @(Get-ChildItem -LiteralPath $kit -Filter '*.nup' -File -ErrorAction SilentlyContinue)
    if ($found.Count -eq 1) { $Nup = $found[0].FullName; Write-Host "Game file: $Nup" }
    else {
        Write-Host 'Your game file: the .nup of Ocarina of Time version 1.1.1 (the same one make-puppet-pack.cmd takes).'
        $Nup = (Read-Host 'Its full path (you can drag the file into this window)').Trim().Trim('"')
    }
}
if (-not (Test-Path -LiteralPath $Nup -PathType Leaf)) { Write-Host "Not a file: $Nup"; exit 1 }

# Room: the build unpacks the game (26 GB) and makes its own files beside it.
$drive = (Get-Item -LiteralPath $kit).PSDrive
$freeGb = [math]::Floor($drive.Free / 1GB)
if ($freeGb -lt 60) { Write-Host "Only $freeGb GB free on $($drive.Name):. The build needs about 60 GB."; exit 1 }

# Hookshot's builder parts, checked.
$zips = Join-Path $work 'downloads'
$tools = Join-Path $work 'HookshotBuilder\tools'
New-Item -ItemType Directory -Force $zips | Out-Null
foreach ($line in [System.IO.File]::ReadAllLines((Join-Path $pieces 'TOOLS'))) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    $sha, $bytes, $into, $url = $t -split '\s+', 4
    $file = Join-Path $zips ([System.IO.Path]::GetFileName($url))
    $done = "$file.unpacked"
    # The source part ("app") is unpacked fresh every time, so the patch always goes onto Hookshot's own files.
    if ((Test-Path -LiteralPath $done) -and -not $Rebuild -and $url -notmatch '-app-') { continue }
    if (-not (Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $sha.ToUpper()) {
        Write-Host "Downloading $([System.IO.Path]::GetFileName($url)) ($([math]::Round([int64]$bytes / 1MB)) MB)"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
        $ProgressPreference = 'Continue'
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $sha.ToUpper()) {
            Remove-Item -LiteralPath $file -Force
            Write-Host "The download of $url does not match its SHA-256: not used. Try again later."
            exit 1
        }
    }
    $dest = Join-Path $work $into
    Write-Host "Unpacking $([System.IO.Path]::GetFileName($file))"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Expand-Archive -LiteralPath $file -DestinationPath $dest -Force
    Set-Content -LiteralPath $done -Value $sha
}
# llvm-mingw unpacks into a folder of its own name: the build looks for tools\llvm-mingw.
$mingw = Join-Path $tools 'llvm-mingw'
$inner = @(Get-ChildItem -LiteralPath $mingw -Directory -Filter 'llvm-mingw-*' -ErrorAction SilentlyContinue)
if ($inner.Count -eq 1 -and -not (Test-Path -LiteralPath (Join-Path $mingw 'bin'))) {
    Get-ChildItem -LiteralPath $inner[0].FullName | Move-Item -Destination $mingw
    Remove-Item -LiteralPath $inner[0].FullName
}
$python = Join-Path $tools 'python\python.exe'
if (-not (Test-Path -LiteralPath $python)) { Write-Host "Not found after unpacking: $python"; exit 1 }

# Co-op's patch on Hookshot's source (the "app" part holds it, "third_party" its third_party folder).
$patches = @(Get-ChildItem -LiteralPath (Join-Path $pieces 'patches') -Filter '*.patch' | Sort-Object Name | ForEach-Object { $_.FullName })
& $python -I (Join-Path $pieces 'apply_patch.py') $tools @patches
if ($LASTEXITCODE -ne 0) { Write-Host 'Co-op''s patch does not fit these Hookshot files.'; exit 1 }

# Hookshot's own build, as its builder runs it.
$env:RX_CPM = Join-Path $tools 'cpm'
$env:WGPU_NATIVE = Join-Path $tools 'wgpu-native'
$env:HOOKSHOT_MINGW = $mingw
if ($Jobs -gt 0) { $env:HOOKSHOT_JOBS = "$Jobs" }
Write-Host 'Building Hookshot with co-op. The first build takes 25 to 60 minutes; the window says what it does.'
Push-Location $tools
try {
    & $python -I (Join-Path $tools 'nx2\make_game.py') --nup $Nup --out $game --language en-US --log (Join-Path $work 'build.log')
    $code = $LASTEXITCODE
} finally { Pop-Location }
if ($code -ne 0) { Write-Host "Hookshot's build stopped (code $code). Its log: $work\build.log"; exit 1 }
if (-not (Select-String -LiteralPath (Join-Path $work 'build.log') -Pattern 'co-op hooks: frame hook' -Quiet)) {
    Write-Host "The build did not put co-op's hooks in (see 'co-op hooks:' in $work\build.log)."
    exit 1
}

# The co-op module beside hookshot.exe, with the C++ runtime it was built with (the kit's copy).
$exeDir = if (Test-Path -LiteralPath (Join-Path $game 'bin\hookshot.exe')) { Join-Path $game 'bin' } else { $game }
Get-ChildItem -LiteralPath $pieces -Filter '*.dll' -File | Copy-Item -Destination $exeDir -Force

# The puppet pack, in the mods folder Hookshot reads (mods\contents\<title>\coop-puppet\romfs).
$packTool = Join-Path $kit 'tools\puppet_pack\build.py'
if (Test-Path -LiteralPath $packTool) {
    $mods = Join-Path $exeDir 'mods\contents\0400486024DEC000\coop-puppet'
    New-Item -ItemType Directory -Force $mods | Out-Null
    & $python -I $packTool --romfs $Nup --mod-dir $mods
    if ($LASTEXITCODE -ne 0) { Write-Host 'The puppet pack was not made: the other players show without a Link.' }
}
Set-Content -LiteralPath (Join-Path $work 'built-with.txt') -Value (Get-HookshotStamp $pieces)
Write-Host ''
Write-Host "Done: Hookshot with co-op is in $game. Start it with Host-Hookshot.bat or Join-Hookshot.bat."
