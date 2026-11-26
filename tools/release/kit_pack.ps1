# The puppet pack tool's part of the Windows kit (make_kit_hookshot.ps1; tested by tests/kit_pack_test.ps1, which
# stages it on its own and runs the staged tool's imports and data reads in CI). The tool reads three kinds of file of
# its own: the *.py modules build.py imports (and theirs), ui_pack.py's strings\*.txt, and the data folders of the
# modules that have one (DATA = os.path.join(HERE, "<name>", "data"): charsel_ui.py's charsel\data, coop_menu.py's
# coopmenu\data). One missing from the kit stops the pack on the players' PCs (0.11.12 kit, db12ad12: no charsel\data,
# FileNotFoundError on manifest.json). Dot-source kit_text.ps1 first (Copy-KitText).

# Copies the tool from <Repo>\tools\puppet_pack into <Kit>\tools\puppet_pack; returns the module names copied.
function Copy-KitPack([string]$Repo, [string]$Kit, [int]$Port) {
    $packTool = Join-Path $Repo 'tools\puppet_pack'
    # (the former emulator kit builder's list: its tests and the scene-placing test helper stay out), plus every module build.py imports.
    $packFiles = @('ainb.py', 'build.py', '__init__.py', 'byml.py', 'dumpcheck.py', 'dungeon_rooms.py', 'hud.py', 'nup.py', 'rstb.py',
        'sarc.py', 'texture.py', 'title_menu.py', 'tunic.py', 'tunic_extra.py', 'zs.py', 'merge_tables.py')   # (merge_tables.py: play.ps1, #48)
    # And every module those import, and theirs (title_menu.py -> ui_pack.py -> msbt.py): one missing from the kit stops
    # build.py at its first import, and the pack is never made on the players' PCs.
    $packSeen = @{}
    $packQueue = [System.Collections.Generic.Queue[string]]::new()
    foreach ($f in $packFiles) { $packQueue.Enqueue($f) }
    while ($packQueue.Count -gt 0) {
        $f = $packQueue.Dequeue()
        if ($packSeen.ContainsKey($f)) { continue }
        $packSeen[$f] = $true
        if (-not (Test-Path (Join-Path $packTool $f))) { throw "tools\puppet_pack\$f (needed by build.py) is not in the repo" }
        foreach ($line in (Get-Content (Join-Path $packTool $f))) {
            $code = $line -replace '#.*$', ''
            if ($code -match '^\s*from \.(\w+) import ') { $packQueue.Enqueue("$($Matches[1]).py") }
            elseif ($code -match '^\s*from \. import (.+)$') {
                foreach ($m in $Matches[1].Split(',')) {
                    $packModule = ($m.Trim() -split '\s+')[0]   # (from . import texture as tx; not $name: the kit's own)
                    if ($packModule) { $packQueue.Enqueue("$packModule.py") }
                }
            }
        }
    }
    foreach ($f in ($packSeen.Keys | Sort-Object)) { Copy-KitText (Join-Path $packTool $f) (Join-Path $Kit "tools\puppet_pack\$f") $Port }
    # ui_pack.py's words for the game's menus (strings\en.txt and its translations), read where the module is
    foreach ($s in (Get-ChildItem -LiteralPath (Join-Path $packTool 'strings') -Filter '*.txt')) {
        Copy-KitText $s.FullName (Join-Path $Kit "tools\puppet_pack\strings\$($s.Name)") $Port
    }
    # Every data folder a kit module reads: byte for byte (deltas and code patch checked by their hashes), never through
    # Copy-KitText. The import walk above finds only .py files; a module without its data makes no tab on the players' PCs.
    foreach ($f in ($packSeen.Keys | Sort-Object)) {
        foreach ($m in [regex]::Matches([IO.File]::ReadAllText((Join-Path $packTool $f)), 'os\.path\.join\(HERE,\s*"(\w+)",\s*"data"\)')) {
            $sub = $m.Groups[1].Value
            $dataDir = Join-Path $packTool "$sub\data"
            if (-not (Get-ChildItem -LiteralPath $dataDir -File -ErrorAction SilentlyContinue)) { throw "tools\puppet_pack\$sub\data (read by $f) is not in the repo" }
            New-Item -ItemType Directory -Force -Path (Join-Path $Kit "tools\puppet_pack\$sub\data") | Out-Null
            foreach ($s in (Get-ChildItem -LiteralPath $dataDir -File)) {
                Copy-Item -LiteralPath $s.FullName -Destination (Join-Path $Kit "tools\puppet_pack\$sub\data\$($s.Name)") -Force
            }
        }
    }
    return @($packSeen.Keys | Sort-Object)
}

# The kit's self-check: every file of every pack data folder in the repo (tools\puppet_pack\<name>\data) is in the kit
# byte for byte, and every strings\*.txt is there. Returns the ones missing or differing ('' none).
function Test-KitPackData([string]$Repo, [string]$Kit) {
    $packTool = Join-Path $Repo 'tools\puppet_pack'
    $miss = @()
    foreach ($d in (Get-ChildItem -LiteralPath $packTool -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'data') })) {
        foreach ($s in (Get-ChildItem -LiteralPath (Join-Path $d.FullName 'data') -File)) {
            $k = Join-Path $Kit "tools\puppet_pack\$($d.Name)\data\$($s.Name)"
            if (-not (Test-Path -LiteralPath $k) -or (Get-FileHash -LiteralPath $k).Hash -ne (Get-FileHash -LiteralPath $s.FullName).Hash) {
                $miss += "$($d.Name)\data\$($s.Name)"
            }
        }
    }
    foreach ($s in (Get-ChildItem -LiteralPath (Join-Path $packTool 'strings') -Filter '*.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Kit "tools\puppet_pack\strings\$($s.Name)"))) { $miss += "strings\$($s.Name)" }
    }
    return @($miss)
}
