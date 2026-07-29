param(
    [Parameter(Mandatory)]
    [ValidateRange(1, 2147483647)]
    [int]$ProcessId,
    [string]$ModuleName = 'HTGame.exe',
    [string]$GameId = 'nte',
    [string]$Cli = (Join-Path $PSScriptRoot '..\.build\windows-vs2022\bin\RelWithDebInfo\anomaly-cli.exe'),
    [string]$ProfileTool = (Join-Path $PSScriptRoot '..\.build\windows-vs2022\bin\RelWithDebInfo\anomaly-profile.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-JsonCommand {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    $lines = @(& $Executable @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
    $text = [string]::Join("`n", $lines).Trim()
    $document = $null
    $parseError = $null
    if (-not [string]::IsNullOrWhiteSpace($text)) {
        try {
            $document = $text | ConvertFrom-Json
        } catch {
            $parseError = $_.Exception.Message
        }
    }
    return [pscustomobject][ordered]@{
        ExitCode = $exitCode
        Document = $document
        Raw = $text
        ParseError = $parseError
    }
}

$resolvedCli = [IO.Path]::GetFullPath($Cli)
$resolvedProfileTool = [IO.Path]::GetFullPath($ProfileTool)
if (-not (Test-Path -LiteralPath $resolvedCli -PathType Leaf)) {
    throw "anomaly CLI not found: $resolvedCli"
}
if (-not (Test-Path -LiteralPath $resolvedProfileTool -PathType Leaf)) {
    throw "anomaly profile tool not found: $resolvedProfileTool"
}

$pidText = $ProcessId.ToString([Globalization.CultureInfo]::InvariantCulture)
$statusResult = Invoke-JsonCommand $resolvedCli @('--pid', $pidText, 'status')
$modulesResult = Invoke-JsonCommand $resolvedCli @('--pid', $pidText, 'modules')
$ueResult = Invoke-JsonCommand $resolvedCli @('--pid', $pidText, 'ue')

$module = $null
if ($null -ne $modulesResult.Document -and $null -ne $modulesResult.Document.modules) {
    $module = @($modulesResult.Document.modules) | Where-Object {
        [string]::Equals($_.name, $ModuleName, [StringComparison]::OrdinalIgnoreCase)
    } | Select-Object -First 1
}

$sectionsResult = $null
$fingerprintResult = $null
$file = $null
if ($null -ne $module) {
    $sectionsResult = Invoke-JsonCommand $resolvedCli @(
        '--pid', $pidText, 'sections', [string]$module.name)
    if (Test-Path -LiteralPath ([string]$module.path) -PathType Leaf) {
        $file = Get-Item -LiteralPath ([string]$module.path)
        $fingerprintResult = Invoke-JsonCommand $resolvedProfileTool @(
            'fingerprint', $file.FullName, $GameId)
    }
}

$memoryText = $null
if ($null -ne $sectionsResult -and $null -ne $sectionsResult.Document -and
    $null -ne $sectionsResult.Document.sections) {
    $memoryText = @($sectionsResult.Document.sections) | Where-Object {
        [string]::Equals($_.name, '.text', [StringComparison]::Ordinal)
    } | Select-Object -First 1
}

$runtimeBuildId = if ($null -ne $ueResult.Document) {
    [string]$ueResult.Document.buildId
} else { '' }
$diskBuildId = if ($null -ne $fingerprintResult -and
    $null -ne $fingerprintResult.Document) {
    [string]$fingerprintResult.Document.id
} else { '' }

$signals = [Collections.Generic.List[string]]::new()
if ($null -eq $module) { $signals.Add('module-not-enumerated') }
if ($null -ne $module -and $null -eq $memoryText) { $signals.Add('memory-text-section-missing') }
if ($null -ne $module -and $null -eq $fingerprintResult) {
    $signals.Add('module-file-not-readable')
} elseif ($null -ne $fingerprintResult -and $fingerprintResult.ExitCode -ne 0) {
    $signals.Add('disk-fingerprint-failed')
}
if (-not [string]::IsNullOrEmpty($diskBuildId) -and
    [string]::IsNullOrEmpty($runtimeBuildId)) {
    $signals.Add('runtime-fingerprint-stale-or-startup-only-failure')
}
if (-not [string]::IsNullOrEmpty($diskBuildId) -and
    -not [string]::IsNullOrEmpty($runtimeBuildId) -and
    -not [string]::Equals($diskBuildId, $runtimeBuildId, [StringComparison]::Ordinal)) {
    $signals.Add('runtime-and-disk-build-id-differ')
}

$report = [pscustomobject][ordered]@{
    SchemaVersion = 1
    ProcessId = $ProcessId
    ModuleName = $ModuleName
    Module = if ($null -eq $module) { $null } else {
        [pscustomobject][ordered]@{
            Name = [string]$module.name
            Path = [string]$module.path
            Base = [string]$module.base
            ImageSize = [long]$module.size
            FileSize = if ($null -eq $file) { $null } else { $file.Length }
        }
    }
    Runtime = [pscustomobject][ordered]@{
        Status = $statusResult.Document
        Ue = $ueResult.Document
        BuildId = $runtimeBuildId
    }
    Memory = [pscustomobject][ordered]@{
        TextSection = $memoryText
        Sections = if ($null -eq $sectionsResult) { $null } else {
            $sectionsResult.Document
        }
    }
    Disk = [pscustomobject][ordered]@{
        Fingerprint = if ($null -eq $fingerprintResult) { $null } else {
            $fingerprintResult.Document
        }
        BuildId = $diskBuildId
        Error = if ($null -eq $fingerprintResult -or $fingerprintResult.ExitCode -eq 0) {
            $null
        } else { $fingerprintResult.Raw }
    }
    BuildIdsMatch = -not [string]::IsNullOrEmpty($runtimeBuildId) -and
        [string]::Equals($runtimeBuildId, $diskBuildId, [StringComparison]::Ordinal)
    Signals = @($signals)
}

$report | ConvertTo-Json -Depth 100
