# How make_kit.ps1 copies a text file into a kit: Windows line ends (the repo stores LF) and
# @PORT@ filled in with the kit's default port. Dot-sourced by make_kit.ps1 and by
# tests/launch_test.ps1, so the test kits are copied the same way as a built kit.
#
# coop.ini.template keeps @PORT@ (and its other @TOKEN@s) for play.ps1 and hookshot-build.ps1,
# which fill it per player with Write-CoopIni (an invite-code join writes 27512, the join
# helper's port). coop-ini.ps1 keeps it too: Write-CoopIni looks for the token itself. Filling
# both with the kit's port happened to work (it then looked for "27510"), but replaced that
# number anywhere in the template, and broke as soon as one of the two was copied another way.
$script:KitTemplatesKeepPort = @('coop.ini.template', 'coop-ini.ps1')

function Copy-KitText([string]$From, [string]$To, [int]$Port) {
    $text = [System.IO.File]::ReadAllText($From).Replace("`r`n", "`n")
    if ($script:KitTemplatesKeepPort -notcontains [System.IO.Path]::GetFileName($To)) { $text = $text.Replace('@PORT@', "$Port") }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
    [System.IO.File]::WriteAllText($To, $text.Replace("`n", "`r`n"), [System.Text.UTF8Encoding]::new($false))
}
