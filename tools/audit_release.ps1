[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$PackageDirectory,
    [Parameter(Mandatory)]
    [ValidateSet('Runtime', 'SDK', 'Tools', 'Symbols')]
    [string]$Component,
    [string]$WorkspaceRoot = (Join-Path $PSScriptRoot '..'),
    [string]$ReportPath
)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7+ (pwsh.exe) is required; invoke this script with pwsh -NoProfile -File.'
}

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath($WorkspaceRoot).TrimEnd('\', '/')
$package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath $package -PathType Container)) {
    throw "package directory not found: $package"
}

$entries = @(Get-ChildItem -LiteralPath $package -Recurse -Force | Sort-Object FullName)
$files = @($entries | Where-Object { -not $_.PSIsContainer })
if ($files.Count -eq 0) { throw "$Component package is empty" }

$legalRequired = @(
    'LICENSE',
    'NOTICE',
    'third_party/licenses/imgui-LICENSE.txt',
    'third_party/licenses/minhook-LICENSE.txt',
    'third_party/licenses/nlohmann-json-LICENSE.txt',
    'third_party/licenses/json-schema-validator-LICENSE.txt',
    'third_party/licenses/noto-sans-cjk-LICENSE.txt'
)

$violations = [Collections.Generic.List[string]]::new()
$violationSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
function Add-Violation([string]$Message) {
    if ($violationSet.Add($Message)) { $violations.Add($Message) }
}

$runtimeRequired = @(
    'dwmapi.dll',
    'AnomalyLauncher.exe',
    'Anomaly/Anomaly.Core.dll',
    'Anomaly/AnomalyCrashCoordinator.exe',
    'Anomaly/anomaly.ini',
    'Anomaly/repository.json',
    'Anomaly/plugin-repositories.json',
    'Anomaly/locales/host/en-US.json',
    'Anomaly/locales/host/zh-CN.json',
    'Anomaly/assets/fonts/NotoSansCJKsc-Regular.ttf',
    'Anomaly/profiles/nte/nte-build-profile.json.example',
    'Anomaly/profiles/nte/nte-current.json',
    'Anomaly/profiles/nte/README.md',
    'Anomaly/plugins/NtePosition/plugin.dll',
    'Anomaly/plugins/NtePosition/manifest.json',
    'Anomaly/plugins/NtePosition/locales/zh-CN.json',
    'Anomaly/plugins/NteTeleport/plugin.dll',
    'Anomaly/plugins/NteTeleport/manifest.json',
    'Anomaly/plugins/NteTeleport/locales/zh-CN.json',
    'Anomaly/plugins/EntityESP/plugin.dll',
    'Anomaly/plugins/EntityESP/manifest.json',
    'Anomaly/plugins/EntityESP/locales/zh-CN.json',
    'Anomaly/plugins/PinkPawHeistESP/plugin.dll',
    'Anomaly/plugins/PinkPawHeistESP/manifest.json',
    'Anomaly/plugins/PinkPawHeistESP/locales/zh-CN.json',
    'Anomaly/plugins/FakeUID/plugin.dll',
    'Anomaly/plugins/FakeUID/manifest.json',
    'Anomaly/plugins/FakeUID/locales/zh-CN.json'
)
$sdkRequired = @(
    'include/anomaly/sdk/anomaly_sdk.h',
    'include/anomaly/sdk/base.h',
    'include/anomaly/sdk/cpp.hpp',
    'include/anomaly/sdk/plugin.h',
    'include/anomaly/sdk/version.h',
    'include/anomaly/sdk/services/core.h',
    'include/anomaly/sdk/services/localization.h',
    'include/anomaly/sdk/services/plugin_state.h',
    'include/anomaly/sdk/services/nte.h',
    'include/anomaly/sdk/services/ue5.h',
    'include/anomaly/sdk/services/ui.h',
    'lib/cmake/AnomalySDK/AnomalyPlugin.cmake',
    'lib/cmake/AnomalySDK/AnomalySDKConfig.cmake',
    'lib/cmake/AnomalySDK/AnomalySDKConfigVersion.cmake',
    'lib/cmake/AnomalySDK/AnomalySDKTargets.cmake',
    'share/anomaly/schemas/plugin-manifest.schema.json',
    'share/anomaly/schemas/build-profile.schema.json',
    'share/anomaly/schemas/repository-config.schema.json',
    'share/anomaly/schemas/repository-index.schema.json',
    'share/anomaly/schemas/plugin-enablement.schema.json',
    'share/anomaly/abi/anomaly-sdk-v1-windows-x64.json',
    'share/anomaly/examples/CMakeLists.txt',
    'share/anomaly/examples/README.md',
    'share/anomaly/examples/hello_ui/plugin.c',
    'share/anomaly/examples/hello_ui/manifest.json',
    'share/anomaly/examples/hello_ui/locales/zh-CN.json',
    'share/anomaly/examples/tick_counter/plugin.cpp',
    'share/anomaly/examples/tick_counter/manifest.json',
    'share/anomaly/examples/reliable_config/plugin.cpp',
    'share/anomaly/examples/reliable_config/manifest.json',
    'share/anomaly/examples/nte_inspector/plugin.cpp',
    'share/anomaly/examples/nte_inspector/manifest.json'
)
$toolsRequired = @(
    'anomaly-cli.exe',
    'anomaly-inspect.exe',
    'anomaly-profile.exe',
    'anomaly-plugin.exe',
    'anomaly-abi-snapshot.exe',
    'anomaly-test-host.exe'
)
$symbolsRequired = @(
    'symbols/dwmapi.pdb',
    'symbols/AnomalyLauncher.pdb',
    'symbols/Anomaly.Core.pdb',
    'symbols/AnomalyCrashCoordinator.pdb',
    'symbols/anomaly-cli.pdb',
    'symbols/anomaly-inspect.pdb',
    'symbols/anomaly-profile.pdb',
    'symbols/anomaly-plugin.pdb',
    'symbols/anomaly-abi-snapshot.pdb',
    'symbols/anomaly-test-host.pdb',
    'symbols/HelloUi.pdb',
    'symbols/TickCounter.pdb',
    'symbols/ReliableConfig.pdb',
    'symbols/NtePosition.pdb',
    'symbols/NteTeleport.pdb',
    'symbols/EntityESP.pdb',
    'symbols/NteInspector.pdb',
    'symbols/PinkPawHeistESP.pdb',
    'symbols/FakeUID.pdb'
)
$runtimeRequired += $legalRequired
$sdkRequired += $legalRequired
$toolsRequired += $legalRequired
$symbolsRequired += $legalRequired

