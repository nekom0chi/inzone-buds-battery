@echo off
cd /d "%~dp0"

set "SCRIPT=%~dp0inzone_buds_battery.py"

if exist "%LocalAppData%\Programs\Python\Python311\pythonw.exe" (
  start "" "%LocalAppData%\Programs\Python\Python311\pythonw.exe" "%SCRIPT%" --tray --show-on-start
  exit /b 0
)

where pyw.exe >nul 2>nul
if not errorlevel 1 (
  start "" pyw.exe -3 "%SCRIPT%" --tray --show-on-start
  exit /b 0
)

where pythonw.exe >nul 2>nul
if not errorlevel 1 (
  start "" pythonw.exe "%SCRIPT%" --tray --show-on-start
  exit /b 0
)

where python.exe >nul 2>nul
if not errorlevel 1 (
  start "" python.exe "%SCRIPT%" --tray --show-on-start
  exit /b 0
)

echo Python was not found.
echo.
echo Please install Python 3 from https://www.python.org/downloads/windows/
echo Then run this BAT again.
pause
