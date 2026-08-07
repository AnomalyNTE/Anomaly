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

$violations = [Collections.Generic.List[string]]::new()
$violationSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
function Add-Violation([string]$Message) {
    if ($violationSet.Add($Message)) { $violations.Add($Message) }
}

$forbiddenExtensions = @(
    '.lib', '.exp', '.obj', '.ilk', '.idb', '.pch', '.iobj', '.ipdb', '.map'
)
$forbiddenSegments = @(
    'CMakeFiles', '_deps', '.cache', 'cache', 'tmp', 'temp', 'tests'
)
foreach ($entry in $entries) {
    $relative = [IO.Path]::GetRelativePath($package, $entry.FullName).Replace('\', '/')
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Add-Violation "reparse point in package: $relative"
    }
    if ($entry.PSIsContainer) { continue }
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
    $isSymbolsMetadata = $relative -in @('LICENSE', 'NOTICE') -or
        ($relative.StartsWith('third_party/licenses/', [StringComparison]::OrdinalIgnoreCase) -and
         $entry.Extension -ieq '.txt')
    if ($Component -eq 'Symbols' -and $entry.Extension -ine '.pdb' -and
        -not $isSymbolsMetadata) {
        Add-Violation "non-PDB file in Symbols component: $relative"
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
