@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "D:\FreeCadDevelopment\FreeCAD\src\Mod\SWDWG\tests"
cl /nologo /EHsc /std:c++17 /O2 /MD /I "D:\libredwg-master\libredwg\include" dump_ltype.cpp "D:\libredwg-master\build-vs2022\Release\libredwg.lib" /Fe:dump_ltype.exe