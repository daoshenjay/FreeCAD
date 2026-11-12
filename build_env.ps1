# Temporary helper for building the SWDWG module.
#
# vcvars64.bat aborts when it inherits the pixi-activation environment, so the
# MSVC/SDK variables are harvested from a clean cmd first and then applied to
# the command below together with the pixi tool paths (cmake/ninja/ccache).
param([Parameter(Mandatory = $true)][string]$CommandLine)

$ErrorActionPreference = "Stop"

$vsenv = Join-Path $env:TEMP "dsh_vsenv.txt"
if (Test-Path $vsenv) { Remove-Item $vsenv -Force }

$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set > `"$vsenv`""
if (-not (Test-Path $vsenv)) { throw "vcvars64.bat 未能导出环境变量" }

$lines = Get-Content $vsenv
function Get-VsValue([string]$name) {
    $hit = $lines | Where-Object { $_ -match "^$name=" } | Select-Object -First 1
    if (-not $hit) { return "" }
    return $hit.Substring($name.Length + 1)
}

$env:INCLUDE = Get-VsValue "INCLUDE"
$env:LIB = Get-VsValue "LIB"
$env:LIBPATH = Get-VsValue "LIBPATH"
$vsPath = Get-VsValue "PATH"

$pixi = "E:\FreeCADWS\FreeCAD\.pixi\envs\default"
$env:PATH = "$pixi\Library\bin;$pixi\Scripts;$pixi\bin;$vsPath"

if (-not $env:INCLUDE) { throw "INCLUDE 为空，vcvars 失败" }

cmd /c $CommandLine
exit $LASTEXITCODE
