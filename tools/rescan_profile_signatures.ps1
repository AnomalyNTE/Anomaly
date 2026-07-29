<#
.SYNOPSIS
Rescans every signature from a Profile after the target program updates.

.DESCRIPTION
Scans a running process with the patterns from a known-good profile and records
instruction/target addresses and RVAs. When every symbol has exactly one match,
the script writes a candidate Profile and validates it with anomaly-profile.exe.
The PE fingerprint is included only in the report and output filename. The source
Profile is never modified.

.EXAMPLE
.\tools\rescan_profile_signatures.ps1 `
    -ProfilePath .\profiles\nte\nte-previous.json

.EXAMPLE
.\tools\rescan_profile_signatures.ps1 -ProcessId 1234 `
    -ProfilePath .\profiles\nte\nte-previous.json `
    -OutputDirectory .\data\signature-scans
#>
[CmdletBinding()]
param(
    [ValidateRange(0, [int]::MaxValue)]
    [int]$ProcessId = 0,

    [string]$ProcessName = 'HTGame',

    [Parameter(Mandatory)]
    [string]$ProfilePath,

    [string]$Cli = '',

    [string]$ProfileTool = '',

    [string]$OutputDirectory = '',

    [switch]$SkipCandidateProfile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-ToolPath {
    param(
        [AllowEmptyString()][string]$ExplicitPath,
        [Parameter(Mandatory)][string]$FileName
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $resolved = [IO.Path]::GetFullPath($ExplicitPath)
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "Tool not found: $resolved"
        }
        return $resolved
    }

    $candidates = @(
        (Join-Path $PSScriptRoot "..\.build\windows-vs2022\bin\RelWithDebInfo\$FileName")
    )
    foreach ($candidate in $candidates) {
        $resolved = [IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath $resolved -PathType Leaf) {
            return $resolved
        }
    }
    throw "$FileName was not found; pass its path explicitly"
}

function Invoke-JsonTool {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    $lines = @(& $Executable @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
    $text = [string]::Join([Environment]::NewLine, $lines).Trim()
    if ($exitCode -ne 0) {
        throw "$([IO.Path]::GetFileName($Executable)) exited with code ${exitCode}: $text"
    }
    try {
        $document = $text | ConvertFrom-Json
    } catch {
        throw "$([IO.Path]::GetFileName($Executable)) returned invalid JSON: $text"
    }
    if ($document.PSObject.Properties.Name -contains 'ok' -and -not $document.ok) {
        throw [string]$document.error
    }
    return $document
}

function Convert-HexToUInt64 {
    param([Parameter(Mandatory)][string]$Value)

    $text = $Value.Trim()
    if ($text.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
        $text = $text.Substring(2)
    }
    return [Convert]::ToUInt64($text, 16)
}

function Format-Hex {
    param([Parameter(Mandatory)][UInt64]$Value)
    return '0x{0:X}' -f $Value
}

function Add-SignedAddress {
    param(
        [Parameter(Mandatory)][UInt64]$Address,
        [Parameter(Mandatory)][Int64]$Addend
    )

    if ($Addend -ge 0) {
        return [UInt64]($Address + [UInt64]$Addend)
    }
    return [UInt64]($Address - [UInt64](-$Addend))
}

function Find-ModuleForAddress {
    param(
        [Parameter(Mandatory)][UInt64]$Address,
        [Parameter(Mandatory)][object[]]$Modules
    )

    foreach ($module in $Modules) {
        $base = Convert-HexToUInt64 ([string]$module.base)
        $size = [UInt64]$module.size
        if ($Address -ge $base -and $Address -lt ($base + $size)) {
            return [pscustomobject]@{
                Name = [string]$module.name
                Rva = Format-Hex ($Address - $base)
            }
        }
    }
    return $null
}

$resolvedProfile = [IO.Path]::GetFullPath($ProfilePath)
if (-not (Test-Path -LiteralPath $resolvedProfile -PathType Leaf)) {
    throw "Base profile not found: $resolvedProfile"
}
try {
    $profile = Get-Content -LiteralPath $resolvedProfile -Raw -Encoding utf8 |
        ConvertFrom-Json
} catch {
    throw "Base profile is not valid JSON: $($_.Exception.Message)"
}
if ($null -eq $profile.symbols -or @($profile.symbols.PSObject.Properties).Count -eq 0) {
    throw 'Base profile does not contain any symbols'
}
$primaryModule = [string](@($profile.symbols.PSObject.Properties)[0].Value.module)
if ([string]::IsNullOrWhiteSpace($primaryModule)) {
    throw 'Base profile symbols do not declare a module'
}

$resolvedCli = Resolve-ToolPath $Cli 'anomaly-cli.exe'
$resolvedProfileTool = Resolve-ToolPath $ProfileTool 'anomaly-profile.exe'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\data\signature-scans'
}
$resolvedOutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

if ($ProcessId -eq 0) {
    $process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
    if ($null -eq $process) {
        throw "$ProcessName is not running; pass -ProcessId after it starts"
    }
    $ProcessId = $process.Id
}

$pidText = $ProcessId.ToString([Globalization.CultureInfo]::InvariantCulture)
$moduleResponse = Invoke-JsonTool $resolvedCli @('--pid', $pidText, 'modules')
$modules = @($moduleResponse.modules)
$mainModule = $modules | Where-Object {
    [string]::Equals(
        [string]$_.name, $primaryModule,
        [StringComparison]::OrdinalIgnoreCase)
} | Select-Object -First 1
if ($null -eq $mainModule) {
    throw "Profile module '$primaryModule' is not loaded in process $ProcessId"
}
$modulePath = [string]$mainModule.path
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    throw "Loaded module file is not readable: $modulePath"
}

$fingerprint = Invoke-JsonTool $resolvedProfileTool @(
    'fingerprint', $modulePath, [string]$profile.game)
$results = [Collections.Generic.List[object]]::new()

foreach ($property in @($profile.symbols.PSObject.Properties)) {
    $id = [string]$property.Name
    $symbol = $property.Value
    $moduleName = [string]$symbol.module
    $loadedModule = $modules | Where-Object {
        [string]::Equals(
            [string]$_.name, $moduleName,
            [StringComparison]::OrdinalIgnoreCase)
    } | Select-Object -First 1

    if ($null -eq $loadedModule) {
        $results.Add([pscustomobject][ordered]@{
            Id = $id
            Status = 'module-not-loaded'
            Module = $moduleName
            Section = [string]$symbol.section
            Pattern = [string]$symbol.pattern
            MatchCount = 0
            Matches = @()
        })
        continue
    }

    try {
        $scan = Invoke-JsonTool $resolvedCli @(
            '--pid', $pidText, 'scan', $moduleName,
            [string]$symbol.section, [string]$symbol.pattern)
        $matches = @($scan.matches)
        $matchDetails = @()
        foreach ($matchText in $matches) {
            $instruction = Convert-HexToUInt64 ([string]$matchText)
            $moduleBase = Convert-HexToUInt64 ([string]$loadedModule.base)
            $resolvedAddress = $null
            $resolveError = $null
            try {
                $kind = [string]$symbol.resolve.kind
                $addend = if ($symbol.resolve.PSObject.Properties.Name -contains 'addend') {
                    [Int64]$symbol.resolve.addend
                } else { [Int64]0 }
                if ($kind -eq 'direct') {
                    $resolvedAddress = Add-SignedAddress $instruction $addend
                } elseif ($kind -eq 'rip-rel32') {
                    $rip = Invoke-JsonTool $resolvedCli @(
                        '--pid', $pidText, 'rip', (Format-Hex $instruction),
                        ([string]$symbol.resolve.offset),
                        ([string]$symbol.resolve.instructionSize))
                    $resolvedAddress = Add-SignedAddress `
                        (Convert-HexToUInt64 ([string]$rip.target)) $addend
                } else {
                    throw "Unsupported resolve kind '$kind'"
                }
            } catch {
                $resolveError = $_.Exception.Message
            }

            $targetLocation = if ($null -eq $resolvedAddress) {
                $null
            } else {
                Find-ModuleForAddress ([UInt64]$resolvedAddress) $modules
            }
            $matchDetails += [pscustomobject][ordered]@{
                Instruction = Format-Hex $instruction
                InstructionRva = Format-Hex ($instruction - $moduleBase)
                Address = if ($null -eq $resolvedAddress) {
                    $null
                } else { Format-Hex ([UInt64]$resolvedAddress) }
                AddressModule = if ($null -eq $targetLocation) {
                    $null
                } else { $targetLocation.Name }
                AddressRva = if ($null -eq $targetLocation) {
                    $null
                } else { $targetLocation.Rva }
                ResolveError = $resolveError
            }
        }

        $status = if ($matches.Count -eq 0) {
            'not-found'
        } elseif ($matches.Count -eq 1 -and $null -eq $matchDetails[0].ResolveError) {
            'unique'
        } elseif ($matches.Count -eq 1) {
            'resolve-failed'
        } else {
            'ambiguous'
        }
        $results.Add([pscustomobject][ordered]@{
            Id = $id
            Status = $status
            Module = $moduleName
            Section = [string]$symbol.section
            Pattern = [string]$symbol.pattern
            MatchCount = $matches.Count
            Matches = $matchDetails
        })
    } catch {
        $results.Add([pscustomobject][ordered]@{
            Id = $id
            Status = 'scan-failed'
            Module = $moduleName
            Section = [string]$symbol.section
            Pattern = [string]$symbol.pattern
            MatchCount = 0
            Matches = @()
            Error = $_.Exception.Message
        })
    }
}

