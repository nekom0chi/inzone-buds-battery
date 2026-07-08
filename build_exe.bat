@echo off
cd /d "%~dp0"

python -m PyInstaller --onefile --windowed --name "INZONE Buds Battery" --collect-submodules PIL ".\inzone_buds_battery.py"

echo.
echo Built: dist\INZONE Buds Battery.exe
pause
