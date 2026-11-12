@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "D:\FreeCadDevelopment\FreeCAD\src\Mod\SWDWG\tests"
cl /nologo /EHsc /std:c++17 /O2 /MD /I "..\App" probe_shx.cpp ..\App\DWGShapeFile.cpp /Fe:probe_shx.exe
if errorlevel 1 goto :eof
echo.
echo === 全部字形概览 ===
probe_shx.exe "C:\Program Files\Autodesk\AutoCAD 2016\Fonts\FSDB_2.SHX"
echo.
echo === 34 号字形详情 ===
probe_shx.exe "C:\Program Files\Autodesk\AutoCAD 2016\Fonts\FSDB_2.SHX" 34
