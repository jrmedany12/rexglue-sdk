@echo off
setlocal

:: 1. Check for and delete the "out" folder
if exist "out" (
    echo [!] Found existing "out" folder. Deleting...
    rmdir /s /q "out"
    echo [+] "out" folder cleared.
) else (
    echo [.] No "out" folder found. Proceeding with a fresh build.
)

echo.

:: 2. Configure CMake with preset
echo [1/2] Configuring CMake with preset: win-amd64...
cmake --preset win-amd64

if %errorlevel% neq 0 (
    echo.
    echo [!] Configuration failed. Build cancelled.
    pause
    exit /b %errorlevel%
)

echo.

:: 3. Build and Install
echo [2/2] Building and installing target...
cmake --build out/build/win-amd64 --target install

if %errorlevel% equ 0 (
    echo.
    echo [+] Clean Build and Install completed successfully!
) else (
    echo.
    echo [!] Build failed.
)

pause
