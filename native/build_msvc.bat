@echo off
setlocal
cd /d "%~dp0"

if not exist "..\dist-native" mkdir "..\dist-native"

rc /nologo /fo app_icon.res app_icon.rc
if errorlevel 1 (
  echo Resource build failed.
  exit /b 1
)

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE inzone_buds_battery.cpp ^
  /Fe:"..\dist-native\INZONE Buds Battery.exe" ^
  /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF app_icon.res user32.lib shell32.lib gdi32.lib advapi32.lib

if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

echo Built: ..\dist-native\INZONE Buds Battery.exe
