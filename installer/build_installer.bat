@echo off
setlocal
cd /d "%~dp0"

set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo Inno Setup 6 was not found.
  echo Download it from https://jrsoftware.org/isdl.php
  exit /b 1
)

"%ISCC%" "INZONE-Buds-Battery.iss"
if errorlevel 1 exit /b 1

echo Built: ..\dist-native\INZONE-Buds-Battery-Setup-v1.2.1.exe
