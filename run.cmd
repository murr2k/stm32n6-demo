@echo off
REM Start the host companion: feeds the board's Host Link screen with this PC's load.
REM Usage: run.cmd [--host 192.168.254.86] [--rate 10]
REM Keep this file plain ASCII.
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
    echo [run.cmd] .venv not found. See README.md, section Setup.
    pause
    exit /b 1
)
".venv\Scripts\python.exe" -c "import psutil" 2>nul
if errorlevel 1 (
    echo [run.cmd] psutil is missing. Install it with:
    echo     .venv\Scripts\python.exe -m pip install psutil
    pause
    exit /b 1
)
".venv\Scripts\python.exe" host\companion.py %*
if errorlevel 1 (
    echo [run.cmd] the companion stopped with an error.
    pause
    exit /b 1
)
endlocal
