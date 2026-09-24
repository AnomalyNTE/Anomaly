@echo off
setlocal EnableExtensions

rem Canonical local build: the same CMake presets and release components as CI.
rem
rem   build.cmd              Runtime, Tools and SDK targets, then the deployable
rem                          Runtime package in game-package\
rem   build.cmd fixtures     also build the development fixtures
rem   build.cmd probes       also build the diagnostic probe packages
rem   build.cmd symbols      also generate and stage linker PDBs (local debugging)
rem   build.cmd package      also stage Tools and SDK under package\
rem
rem Arguments can be combined, e.g. "build.cmd fixtures package".

set "CMAKE=cmake"
where cmake >nul 2>nul
if not errorlevel 1 goto :arguments

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :missing_cmake
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto :missing_cmake

set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto :missing_cmake

:arguments
set "FIXTURES=OFF"
set "PROBES=OFF"
set "SYMBOLS=OFF"
set "PACKAGE=OFF"

:parse
if "%~1"=="" goto :run
if /i "%~1"=="fixtures" goto :argument_fixtures
if /i "%~1"=="probes" goto :argument_probes
if /i "%~1"=="symbols" goto :argument_symbols
if /i "%~1"=="package" goto :argument_package
if /i "%~1"=="help" goto :help
if /i "%~1"=="-h" goto :help
if /i "%~1"=="--help" goto :help
echo Unknown argument: %~1
goto :usage

:argument_fixtures
set "FIXTURES=ON"
shift
goto :parse

:argument_probes
set "PROBES=ON"
shift
goto :parse

:argument_symbols
set "SYMBOLS=ON"
shift
goto :parse

:argument_package
set "PACKAGE=ON"
shift
goto :parse

:run
set "BUILD_DIR=%~dp0.build\windows-vs2022"
set "PACKAGE_DIR=%BUILD_DIR%\game-package"

rem Fixtures, diagnostic probes and PDBs stay out of the default build.
"%CMAKE%" --preset windows-vs2022 -DANOMALY_BUILD_TEST_FIXTURES=%FIXTURES% -DANOMALY_BUILD_DIAGNOSTIC_PROBES=%PROBES% -DANOMALY_BUILD_SYMBOLS=%SYMBOLS%
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" --build --preset windows-relwithdebinfo --parallel
if errorlevel 1 exit /b %errorlevel%

rem Keep the existing package tree and replace only files owned by the install set.
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%PACKAGE_DIR%" --component GameRuntime
if errorlevel 1 exit /b %errorlevel%

if "%PACKAGE%"=="ON" goto :stage_components
exit /b 0

:stage_components
rem Stage the release components exactly as tools\package_release.ps1 does.
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%BUILD_DIR%\package\runtime" --component GameRuntime
if errorlevel 1 exit /b %errorlevel%
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%BUILD_DIR%\package\tools" --component Tools
if errorlevel 1 exit /b %errorlevel%
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%BUILD_DIR%\package\sdk" --component SDK
if errorlevel 1 exit /b %errorlevel%
if "%SYMBOLS%"=="OFF" exit /b 0
"%CMAKE%" --install "%BUILD_DIR%" --config RelWithDebInfo --prefix "%BUILD_DIR%\package\symbols" --component Symbols
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:help
set "USAGE_EXIT=0"
goto :print_usage

:usage
set "USAGE_EXIT=1"

:print_usage
echo.
echo usage: build.cmd [fixtures] [probes] [symbols] [package]
echo.
echo   (no argument)  build the Runtime, Tools and SDK targets, then install the
echo                  deployable Runtime package into
echo                  .build\windows-vs2022\game-package
echo   fixtures       also build the development fixtures
echo   probes         also build the diagnostic probe packages
echo   symbols        also generate linker PDBs and stage the Symbols component
echo   package        also stage Tools and SDK into .build\windows-vs2022\package
echo.
exit /b %USAGE_EXIT%

:missing_cmake
echo CMake was not found. Install CMake 3.22+ or Visual Studio 2022 C++ Build Tools.
exit /b 2
