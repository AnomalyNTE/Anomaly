@echo off
setlocal
set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
set "WINSDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"
set "REPO=D:\NTEtools\anomaly载具改装召唤\改装目录全量分析\2.7官方源码\Anomaly-783174432e3259e4877da4ec45013d86e8c84ef3"
set "SRC=D:\NTEtools\anomaly载具改装召唤\改装目录全量分析\2.7改装版\src"
set "OUTDIR=D:\NTEtools\anomaly载具改装召唤\改装目录全量分析\2.7改装版\build"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set "INCLUDE=%MSVC%\include;%WINSDK%\ucrt;%WINSDK%\um;%WINSDK%\shared;%WINSDK%\winrt"
set "LIB=%MSVC%\lib\x64;%WINSDK_LIB%\ucrt\x64;%WINSDK_LIB%\um\x64"

"%MSVC%\bin\Hostx64\x64\cl.exe" /nologo /std:c++20 /EHsc /O2 /LD /utf-8 ^
    /I"%REPO%\include" /I"%REPO%\plugins\NteVehicle" ^
    "%SRC%\plugin.cpp" "%SRC%\runtime.cpp" "%SRC%\native_access.cpp" ^
    /Fe"%OUTDIR%\plugin.dll" ^
    /Fo"%OUTDIR%\\" ^
    /link /DLL /MACHINE:X64 /OUT:"%OUTDIR%\plugin.dll" user32.lib ^
    /LIBPATH:"%MSVC%\lib\x64" /LIBPATH:"%WINSDK_LIB%\ucrt\x64" /LIBPATH:"%WINSDK_LIB%\um\x64"

if %errorlevel% neq 0 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILD OK
dir "%OUTDIR%\plugin.dll"
rem Local: archive versioned dll + manifest into the history folder.
powershell -NoProfile -Command "$root='%~dp0'; $m=@(Get-ChildItem -LiteralPath $root -Recurse -Filter manifest.json)[0]; $v=($m|Get-Content -Raw|ConvertFrom-Json).version; $h=@(Get-ChildItem -LiteralPath $root -Directory | Where-Object { $_.Name -ne 'build' -and $_.Name -ne 'src' -and $_.Name -notlike 'NteVehicle-*' })[0]; if($m -and $v -and $h){ Copy-Item -LiteralPath $m.FullName -Destination (Join-Path $h.FullName ('v{0}-manifest.json' -f $v)) -Force; Copy-Item -LiteralPath (Join-Path $root 'build\plugin.dll') -Destination (Join-Path $h.FullName ('v{0}-plugin.dll' -f $v)) -Force; Write-Host ('ARCHIVED ' + $v) } else { Write-Host ARCHIVE-SKIPPED }"