$uniqueCount = @($results | Where-Object Status -eq 'unique').Count
$allUnique = $uniqueCount -eq $results.Count
$timestamp = [DateTime]::UtcNow
$stamp = $timestamp.ToString('yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Path $resolvedOutputDirectory -Force | Out-Null
$reportPath = Join-Path $resolvedOutputDirectory "signature-scan-$stamp.json"
$candidatePath = $null
$validation = $null

if ($allUnique -and -not $SkipCandidateProfile) {
    $candidatePath = Join-Path $resolvedOutputDirectory "$($fingerprint.id).candidate.json"
    $profile | ConvertTo-Json -Depth 100 |
        Set-Content -LiteralPath $candidatePath -Encoding utf8

    $validationLines = @(& $resolvedProfileTool validate $candidatePath 2>&1 |
        ForEach-Object { [string]$_ })
    $validation = [pscustomobject][ordered]@{
        ExitCode = $LASTEXITCODE
        Output = [string]::Join([Environment]::NewLine, $validationLines).Trim()
    }
    if ($validation.ExitCode -ne 0) {
        Remove-Item -LiteralPath $candidatePath -Force
        $candidatePath = $null
    }
}

$report = [pscustomobject][ordered]@{
    SchemaVersion = 1
    TimestampUtc = $timestamp.ToString('O')
    ProcessId = $ProcessId
    BaseProfile = $resolvedProfile
    ModulePath = $modulePath
    Fingerprint = $fingerprint
    Summary = [pscustomobject][ordered]@{
        SymbolCount = $results.Count
        Unique = $uniqueCount
        NotFound = @($results | Where-Object Status -eq 'not-found').Count
        Ambiguous = @($results | Where-Object Status -eq 'ambiguous').Count
        Failed = @($results | Where-Object {
            $_.Status -in @('module-not-loaded', 'scan-failed', 'resolve-failed')
        }).Count
        AllUnique = $allUnique
    }
    Symbols = @($results)
    CandidateProfile = $candidatePath
    CandidateValidation = $validation
}
$report | ConvertTo-Json -Depth 100 |
    Set-Content -LiteralPath $reportPath -Encoding utf8

[pscustomobject][ordered]@{
    ReportPath = $reportPath
    CandidateProfile = $candidatePath
    BuildId = [string]$fingerprint.id
    Summary = $report.Summary
} | ConvertTo-Json -Depth 10

if (-not $allUnique) {
    exit 2
}
