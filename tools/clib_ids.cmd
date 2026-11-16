@echo off
rem List the flat CommonLibF4 REL::Relocation statics (function-local "func"/"singleton" etc.) referenced by FRIK + framework objects.
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
for /r "%~dp0..\build\.objs\FRIK" %%f in (*.obj) do dumpbin /symbols "%%f" | findstr /C:"Relocation@" | findstr /C:"SECT"
for /r "%~dp0..\build\.objs\f4cf" %%f in (*.obj) do dumpbin /symbols "%%f" | findstr /C:"Relocation@" | findstr /C:"SECT"