$requiredPaths = switch ($Component) {
    'Runtime' { $runtimeRequired }
    'SDK' { $sdkRequired }
    'Tools' { $toolsRequired }
    'Symbols' { $symbolsRequired }
}
$requiredSet = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($path in $requiredPaths) { [void]$requiredSet.Add($path) }

$forbiddenExtensions = @(
    '.lib', '.exp', '.obj', '.ilk', '.idb', '.pch', '.iobj', '.ipdb', '.map'
)
$forbiddenSegments = @(
    'CMakeFiles', '_deps', '.cache', 'cache', 'tmp', 'temp', 'tests'
)
$relativeFiles = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)

foreach ($entry in $entries) {
    $relative = [IO.Path]::GetRelativePath($package, $entry.FullName).Replace('\', '/')
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Add-Violation "reparse point in package: $relative"
    }
    if ($entry.PSIsContainer) { continue }
    [void]$relativeFiles.Add($relative)
    $segments = $relative.Split('/', [StringSplitOptions]::RemoveEmptyEntries)
    if ($entry.Name -in @(
            'CMakeCache.txt', 'cmake_install.cmake', 'build.ninja', 'build.ninja.deps',
            'build.ninja.log', 'Makefile')) {
        Add-Violation "build metadata: $relative"
    }
    if ($segments | Where-Object { $_ -in $forbiddenSegments }) {
        Add-Violation "temporary/build directory: $relative"
    }
    if ($Component -ne 'Symbols' -and
        $entry.Extension.ToLowerInvariant() -in $forbiddenExtensions) {
        Add-Violation "build/import artifact: $relative"
    }
    if ($Component -ne 'Symbols' -and $entry.Extension -ieq '.pdb') {
        Add-Violation "PDB outside Symbols component: $relative"
    }
    if ($Component -eq 'Symbols' -and $entry.Extension -ine '.pdb' -and
        -not $requiredSet.Contains($relative)) {
        Add-Violation "non-PDB file in Symbols component: $relative"
    }
}

foreach ($required in $requiredPaths) {
    if (-not $relativeFiles.Contains($required)) {
        Add-Violation "missing required $Component component file: $required"
    }
}

if ($Component -ne 'Symbols') {
    $needles = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($path in @($workspace, $workspace.Replace('\', '/'))) {
        if ($path) { [void]$needles.Add($path) }
    }
    $userProfile = [Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)
    if ($userProfile) {
        $trimmedProfile = $userProfile.TrimEnd('\', '/')
        [void]$needles.Add($trimmedProfile)
        [void]$needles.Add($trimmedProfile.Replace('\', '/'))
    }

    $decodings = [Collections.Generic.List[Text.Encoding]]::new()
    $encodingCodePages = [Collections.Generic.HashSet[int]]::new()
    foreach ($encoding in @(
            [Text.Encoding]::UTF8,
            [Text.Encoding]::Unicode,
            [Text.Encoding]::BigEndianUnicode)) {
        if ($encodingCodePages.Add($encoding.CodePage)) { $decodings.Add($encoding) }
    }
    try {
        $ansiCodePage = [Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage
        $ansi = [Text.Encoding]::GetEncoding($ansiCodePage)
        if ($encodingCodePages.Add($ansi.CodePage)) { $decodings.Add($ansi) }
    } catch {
        # UTF-8 and UTF-16 scans remain active when the legacy code page is unavailable.
    }

    foreach ($file in $files) {
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $matchedNeedle = $null
        foreach ($encoding in $decodings) {
            $decoded = $encoding.GetString($bytes)
            foreach ($needle in $needles) {
                if ($decoded.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    $matchedNeedle = $needle
                    break
                }
            }
            if ($matchedNeedle) { break }
        }
        if ($matchedNeedle) {
            $relative = [IO.Path]::GetRelativePath($package, $file.FullName).Replace('\', '/')
            Add-Violation "absolute build/user path '$matchedNeedle' in $relative"
        }
    }
}

$inventory = @($files | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{
        Path = [IO.Path]::GetRelativePath($package, $_.FullName).Replace('\', '/')
        Size = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$report = [pscustomobject]@{
    SchemaVersion = 1
    PolicyVersion = 1
    Component = $Component
    Passed = $violations.Count -eq 0
    FileCount = $files.Count
    Violations = @($violations)
    Inventory = $inventory
    SymbolsPathPrivacyNote = if ($Component -eq 'Symbols') {
        'PDBs are distributed separately and may contain compiler/debug source paths.'
    } else { $null }
}
if ($ReportPath) {
    $reportFile = [IO.Path]::GetFullPath($ReportPath)
    $parent = [IO.Path]::GetDirectoryName($reportFile)
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $report | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $reportFile -Encoding utf8NoBOM
}
if ($violations.Count -ne 0) {
    throw "$Component package audit failed:`n - $($violations -join "`n - ")"
}
$report
