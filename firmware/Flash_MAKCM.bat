@echo off
REM Launch the MAKCM guided flash tool.
cd /d "%~dp0"

REM Use whatever Python is on PATH.
where python >nul 2>&1 && (set "PY=python") || (
  where py >nul 2>&1 && (set "PY=py") || (
    echo Python not found. Install Python or add it to PATH.
    pause
    exit /b 1
  )
)

echo Using %PY%

REM Ensure required Python packages are present in this interpreter.
"%PY%" -c "import serial" 2>nul || (
  echo Installing pyserial...
  "%PY%" -m pip install pyserial
)
"%PY%" -c "import esptool" 2>nul || (
  echo Installing esptool...
  "%PY%" -m pip install esptool
)

"%PY%" "%~dp0flash_tool.py"
if errorlevel 1 (
  echo.
  echo Flash tool exited with an error.
  pause
)
