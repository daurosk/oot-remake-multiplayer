# Static checks on the CI runner scripts (docs/ci-runner.md):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\scripts_test.ps1
# - every script parses;
# - no script or function assigns to one of its own parameters under another
#   spelling of case. PowerShell names ignore case, so `$user = Get-LocalUser`
#   in a script with [string]$User overwrites the parameter and, through its
#   [string] type, turns the account into its name: uninstall.ps1 once threw on
#   `$user.SID.Value` that way and skipped everything after it.
# - uninstall.ps1 has no `exit` before its last step, so one failure cannot skip
#   the rest.
param([string[]]$Paths = @(Get-ChildItem (Join-Path $PSScriptRoot '*.ps1') | ForEach-Object FullName))
$ErrorActionPreference = 'Stop'
$failed = 0
function Ok($name, $cond) { if ($cond) { "ok   $name" } else { "FAIL $name"; $script:failed++ } }

# Assignments (=, +=, a foreach variable) to a parameter of the same
# scope spelled in another case. Returns "line: `$name` shadows param `$Param`".
function Find-ParamShadows {
    param([System.Management.Automation.Language.Ast]$Ast)
    $owners = @($Ast) + @($Ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true))
    foreach ($owner in $owners) {
        $body = if ($owner -is [System.Management.Automation.Language.FunctionDefinitionAst]) { $owner.Body } else { $owner }
        $params = @()
        if ($body.ParamBlock) { $params += $body.ParamBlock.Parameters }
        if ($owner -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $owner.Parameters) { $params += $owner.Parameters }
        $names = @($params | ForEach-Object { $_.Name.VariablePath.UserPath })
        if (-not $names.Count) { continue }
        $targets = $body.FindAll({
            param($a)
            ($a -is [System.Management.Automation.Language.AssignmentStatementAst]) -or
            ($a -is [System.Management.Automation.Language.ForEachStatementAst])
        }, $true)
        foreach ($t in $targets) {
            # Only this scope: skip anything inside a nested function.
            $up = $t.Parent; $nested = $false
            while ($up -and $up -ne $body) {
                if ($up -is [System.Management.Automation.Language.FunctionDefinitionAst]) { $nested = $true; break }
                $up = $up.Parent
            }
            if ($nested) { continue }
            $left = if ($t -is [System.Management.Automation.Language.ForEachStatementAst]) { $t.Variable } else { $t.Left }
            while ($left -is [System.Management.Automation.Language.ConvertExpressionAst] -or
                   $left -is [System.Management.Automation.Language.AttributedExpressionAst]) { $left = $left.Child }
            if ($left -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
            $n = $left.VariablePath.UserPath
            foreach ($p in $names) {
                if ($n -ieq $p -and $n -cne $p) { "$($t.Extent.StartLineNumber): `$$n shadows param `$$p" }
            }
        }
    }
}
function Parse([string]$Text) {
    $tokens = $null; $errs = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errs)
    , @($ast, $errs)
}

# --- the checker itself, on made-up scripts
$bad = Parse "param([string]`$User)`n`$user = Get-LocalUser -Name `$User`n"
Ok 'finds $user = ... under [string]$User' (@(Find-ParamShadows $bad[0]).Count -eq 1)
$bad = Parse "function F { param(`$Sid) foreach (`$sid in 1,2) { } }"
Ok 'finds a foreach variable in a function' (@(Find-ParamShadows $bad[0]).Count -eq 1)
$bad = Parse "function F(`$Name) { [string]`$NAME = 'x' }"
Ok 'finds a typed assignment to an inline param' (@(Find-ParamShadows $bad[0]).Count -eq 1)
$good = Parse "param(`$User)`n`$acctObj = Get-LocalUser -Name `$User`n`$User = `$User.Trim()`nfunction G { `$user = 1 }"
Ok 'no false alarm: other name, same case, other function' (@(Find-ParamShadows $good[0]).Count -eq 0)

# --- the scripts
foreach ($path in $Paths) {
    $leaf = Split-Path $path -Leaf
    $parsed = Parse ([IO.File]::ReadAllText($path))
    Ok "$leaf parses" (-not $parsed[1].Count)
    $hits = @(Find-ParamShadows $parsed[0])
    foreach ($h in $hits) { "     $leaf $h" }
    Ok "$leaf assigns no param under another case" (-not $hits.Count)
}

# --- uninstall.ps1 runs every step
$un = Join-Path $PSScriptRoot 'uninstall.ps1'
$ast = (Parse ([IO.File]::ReadAllText($un)))[0]
$exits = @($ast.FindAll({ param($a) $a -is [System.Management.Automation.Language.ExitStatementAst] }, $true))
$last = ($ast.EndBlock.Statements | Select-Object -Last 2 | Select-Object -First 1).Extent.StartLineNumber
Ok 'uninstall.ps1: exit only in the final failure report' (@($exits | Where-Object { $_.Extent.StartLineNumber -lt $last }).Count -eq 0)
$throws = @($ast.EndBlock.Statements | Where-Object { $_ -is [System.Management.Automation.Language.ThrowStatementAst] -or
    ($_ -is [System.Management.Automation.Language.PipelineAst] -and $_.Extent.Text -match '^\s*throw\b') })
Ok 'uninstall.ps1: no top-level throw' ($throws.Count -eq 0)

if ($failed) { "$failed failed"; exit 1 }
'all passed'
