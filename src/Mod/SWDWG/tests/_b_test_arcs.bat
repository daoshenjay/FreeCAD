@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "D:\FreeCadDevelopment\FreeCAD\src\Mod\SWDWG\tests"
cl /nologo /EHsc /std:c++17 /O2 /MD /DSWDWG_EXPORTS /I "..\App" /I "D:\FreeCadDevelopment\FreeCAD\src" /I "D:\FreeCadDevelopment\build\src" test_arcs.cpp ..\App\DWGShapeFile.cpp /Fe:test_arcs.exe
if errorlevel 1 goto :eof
test_arcs.exe
