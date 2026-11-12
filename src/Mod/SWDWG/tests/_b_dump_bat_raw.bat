@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "D:\FreeCadDevelopment\FreeCAD\src\Mod\SWDWG\tests"
cl /nologo /EHsc /std:c++20 /O2 /MD dump_bat_raw.cpp /Fe:dump_bat_raw.exe
