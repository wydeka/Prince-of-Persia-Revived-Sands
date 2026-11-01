@echo off
rem Build RevivedSands_Setup.exe. Needs Inno Setup: https://jrsoftware.org/isdl.php
rem You can also pass the path to ISCC.exe as the first argument: build_setup.bat "D:\Tools\Inno Setup 6\ISCC.exe"
cd /d "%~dp0"
set "ISCC="
if not "%~1"=="" call :try "%~1"
call :try "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
call :try "%ProgramFiles%\Inno Setup 6\ISCC.exe"
call :try "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
call :try "%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe"
call :try "%ProgramFiles%\Inno Setup 7\ISCC.exe"
call :try "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
if not defined ISCC for /f "delims=" %%P in ('where ISCC.exe 2^>nul') do call :try "%%P"
if not defined ISCC for %%K in ("HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1" "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1" "HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1") do for /f "tokens=2,*" %%A in ('reg query %%K /v InstallLocation 2^>nul') do if "%%A"=="REG_SZ" call :try "%%BISCC.exe"
if not defined ISCC goto :notfound
echo Using: %ISCC%
"%ISCC%" RevivedSands.iss
if errorlevel 1 goto :failed
echo Done: Output\RevivedSands_Setup.exe
pause
exit /b 0

:notfound
echo ISCC.exe was not found.
echo Easy way: open RevivedSands.iss in Inno Setup and press Build ^> Compile (Ctrl+F9).
echo Or run:  build_setup.bat "full path to ISCC.exe"
pause
exit /b 1

:failed
echo BUILD FAILED. Send the error text above.
pause
exit /b 1

:try
if defined ISCC goto :eof
if exist "%~1" set "ISCC=%~1"
goto :eof
