@echo off
setlocal
cd /d "%~dp0"

if not exist "..\dist-native" mkdir "..\dist-native"

set "GPP=g++"
where g++ >nul 2>nul
if errorlevel 1 (
  set "GPP=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
)

"%GPP%" -std=c++17 -O2 -municode -mwindows -static -static-libgcc -static-libstdc++ inzone_buds_battery.cpp ^
  -o "..\dist-native\INZONE Buds Battery Native.exe" ^
  -luser32 -lshell32 -lgdi32

if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

echo Built: ..\dist-native\INZONE Buds Battery Native.exe
