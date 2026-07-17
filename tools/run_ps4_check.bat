@echo off
setlocal

cd /d "%~dp0"

echo Starting PS4 controller injection check...
echo.

py -3 -c "import serial" >nul 2>&1
if errorlevel 1 (
    echo Installing required Python package: pyserial
    py -3 -m pip install pyserial
    if errorlevel 1 (
        echo.
        echo Could not install pyserial automatically.
        echo Try running this command manually:
        echo   py -3 -m pip install pyserial
        pause
        exit /b 1
    )
    echo.
)

py -3 ps4_check.py %*
echo.
pause

endlocal
