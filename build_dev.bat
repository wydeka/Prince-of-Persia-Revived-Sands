@echo off
rem Developer build of the DLL: log files and developer keys (F4, F6-F9) are ON by default.
rem Usage: build_dev.bat "C:\path\to\game folder"   (copies dinput8.dll and revived.ini there if given)
cd /d "%~dp0"
python -m ziglang cc -target x86-windows-gnu -O2 -shared -o dinput8.dll dinput8.c dinput8.def
if errorlevel 1 exit /b 1
if not "%~1"=="" copy /y dinput8.dll "%~1\dinput8.dll"
