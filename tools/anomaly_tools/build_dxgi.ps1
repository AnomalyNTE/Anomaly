[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$DumperPath
)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7+ (pwsh.exe) is required; invoke this script with pwsh -NoProfile -File.'
}

$ErrorActionPreference = 'Stop'

$source = $PSScriptRoot
$workspace = [IO.Path]::GetFullPath((Join-Path $source '..\..'))
$resolvedDumper = (Resolve-Path -LiteralPath $DumperPath -ErrorAction Stop).Path
if ([IO.Path]::GetExtension($resolvedDumper) -ine '.dll') {
    throw 'DumperPath must point to a DLL.'
}
$objectDirectory = Join-Path $workspace '.build\windows-vs2022\anomaly-tools-dxgi\RelWithDebInfo'
$outputDirectory = Join-Path $workspace '.build\windows-vs2022\bin\RelWithDebInfo\anomaly-tools\dxgi'
New-Item -ItemType Directory -Force -Path $objectDirectory, $outputDirectory | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $installation) { throw 'Visual Studio 2022 C++ tools were not found.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines = & $env:ComSpec /d /s /c "`"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio x64 environment setup failed.' }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { continue }
    [Environment]::SetEnvironmentVariable(
        $line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
}

$expectedExports = [ordered]@{
    ApplyCompatResolutionQuirking = 1
    CompatString = 2
    CompatValue = 3
    CreateDXGIFactory = 10
    CreateDXGIFactory1 = 11
    CreateDXGIFactory2 = 12
    DXGID3D10CreateDevice = 13
    DXGID3D10CreateLayeredDevice = 14
    DXGID3D10GetLayeredDeviceSize = 15
    DXGID3D10RegisterLayers = 16
    DXGIDeclareAdapterRemovalSupport = 17
    DXGIDisableVBlankVirtualization = 18
    DXGIDumpJournal = 4
    DXGIGetDebugInterface1 = 19
    DXGIReportAdapterConfiguration = 20
    PIXBeginCapture = 5
    PIXEndCapture = 6
    PIXGetCaptureState = 7
    SetAppCompatStringPointer = 8
    UpdateHMDEmulationStatus = 9
}
$systemModule = Join-Path $env:SystemRoot 'System32\dxgi.dll'
$exportLines = @(& dumpbin.exe /nologo /exports $systemModule)
$actualExports = @{}
$ordinalOnlyExports = [Collections.Generic.List[int]]::new()
foreach ($line in $exportLines) {
    if ($line -match '^\s*(\d+)\s+[0-9A-F]+\s+[0-9A-F]+\s+([A-Za-z_]\w*)(?:\s+=.*)?$') {
        $actualExports[$Matches[2]] = [int]$Matches[1]
    } elseif ($line -match '^\s*(\d+)\s+(?:[0-9A-F]+\s+)?\[NONAME\]') {
        $ordinalOnlyExports.Add([int]$Matches[1])
    }
}
foreach ($entry in $expectedExports.GetEnumerator()) {
    if (-not $actualExports.ContainsKey($entry.Key) -or
        $actualExports[$entry.Key] -ne $entry.Value) {
        throw "System32 DXGI export mismatch: $($entry.Key)@$($entry.Value) was not found."
    }
}
if ($actualExports.Count -ne $expectedExports.Count -or $ordinalOnlyExports.Count -ne 0) {
    throw 'System32 DXGI export surface differs from this proxy; update the table first.'
}

$proxyObject = Join-Path $objectDirectory 'dxgi_proxy.obj'
$stubObject = Join-Path $objectDirectory 'dxgi_stubs.obj'
$proxy = Join-Path $outputDirectory 'dxgi.dll'
$proxyPdb = Join-Path $outputDirectory 'AnomalyToolsDXGI.pdb'
$importLibrary = Join-Path $objectDirectory 'dxgi-proxy.lib'
$bundledDumper = Join-Path $outputDirectory 'Dumper-7.dll'
$bundledDumperConfig = Join-Path $outputDirectory 'Dumper-7.ini'
$dumperConfigSource = Join-Path $source 'Dumper-7.ini'
$stagedLog = Join-Path $outputDirectory 'anomaly-tools.log'

foreach ($artifact in @($bundledDumper, $stagedLog)) {
    if (Test-Path -LiteralPath $artifact -PathType Leaf) {
        Remove-Item -LiteralPath $artifact -Force
    }
}

& ml64.exe /nologo /c "/Fo$stubObject" (Join-Path $source 'dxgi_stubs.asm')
if ($LASTEXITCODE -ne 0) { throw 'DXGI MASM compilation failed.' }
& cl.exe /nologo /std:c++20 /O2 /Ob2 /MT /W4 /permissive- /EHsc `
    /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /c `
    (Join-Path $source 'dxgi_proxy.cpp') "/Fo$proxyObject"
if ($LASTEXITCODE -ne 0) { throw 'DXGI proxy compilation failed.' }
& link.exe /nologo /dll /machine:x64 /debug:full /opt:ref /opt:icf "/out:$proxy" `
    "/pdb:$proxyPdb" "/implib:$importLibrary" `
    $proxyObject $stubObject kernel32.lib
if ($LASTEXITCODE -ne 0) { throw 'DXGI proxy link failed.' }

$proxyExportLines = @(& dumpbin.exe /nologo /exports $proxy)
$proxyExports = @{}
$proxyOrdinalOnlyExports = [Collections.Generic.List[int]]::new()
foreach ($line in $proxyExportLines) {
    if ($line -match '^\s*(\d+)\s+[0-9A-F]+\s+[0-9A-F]+\s+([A-Za-z_]\w*)(?:\s+=.*)?$') {
        $proxyExports[$Matches[2]] = [int]$Matches[1]
    } elseif ($line -match '^\s*(\d+)\s+(?:[0-9A-F]+\s+)?\[NONAME\]') {
        $proxyOrdinalOnlyExports.Add([int]$Matches[1])
    }
}
foreach ($entry in $expectedExports.GetEnumerator()) {
    if (-not $proxyExports.ContainsKey($entry.Key) -or
        $proxyExports[$entry.Key] -ne $entry.Value) {
        throw "Built DXGI export mismatch: $($entry.Key)@$($entry.Value) was not found."
    }
}
if ($proxyExports.Count -ne $expectedExports.Count -or
    $proxyOrdinalOnlyExports.Count -ne 0) {
    throw 'Built DXGI export surface differs from System32 DXGI.'
}

Copy-Item -LiteralPath $resolvedDumper -Destination $bundledDumper -Force
Copy-Item -LiteralPath $dumperConfigSource -Destination $bundledDumperConfig -Force

Get-FileHash -LiteralPath $proxy -Algorithm SHA256
Get-FileHash -LiteralPath $bundledDumper -Algorithm SHA256
Write-Host "Anomaly Tools DXGI output: $outputDirectory"
