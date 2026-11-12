$ErrorActionPreference = 'Stop'
$root = 'd:\FreeCadDevelopment\FreeCAD\src\Mod\SWDWG'
Set-Location "$root\tests"

# Locate VS developer environment
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

$out = cmd /c "call `"$vcvars`" >nul 2>&1 && cl /nologo /EHsc /std:c++17 /I ..\App test_zigzag.cpp ..\App\DWGShapeFile.cpp /Fe:test_zigzag.exe" 2>&1
if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 20; throw 'compile failed' }

& "$root\tests\test_zigzag.exe" | Out-File -Encoding utf8 "$root\tests\zigzag.svg"
Write-Host 'zigzag.svg written'
