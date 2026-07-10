@echo off
setlocal

cd /d "%~dp0"

echo Starting MAKCM no-throughput debugger...
echo.
echo Wiring: PC -^> USB2 (middle) AND USB1 plugged in (powers the Left MCU).
echo Controller -^> USB3. Have the controller ready to unplug/replug.
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

py -3 makcm_debug.py %*

echo.
pause

endlocal
