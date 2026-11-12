@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "D:\FreeCadDevelopment\FreeCAD\src\Mod\SWDWG\tests"
cl /nologo /EHsc /std:c++20 /O2 /MD /DFCUseFreeType /I "D:\FreeCadDevelopment\LibPack\include\freetype2" /I "D:\FreeCadDevelopment\LibPack\include\freetype2\freetype" /I "..\App" /I "D:\FreeCadDevelopment\FreeCAD\src" test_ttf.cpp ..\App\FTTextRenderer.cpp "D:\FreeCadDevelopment\LibPack\lib\freetype.lib" /Fe:test_ttf.exe
