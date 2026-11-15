@echo off
REM Save the board's frame buffer as a PNG.
REM Usage: shot.cmd --host 192.168.254.86 [-o captures\name.png]
REM Keep this file plain ASCII.
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
    echo [shot.cmd] .venv not found. See README.md, section Setup.
    pause
    exit /b 1
)
".venv\Scripts\python.exe" tools\shot.py %*
endlocal
