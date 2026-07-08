@echo off
setlocal

cd /d "%~dp0"

echo Starting MAKCU inject mapper...
echo Get an offline game or controller-test screen visible first.
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

py -3 inject_mapper.py %*
if errorlevel 1 (
    echo.
    echo Inject mapper exited with an error.
    pause
)

endlocal
