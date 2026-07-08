@echo off
setlocal

cd /d "%~dp0"

echo Launching Makcu GUI...
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

py -3 makcu_gui.py
if errorlevel 1 (
    echo.
    echo Python launcher failed, trying python directly...
    python makcu_gui.py
)

if errorlevel 1 (
    echo.
    echo Makcu GUI exited with an error.
    pause
)

endlocal
