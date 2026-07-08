@echo off
setlocal
cd /d "%~dp0"

if not exist "..\dist-native" mkdir "..\dist-native"

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE inzone_buds_battery.cpp ^
  /Fe:"..\dist-native\INZONE Buds Battery.exe" ^
  /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib gdi32.lib

if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

echo Built: ..\dist-native\INZONE Buds Battery.exe
