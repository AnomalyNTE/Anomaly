[CmdletBinding()]
param(
    [string]$BuildDirectory = '.build\windows-vs2022',
    [string]$Version = '1.0.0',
    [string]$OutputDirectory = '.build\release',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'RelWithDebInfo',
    [switch]$AllowDirtySource
)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7+ (pwsh.exe) is required; invoke this script with pwsh -NoProfile -File.'
}

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Get-WorkspacePath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $root $Path))
}

function Assert-WorkspaceDescendant([string]$Path, [string]$Name) {
    $relative = [IO.Path]::GetRelativePath($root, $Path)
    if ($relative -eq '.' -or [IO.Path]::IsPathRooted($relative) -or
        $relative -eq '..' -or $relative.StartsWith('..\') -or $relative.StartsWith('../')) {
        throw "$Name must remain in a child directory of the workspace"
    }
}

$semVerPattern = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$'
$semVer = [regex]::Match($Version, $semVerPattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
if (-not $semVer.Success) {
    throw "release version is not valid SemVer: $Version"
}
$versionCore = "$($semVer.Groups[1].Value).$($semVer.Groups[2].Value).$($semVer.Groups[3].Value)"

$build = Get-WorkspacePath $BuildDirectory
$output = Get-WorkspacePath $OutputDirectory
Assert-WorkspaceDescendant $build 'build directory'
Assert-WorkspaceDescendant $output 'release output directory'
if (-not (Test-Path -LiteralPath $build -PathType Container)) {
    throw "build directory not found: $build"
}

$cachePath = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache not found: $cachePath"
}
$cache = Get-Content -LiteralPath $cachePath -Raw
function Read-CMakeCacheValue([string]$Name) {
    $match = [regex]::Match(
        $cache, "(?m)^$([regex]::Escape($Name)):[^=]*=(.*)$",
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    return $null
}

$projectVersion = Read-CMakeCacheValue 'CMAKE_PROJECT_VERSION'
if (-not $projectVersion) { throw 'CMAKE_PROJECT_VERSION is missing from CMakeCache.txt' }
if ($projectVersion -ne $versionCore) {
    throw "release version $Version does not match configured project version $projectVersion"
}
$buildType = Read-CMakeCacheValue 'CMAKE_BUILD_TYPE'
if ($buildType -and $buildType -ne $Configuration) {
    throw "single-config build type $buildType does not match requested configuration $Configuration"
}
$configurationTypes = Read-CMakeCacheValue 'CMAKE_CONFIGURATION_TYPES'
if ($configurationTypes -and $Configuration -notin @($configurationTypes -split ';')) {
    throw "configuration $Configuration is not available in this build tree"
}

$cmake = 'cmake'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $vs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $candidate = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $candidate) { $cmake = $candidate }
}

$artifactScript = Join-Path $PSScriptRoot 'release_artifacts.ps1'
if (-not (Test-Path -LiteralPath $artifactScript -PathType Leaf)) {
    throw "release artifact script not found: $artifactScript"
}
. $artifactScript

$git = Get-Command git -ErrorAction Stop
$sourceCommit = (& $git.Source -C $root rev-parse --verify HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'release source commit could not be resolved'
}
$sourceStatus = @(& $git.Source -C $root status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw 'release source status could not be resolved' }
$sourceDirty = $sourceStatus.Count -ne 0
if ($sourceDirty -and -not $AllowDirtySource) {
    throw 'release source tree is dirty; commit the candidate before packaging'
}
$sourceCommitText = (& $git.Source -C $root show -s --format=%cI $sourceCommit).Trim()
if ($LASTEXITCODE -ne 0) { throw 'release source commit time could not be resolved' }
$sourceCommitUtc = [DateTimeOffset]::Parse(
    $sourceCommitText, [Globalization.CultureInfo]::InvariantCulture)

