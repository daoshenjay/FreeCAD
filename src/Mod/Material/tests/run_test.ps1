# SPDX-License-Identifier: LGPL-2.1-or-later
<#
.SYNOPSIS
    Run a GUI test script inside FreeCAD with a hard timeout.

.DESCRIPTION
    The test scripts close their own window, but a modal dialog or a crash can
    leave FreeCAD open.  This runner starts FreeCAD, waits for the script,
    captures the output and kills every FreeCAD process if the timeout expires,
    so a failing test never needs a manual window close.

.EXAMPLE
    powershell -File src\Mod\Material\tests\run_test.ps1 -Script src\Mod\Material\tests\qa_layers_ui.py
#>
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [int]$TimeoutSeconds = 240,
    [string]$RepoRoot = (Get-Location).Path
)

$ErrorActionPreference = 'Continue'
$scriptPath = (Resolve-Path -LiteralPath $Script).Path
$pixi = Join-Path $env:USERPROFILE '.pixi\bin\pixi.exe'
$log = Join-Path $env:TEMP ("swtest_" + [guid]::NewGuid().ToString('N') + ".log")

Write-Host "running $scriptPath (timeout ${TimeoutSeconds}s)" -ForegroundColor Cyan
$process = Start-Process -PassThru -FilePath $pixi `
    -ArgumentList @('run', 'FreeCAD.exe', $scriptPath) `
    -WorkingDirectory $RepoRoot `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err"

$finished = $process.WaitForExit($TimeoutSeconds * 1000)
if (-not $finished) {
    Write-Warning "timeout after ${TimeoutSeconds}s - killing FreeCAD"
    Get-Process FreeCAD -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    try { $process.Kill() } catch { }
    Start-Sleep -Seconds 1
}

if (Test-Path $log) { Get-Content $log }
if (Test-Path "$log.err") {
    $errors = Get-Content "$log.err" | Where-Object { $_ -match 'Traceback|Error|Exception' }
    if ($errors) { Write-Host "--- stderr ---" -ForegroundColor Yellow; $errors }
}

# never leave a stray window behind
Get-Process FreeCAD -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle } |
    ForEach-Object { Write-Host "cleaning up FreeCAD pid $($_.Id)"; Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }

Remove-Item $log, "$log.err" -Force -ErrorAction SilentlyContinue
if (-not $finished) { exit 124 }
exit $process.ExitCode
