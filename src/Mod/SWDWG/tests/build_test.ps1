# Build + run the standalone SWDWG unit tests:
#   test_align.cpp - A-alignment / complex-element cutting (header only)
#   test_shape.cpp - .shx shape file parser (links ..\App\DWGShapeFile.cpp)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$vcvars = $null
$cands = @(
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
foreach ($c in $cands) {
    if (Test-Path $c) { $vcvars = $c; break }
}
if (-not $vcvars) {
    Write-Error "vcvars64.bat not found"
}
Write-Host "Using: $vcvars"

function Build-Run($name, $srcs) {
    $exe = Join-Path $root "$name.exe"
    $out = Join-Path $root "$name.out"
    $srcArgs = ($srcs | ForEach-Object { "`"$_`"" }) -join " "
    # cd into the test dir so the .obj files land next to the sources.
    cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && cl /nologo /EHsc /std:c++17 /I `"$root\..\App`" $srcArgs /Fe:`"$exe`" > `"$out`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "=== BUILD FAILED ($name) ==="
        Get-Content $out
        exit 1
    }
    Write-Host "=== BUILD OK ($name) ==="
    & $exe
    $code = $LASTEXITCODE
    Write-Host "=== $name exit code: $code ==="
    Remove-Item $exe, "$root\*.obj", $out -ErrorAction SilentlyContinue
    if ($code -ne 0) { exit $code }
}

Build-Run "test_align" @((Join-Path $root "test_align.cpp"))
Build-Run "test_shape" @((Join-Path $root "test_shape.cpp"), (Join-Path $root "..\App\DWGShapeFile.cpp"))

Write-Host "=== ALL TESTS PASSED ==="
exit 0
