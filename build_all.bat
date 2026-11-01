@echo off
rem Builds everything: DLL -> installer engine -> Inno Setup wizard.
rem Needs: Python with "pip install ziglang", Go 1.24+, Inno Setup 6.
cd /d "%~dp0"
if not exist installer\payload mkdir installer\payload
copy /y ai\revived.ini installer\payload\revived.ini >nul
python -m ziglang cc -target x86-windows-gnu -O2 -shared -DPUBLIC_BUILD -o installer\payload\dinput8.dll ai\dinput8.c ai\dinput8.def
if errorlevel 1 goto failed
cd installer
set GOOS=windows
set GOARCH=amd64
set CGO_ENABLED=0
go build -trimpath -ldflags "-s -w -H=windowsgui" -o ..\setup\RevivedSands_Installer.exe .
if errorlevel 1 goto failed
cd ..\setup
call build_setup.bat
exit /b 0
:failed
echo BUILD FAILED
pause
exit /b 1
