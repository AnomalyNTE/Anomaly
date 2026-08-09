$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$vsDevCmd = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
$nlohmannInclude = Join-Path $repoRoot ".build\windows-vs2022\_deps\nlohmann_json-src\single_include"
$objectDir = Join-Path $PSScriptRoot ".build"
$testExe = Join-Path $objectDir "controller_reacquisition_test.exe"

if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "Visual Studio 2022 Build Tools were not found."
}
if (-not (Test-Path -LiteralPath $nlohmannInclude)) {
    throw "The configured Anomaly dependency tree is missing nlohmann/json.hpp."
}

New-Item -ItemType Directory -Path $objectDir -Force | Out-Null
$compile = @(
    "cl.exe /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /MD",
    "/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX",
    "/I`"$(Join-Path $repoRoot 'include')`" /I`"$nlohmannInclude`"",
    "/Fo`"$(Join-Path $objectDir 'controller_reacquisition_test.obj')`"",
    "`"$(Join-Path $PSScriptRoot 'controller_reacquisition_test.cpp')`"",
    "/Fe:`"$testExe`""
) -join " "
$command = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && $compile"

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Chronos controller reacquisition test compilation failed with exit code $LASTEXITCODE."
}

& $testExe
if ($LASTEXITCODE -ne 0) {
    throw "Chronos controller reacquisition test failed with exit code $LASTEXITCODE."
}
