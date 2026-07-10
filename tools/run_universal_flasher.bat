@echo off
setlocal

cd /d "%~dp0"

echo Starting MAKCM Universal Flasher...
echo.

py -3 -c "import serial, esptool" >nul 2>&1
if errorlevel 1 (
    echo Installing required Python packages: pyserial esptool
    py -3 -m pip install pyserial esptool
    if errorlevel 1 (
        echo.
        echo Could not install packages automatically.
        echo Try running this command manually:
        echo   py -3 -m pip install pyserial esptool
        pause
        exit /b 1
    )
    echo.
)

py -3 universal_flasher.py %*
if errorlevel 1 (
    echo.
    echo Universal flasher exited with an error.
    pause
)

endlocal
