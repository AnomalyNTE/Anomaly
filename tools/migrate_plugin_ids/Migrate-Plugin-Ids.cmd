@echo off
rem Launcher for migrate_plugin_ids.ps1.
rem All user-facing text comes from the PowerShell script, so this file stays pure ASCII
rem and cannot be broken by the console code page.
setlocal
cd /d "%~dp0"

set "SCRIPT=%~dp0migrate_plugin_ids.ps1"

if not exist "%SCRIPT%" (
    echo.
    echo   [ERROR] migrate_plugin_ids.ps1 is missing next to this launcher.
    echo   Keep Migrate-Plugin-Ids.cmd, migrate_plugin_ids.ps1 and README.md in one folder,
    echo   placed next to AnomalyLauncher.exe.
    echo.
    pause
    exit /b 1
)

set "PS="
where pwsh.exe >nul 2>nul && set "PS=pwsh.exe"
if not defined PS (
    where powershell.exe >nul 2>nul && set "PS=powershell.exe"
)
if not defined PS (
    echo.
    echo   [ERROR] PowerShell was not found on this system.
    echo.
    pause
    exit /b 1
)

"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Interactive
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
