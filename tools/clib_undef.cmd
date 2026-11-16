@echo off
rem RE:: functions FRIK + framework objects import from the flat commonlibf4.lib (undefined externals).
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
for /r "%~dp0..\build\.objs\FRIK" %%f in (*.obj) do dumpbin /symbols "%%f" | findstr /C:"UNDEF" | findstr /C:"@RE@@"
for /r "%~dp0..\build\.objs\f4cf" %%f in (*.obj) do dumpbin /symbols "%%f" | findstr /C:"UNDEF" | findstr /C:"@RE@@"
