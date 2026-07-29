[CmdletBinding()]
param(
    [string]$BuildDirectory = '.build\windows-vs2022',
    [double]$IdleHours = 4,
    [double]$PluginHours = 4,
    [switch]$Quick,
    [string]$OutputDirectory = '.build\stability'
)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7+ (pwsh.exe) is required; invoke this script with pwsh -NoProfile -File.'
}

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if (-not $build.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or
    -not $output.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'stability paths must remain inside the workspace'
}
if ($IdleHours -lt 0 -or $IdleHours -gt 168 -or $PluginHours -lt 0 -or $PluginHours -gt 168) {
    throw 'soak durations must be between 0 and 168 hours'
}

function Find-BuildArtifact([string[]]$RelativeCandidates) {
    foreach ($relative in $RelativeCandidates) {
        $candidate = Join-Path $build $relative
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    throw "build artifact not found: $($RelativeCandidates -join ', ')"
}

function Find-BuildDirectory([string[]]$RelativeCandidates) {
    foreach ($relative in $RelativeCandidates) {
        $candidate = Join-Path $build $relative
        if (Test-Path -LiteralPath $candidate -PathType Container) { return $candidate }
    }
    throw "build directory not found: $($RelativeCandidates -join ', ')"
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$log = Join-Path $output "stability-$stamp.log"
$results = [Collections.Generic.List[object]]::new()

function Invoke-Gate([string]$Name, [string]$Executable, [string[]]$Arguments) {
    $started = (Get-Date).ToUniversalTime()
    "[$($started.ToString('o'))] START $Name`n$Executable $($Arguments -join ' ')" |
        Tee-Object -FilePath $log -Append | Write-Host
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $log -Append | Write-Host
    $exitCode = $LASTEXITCODE
    $ended = (Get-Date).ToUniversalTime()
    $results.Add([pscustomobject]@{
        Name = $Name
        StartedUtc = $started.ToString('o')
        EndedUtc = $ended.ToString('o')
        DurationSeconds = [math]::Round(($ended - $started).TotalSeconds, 3)
        ExitCode = $exitCode
    })
    if ($exitCode -ne 0) { throw "$Name failed with exit code $exitCode" }
}

$performance = Find-BuildArtifact @(
    'bin\anomaly-performance-tests.exe',
    'bin\RelWithDebInfo\anomaly-performance-tests.exe')
$testHost = Find-BuildArtifact @(
    'bin\anomaly-test-host.exe',
    'bin\RelWithDebInfo\anomaly-test-host.exe')
$renderFixture = Find-BuildArtifact @(
    'bin\anomaly-render-fixture.exe',
    'bin\RelWithDebInfo\anomaly-render-fixture.exe')
$helloUi = Find-BuildDirectory @(
    'bin\sdk-examples\HelloUi',
    'bin\RelWithDebInfo\sdk-examples\HelloUi')

$idleMilliseconds = if ($Quick) { 1000 } else { [int64]($IdleHours * 60 * 60 * 1000) }
$idleBudget = if ($Quick) { '2.0' } else { '0.5' }
if ($idleMilliseconds -gt 0) {
    Invoke-Gate 'idle-runtime-budget' $performance @(
        '--idle-window-ms', $idleMilliseconds.ToString(),
        '--idle-budget-percent', $idleBudget)
}

$pluginSeconds = if ($Quick) { 5 } else { [int]($PluginHours * 60 * 60) }
if ($pluginSeconds -gt 0) {
    $reloadEveryTicks = if ($Quick) { 30 } else { 3600 }
    Invoke-Gate 'example-plugin-soak' $testHost @(
        '--plugin', $helloUi,
        '--duration-seconds', $pluginSeconds.ToString(),
        '--tick-interval-ms', '16',
        '--reload-every-ticks', $reloadEveryTicks.ToString())
}

$frames = if ($Quick) { 200 } else { 1000 }
$resizes = if ($Quick) { 10 } else { 50 }
$rebuilds = if ($Quick) { 4 } else { 10 }
Invoke-Gate 'render-window-continuity' $renderFixture @(
    '--frames', $frames.ToString(),
    '--resizes', $resizes.ToString(),
    '--rebuilds', $rebuilds.ToString())

$summary = Join-Path $output "stability-$stamp.json"
[pscustomobject]@{
    SchemaVersion = 1
    CompletedUtc = (Get-Date).ToUniversalTime().ToString('o')
    Quick = [bool]$Quick
    IdleHours = $IdleHours
    PluginHours = $PluginHours
    Results = $results
    LogSha256 = (Get-FileHash -LiteralPath $log -Algorithm SHA256).Hash.ToLowerInvariant()
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summary -Encoding utf8NoBOM
Write-Output $summary
