<#
.SYNOPSIS
Collects read-only evidence for NTE outbound network-path discovery.

.DESCRIPTION
Requires an already injected Anomaly Runtime with a ready validated NTE
Profile. The script calls only anomaly-cli status, ue, modules, sections, and
scan commands, then reports matching static strings from selected modules and
the target process's local UDP bindings. It never reads, captures, writes, or
saves network payloads.

Static strings and UDP bindings are discovery evidence only. They do not prove
that a function is active, identify a calling convention, or describe packet
fields.

.EXAMPLE
pwsh -NoProfile -File .\tools\scan_nte_outbound_anchors.ps1 -ProcessId 1234
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateRange(1, 2147483647)]
    [int]$ProcessId,

    [string]$Cli = (Join-Path $PSScriptRoot '..\.build\windows-vs2022\bin\RelWithDebInfo\anomaly-cli.exe'),

    [string]$Netstat = 'netstat.exe',

    [string[]]$ModuleName = @(
        'HTGame.exe',
        'HTGameBase.dll',
        'WPGameClientSDK_64.dll',
        'WPChatSDK_64.dll',
        'libcef.dll'
    )
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-AnalyzerJson {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    $lines = @(& $Executable @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
    $text = [string]::Join([Environment]::NewLine, $lines).Trim()
    if ($exitCode -ne 0) {
        throw "anomaly-cli exited with code ${exitCode}: $text"
    }

    try {
        $document = $text | ConvertFrom-Json
    } catch {
        throw "anomaly-cli returned invalid JSON: $text"
    }
    if (-not ($document.PSObject.Properties.Name -contains 'ok') -or -not $document.ok) {
        $message = if ($document.PSObject.Properties.Name -contains 'error') {
            [string]$document.error
        } else {
            $text
        }
        throw "anomaly-cli request failed: $message"
    }
    return $document
}

function Convert-AsciiPattern {
    param([Parameter(Mandatory)][string]$Text)

    return ([Text.Encoding]::ASCII.GetBytes($Text) |
        ForEach-Object { '{0:X2}' -f $_ }) -join ' '
}

function Get-LocalUdpBindings {
    param(
        [Parameter(Mandatory)][int]$TargetProcessId,
        [Parameter(Mandatory)][string]$Executable
    )

    $bindings = [Collections.Generic.List[object]]::new()
    $lines = @(& $Executable -ano -p UDP 2>&1 | ForEach-Object { [string]$_ })
    foreach ($line in $lines) {
        if ($line -notmatch '^\s*UDP\s+(?<local>\S+)\s+\*:\*\s+(?<pid>\d+)\s*$') {
            continue
        }
        if ([int]$Matches.pid -ne $TargetProcessId) {
            continue
        }
        $bindings.Add([pscustomobject][ordered]@{
            LocalEndpoint = [string]$Matches.local
        })
    }
    return @($bindings | Sort-Object LocalEndpoint -Unique)
}

$resolvedCli = [IO.Path]::GetFullPath($Cli)
if (-not (Test-Path -LiteralPath $resolvedCli -PathType Leaf)) {
    throw "anomaly CLI not found: $resolvedCli"
}

$pidText = $ProcessId.ToString([Globalization.CultureInfo]::InvariantCulture)
$status = Invoke-AnalyzerJson $resolvedCli @('--pid', $pidText, 'status')
$ue = Invoke-AnalyzerJson $resolvedCli @('--pid', $pidText, 'ue')
$modulesResponse = Invoke-AnalyzerJson $resolvedCli @('--pid', $pidText, 'modules')

if (-not [string]::Equals([string]$status.profile_game, 'nte', [StringComparison]::OrdinalIgnoreCase)) {
    throw "process $ProcessId is not configured for the NTE profile"
}
if (-not [string]::Equals([string]$ue.state, 'ready', [StringComparison]::OrdinalIgnoreCase) -or
    [string]::IsNullOrWhiteSpace([string]$ue.profileHash)) {
    throw 'outbound anchor scanning requires a ready validated NTE Profile'
}

$anchorGroups = [ordered]@{
    'ue-network' = @(
        'GameNetDriver',
        'PendingNetDriver',
        'BeaconNetDriver',
        'FlushNet',
        'PacketHandler',
        'FOutBunch',
        'LowLevelSend',
        'SendBunch',
        'ProcessRemoteFunction'
    )
    'transport-api' = @(
        'WSASend',
        'WSASendTo',
        'sendto',
        'WinHttpSendRequest',
        'WinHttpWriteData',
        'InternetWriteFile',
        'WebSocketSend'
    )
    'crypto-or-quic' = @(
        'SSL_write',
        'SSL_write_ex',
        'quic'
    )
}

$loadedModules = @($modulesResponse.modules)
$moduleReports = [Collections.Generic.List[object]]::new()
foreach ($requestedName in $ModuleName | Select-Object -Unique) {
    $loadedModule = $loadedModules | Where-Object {
        [string]::Equals([string]$_.name, $requestedName, [StringComparison]::OrdinalIgnoreCase)
    } | Select-Object -First 1
    if ($null -eq $loadedModule) {
        $moduleReports.Add([pscustomobject][ordered]@{
            Name = $requestedName
            Status = 'not-loaded'
            Base = $null
            ImageSize = $null
            RdataPresent = $false
            Anchors = @()
        })
        continue
    }

    $sections = Invoke-AnalyzerJson $resolvedCli @('--pid', $pidText, 'sections', [string]$loadedModule.name)
    $rdataPresent = $null -ne (@($sections.sections) | Where-Object {
        [string]::Equals([string]$_.name, '.rdata', [StringComparison]::Ordinal)
    } | Select-Object -First 1)
    $anchorMatches = [Collections.Generic.List[object]]::new()
    if ($rdataPresent) {
        foreach ($group in $anchorGroups.GetEnumerator()) {
            foreach ($anchor in @($group.Value)) {
                $scan = Invoke-AnalyzerJson $resolvedCli @(
                    '--pid', $pidText, 'scan', [string]$loadedModule.name,
                    '.rdata', (Convert-AsciiPattern $anchor))
                $anchorMatches.Add([pscustomobject][ordered]@{
                    Group = [string]$group.Key
                    Anchor = $anchor
                    MatchCount = [int]$scan.count
                    Matches = @($scan.matches | ForEach-Object { [string]$_ })
                })
            }
        }
    }

    $moduleReports.Add([pscustomobject][ordered]@{
        Name = [string]$loadedModule.name
        Status = if ($rdataPresent) { 'scanned' } else { 'rdata-missing' }
        Base = [string]$loadedModule.base
        ImageSize = [UInt64]$loadedModule.size
        RdataPresent = $rdataPresent
        Anchors = @($anchorMatches)
    })
}

$ueNetworkHits = @(
    foreach ($module in $moduleReports) {
        foreach ($anchor in @($module.Anchors)) {
            if ($anchor.Group -eq 'ue-network' -and $anchor.MatchCount -gt 0) {
                [pscustomobject]@{ Module = $module.Name; Anchor = $anchor.Anchor }
            }
        }
    }
)
$udpBindings = Get-LocalUdpBindings $ProcessId $Netstat
$classification = if ($udpBindings.Count -gt 0 -and $ueNetworkHits.Count -gt 0) {
    'udp-and-ue-network-evidence'
} elseif ($udpBindings.Count -gt 0) {
    'udp-bindings-only'
} elseif ($ueNetworkHits.Count -gt 0) {
    'ue-network-strings-only'
} else {
    'no-selected-anchor-evidence'
}

[pscustomobject][ordered]@{
    SchemaVersion = 1
    TimestampUtc = [DateTime]::UtcNow.ToString('O')
    ProcessId = $ProcessId
    Runtime = [pscustomobject][ordered]@{
        BuildId = [string]$ue.buildId
        ProfileHash = [string]$ue.profileHash
        ProfileChannel = [string]$ue.profileChannel
        ProfileState = [string]$ue.state
        AdapterStarted = [bool]$ue.adapterStarted
    }
    Scope = [pscustomobject][ordered]@{
        RequestedModules = @($ModuleName | Select-Object -Unique)
        StaticSection = '.rdata'
        PayloadCapture = $false
    }
    UdpBindings = $udpBindings
    Modules = @($moduleReports)
    Summary = [pscustomobject][ordered]@{
        Classification = $classification
        UdpBindingCount = $udpBindings.Count
        UeNetworkAnchorCount = $ueNetworkHits.Count
        PacketCaptureAvailable = $false
        SchemaRecoveryAvailable = $false
    }
    Limitations = @(
        'Static strings and UDP bindings do not prove an active send call site.',
        'anomaly-cli currently has no packet-event stream or payload capture command.',
        'No payload bytes, remote endpoints, credentials, or packet schema are captured by this tool.',
        'A profile-gated UE send hook needs a unique symbol, validator, and verified ABI before it can claim logical packet coverage.'
    )
} | ConvertTo-Json -Depth 16
