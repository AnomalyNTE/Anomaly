@echo off
setlocal EnableExtensions

rem Canonical local build: the same CMake presets used by CI.
set "CMAKE=cmake"
set "CTEST=ctest"
where cmake >nul 2>nul
if not errorlevel 1 goto :run

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :missing_cmake
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto :missing_cmake

set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto :missing_cmake
set "CTEST=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if not exist "%CTEST%" goto :missing_cmake

:run
set "BUILD_DIR=%~dp0.build\windows-vs2022"
set "PACKAGE_DIR=%BUILD_DIR%\game-package"

"%CMAKE%" --preset windows-vs2022
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" --build --preset windows-relwithdebinfo --parallel
if errorlevel 1 exit /b %errorlevel%

"%CTEST%" --preset windows-relwithdebinfo
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" -E remove_directory "%PACKAGE_DIR%"
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%PACKAGE_DIR%" --component GameRuntime
exit /b %errorlevel%

:missing_cmake
echo CMake was not found. Install CMake 3.22+ or Visual Studio 2022 C++ Build Tools.
exit /b 2
