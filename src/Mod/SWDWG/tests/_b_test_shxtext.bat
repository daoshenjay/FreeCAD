@echo off
rem Build + run the .shx single-line text renderer test (TextShxRenderer).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "D:\FreeCadDevelopment\FreeCAD\src\Mod\SWDWG\tests"
cl /nologo /EHsc /std:c++17 /O2 /MD /DSWDWG_EXPORTS /I "..\App" /I "D:\FreeCadDevelopment\FreeCAD\src" /I "D:\FreeCadDevelopment\build\src" test_shxtext.cpp ..\App\TextShxRenderer.cpp ..\App\FontResolver.cpp ..\App\DWGShapeFile.cpp /Fe:test_shxtext.exe
test_shxtext.exe
