@echo off
REM One-step build / flash / run for the STM32N6570-DK demo.
REM Usage: n6.cmd go | build | flash | run | console   (see tools\n6.py --help)
REM Keep this file plain ASCII.
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
    echo [n6.cmd] .venv not found. See README.md, section Setup.
    pause
    exit /b 1
)
".venv\Scripts\python.exe" tools\n6.py %*
if errorlevel 1 (
    echo [n6.cmd] failed.
    exit /b 1
)
endlocal
