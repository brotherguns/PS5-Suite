@echo off
REM PS5 Upload Server - Windows Build Script
REM This requires WSL (Windows Subsystem for Linux) with PS5 SDK installed

echo Building PS5 Upload Server payload...
echo.

REM Check if WSL is available
where wsl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: WSL is not installed or not in PATH
    echo Please install WSL and PS5 Payload SDK to build the payload
    pause
    exit /b 1
)

REM Convert Windows path to WSL path
set "PAYLOAD_DIR=%~dp0"
set "WSL_PATH=/mnt/c/Users/HACKMAN/Desktop/ps5 test/my_projects/ps5_upload_suite/payload"

echo Compiling with PS5 SDK...
wsl bash -c "cd '%WSL_PATH%' && make clean && make"

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo Build successful!
    echo Output: ps5_upload_server.elf
    echo ========================================
    echo.
    echo Next steps:
    echo 1. Upload ps5_upload_server.elf to PS5: /data/etaHEN/payloads/
    echo 2. Restart the payload on PS5
    echo 3. Test upload speed with the client
    echo.
) else (
    echo.
    echo ========================================
    echo Build FAILED!
    echo ========================================
    echo.
    echo Make sure PS5 Payload SDK is installed in WSL:
    echo   sudo apt install git build-essential
    echo   git clone https://github.com/ps5-payload-dev/sdk
    echo   cd sdk
    echo   sudo make install
    echo.
)

pause
