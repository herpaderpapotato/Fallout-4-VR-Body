@echo off
rem Run errsweep.py inside the MSVC x64 environment (cl.exe needs INCLUDE/LIB).
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
python -I "%~dp0errsweep.py" %*
