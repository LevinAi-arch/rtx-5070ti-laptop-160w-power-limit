@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [!] This script requires Administrator privileges.
    echo Please right-click this file and select "Run as administrator".
    pause
    exit /b 1
)

echo [*] Installing Nvpwr test certificate to Trusted Root and Trusted Publishers...
certutil -addstore -f "Root" "%~dp0Nvpwr.cer"
certutil -addstore -f "TrustedPublisher" "%~dp0Nvpwr.cer"

echo.
echo [*] Enabling Windows Test Mode (testsigning on)...
bcdedit /set testsigning on

echo.
echo =========================================================================
echo [OK] Setup complete!
echo.
echo NOTE: If Secure Boot is enabled in your BIOS/UEFI, Windows will ignore
echo testsigning mode. Ensure Secure Boot is DISABLED in your BIOS settings,
echo then restart your PC.
echo.
echo After restarting, run dist\NvpwrControl.exe as administrator.
echo =========================================================================
pause