New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($generatedFile in @('SHA256SUMS.txt', 'release-manifest.json')) {
    $path = Join-Path $output $generatedFile
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}
$auditDirectory = Join-Path $output 'audit'
if (Test-Path -LiteralPath $auditDirectory) {
    Remove-Item -LiteralPath $auditDirectory -Recurse -Force
}
$sbomDirectory = Join-Path $output 'sbom'
if (Test-Path -LiteralPath $sbomDirectory) {
    Remove-Item -LiteralPath $sbomDirectory -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $sbomDirectory | Out-Null

$packages = @(
    [pscustomobject]@{
        InstallComponent = 'GameRuntime'; Component = 'Runtime'; Slug = 'runtime'
        Name = "Anomaly-$Version-runtime"
    },
    [pscustomobject]@{
        InstallComponent = 'SDK'; Component = 'SDK'; Slug = 'sdk'
        Name = "Anomaly-$Version-sdk"
    },
    [pscustomobject]@{
        InstallComponent = 'Tools'; Component = 'Tools'; Slug = 'tools'
        Name = "Anomaly-$Version-tools"
    },
    [pscustomobject]@{
        InstallComponent = 'Symbols'; Component = 'Symbols'; Slug = 'symbols'
        Name = "Anomaly-$Version-symbols"
    }
)

$stages = @{}
foreach ($package in $packages) {
    $stage = Join-Path $output $package.Name
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
    & $cmake --install $build --config $Configuration --prefix $stage `
        --component $package.InstallComponent
    if ($LASTEXITCODE -ne 0) { throw "install failed for $($package.InstallComponent)" }

    $stages[$package.Component] = $stage
}

# Each shipped PE must have exactly one corresponding linker PDB in the
# separately distributed Symbols component.
$expectedPdbNames = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($component in @('Runtime', 'SDK', 'Tools')) {
    Get-ChildItem -LiteralPath $stages[$component] -Recurse -Force -File |
        Where-Object { $_.Extension -ieq '.dll' -or $_.Extension -ieq '.exe' } |
        ForEach-Object {
            $relative = [IO.Path]::GetRelativePath(
                $stages[$component], $_.FullName).Replace('\', '/')
            $pdbName = if ($component -eq 'Runtime' -and
                $relative -match '(?i)^Anomaly/plugins/([^/]+)/plugin\.dll$') {
                "$($Matches[1]).pdb"
            } else {
                "$($_.BaseName).pdb"
            }
            [void]$expectedPdbNames.Add($pdbName)
        }
}
foreach ($sdkExamplePdb in @(
        'HelloUi.pdb', 'TickCounter.pdb', 'ReliableConfig.pdb', 'NteInspector.pdb')) {
    [void]$expectedPdbNames.Add($sdkExamplePdb)
}
$actualPdbNames = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
Get-ChildItem -LiteralPath $stages.Symbols -Recurse -Force -File |
    Where-Object { $_.Extension -ieq '.pdb' } |
    ForEach-Object { [void]$actualPdbNames.Add($_.Name) }
$missingPdb = @($expectedPdbNames | Where-Object { -not $actualPdbNames.Contains($_) } | Sort-Object)
$unexpectedPdb = @($actualPdbNames | Where-Object { -not $expectedPdbNames.Contains($_) } | Sort-Object)
if ($missingPdb.Count -ne 0 -or $unexpectedPdb.Count -ne 0) {
    throw "Symbols inventory does not match published PE files; missing=[$($missingPdb -join ', ')]; unexpected=[$($unexpectedPdb -join ', ')]"
}

$artifacts = @()
$sbomFiles = @()
foreach ($package in $packages) {
    $stage = $stages[$package.Component]
    $zip = "$stage.zip"
    New-DeterministicZip -SourceDirectory $stage -DestinationPath $zip
    $artifacts += Get-FileHash -LiteralPath $zip -Algorithm SHA256

    $sbomPath = Join-Path $sbomDirectory "$($package.Slug).spdx.json"
    New-SpdxSbom `
        -PackageDirectory $stage `
        -Component $package.Component `
        -Version $Version `
        -SourceCommit $sourceCommit `
        -SourceCommitUtc $sourceCommitUtc `
        -OutputPath $sbomPath
    $sbomFiles += Get-Item -LiteralPath $sbomPath
}

$publishedFiles = @(
    $artifacts | ForEach-Object { Get-Item -LiteralPath $_.Path }
    $sbomFiles
)
$published = [object[]]@($publishedFiles | ForEach-Object {
    [pscustomobject]@{
        Path = $_.FullName
        RelativePath = [IO.Path]::GetRelativePath($output, $_.FullName).Replace('\', '/')
        Size = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
[Array]::Sort($published, [Comparison[object]]{
    param($left, $right)
    [StringComparer]::Ordinal.Compare(
        [string]$left.RelativePath, [string]$right.RelativePath)
})
$checksumPath = Join-Path $output 'SHA256SUMS.txt'
$published | ForEach-Object {
    "$($_.Sha256)  $($_.RelativePath)"
} | Set-Content -LiteralPath $checksumPath -Encoding ascii

$expected = @{}
Get-Content -LiteralPath $checksumPath | ForEach-Object {
    if ($_ -notmatch '^([0-9a-f]{64})  (.+)$') { throw "invalid SHA256SUMS line: $_" }
    $expected[$Matches[2]] = $Matches[1]
}
if ($expected.Count -ne $published.Count) {
    throw 'release checksum inventory count mismatch'
}
foreach ($file in $published) {
    $actual = (Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($expected[$file.RelativePath] -ne $actual) {
        throw "release checksum verification failed: $($file.RelativePath)"
    }
}

[pscustomobject]@{
    SchemaVersion = 3
    Version = $Version
    ProjectVersion = $projectVersion
    Configuration = $Configuration
    GeneratedUtc = $sourceCommitUtc.ToUniversalTime().ToString('o')
    Source = [pscustomobject]@{
        Commit = $sourceCommit
        CommitUtc = $sourceCommitUtc.ToUniversalTime().ToString('o')
        Dirty = $sourceDirty
    }
    Reproducibility = [pscustomobject]@{
        Scope = 'identical staged bytes'
        ArchiveFormat = 'ZIP'
        EntryOrder = 'ordinal relative path'
        EntryTimestamp = '1980-01-01T00:00:00 (ZIP DOS timestamp)'
    }
    Artifacts = @($artifacts | ForEach-Object {
        [pscustomobject]@{
            Name = [IO.Path]::GetFileName($_.Path)
            Size = (Get-Item -LiteralPath $_.Path).Length
            Sha256 = $_.Hash.ToLowerInvariant()
        }
    })
    Sboms = @($published | Where-Object { $_.RelativePath -like 'sbom/*' } |
        Select-Object RelativePath, Size, Sha256)
    Checksums = 'SHA256SUMS.txt'
} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $output 'release-manifest.json') -Encoding utf8NoBOM

$artifacts | Select-Object Path, Hash | Format-Table -AutoSize
