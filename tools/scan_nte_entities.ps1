param(
    [int]$ProcessId = 0,

    [string]$Cli = '',

    [int]$SampleDelayMs = 750,

    [int]$ThrottleLimit = 24,

    [string]$OutputDirectory = ''
)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    $pwsh = (Get-Command pwsh.exe -ErrorAction Stop).Source
    $arguments = @('-NoProfile', '-File', $PSCommandPath)
    if ($ProcessId -gt 0) { $arguments += @('-ProcessId', $ProcessId) }
    if ($Cli) { $arguments += @('-Cli', $Cli) }
    $arguments += @('-SampleDelayMs', $SampleDelayMs, '-ThrottleLimit', $ThrottleLimit)
    if ($OutputDirectory) { $arguments += @('-OutputDirectory', $OutputDirectory) }
    & $pwsh @arguments
    exit $LASTEXITCODE
}

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($ProcessId -lt 0 -or $SampleDelayMs -lt 100 -or $SampleDelayMs -gt 10000 -or
    $ThrottleLimit -lt 1 -or $ThrottleLimit -gt 64) {
    throw 'Invalid ProcessId, SampleDelayMs, or ThrottleLimit'
}

if ($ProcessId -eq 0) {
    $target = Get-Process -Name 'HTGame' -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
    if ($null -eq $target) {
        throw 'HTGame is not running; pass -ProcessId explicitly after it starts'
    }
    $ProcessId = $target.Id
}

if (-not $Cli) {
    $Cli = Join-Path $PSScriptRoot '..\.build\windows-vs2022\bin\RelWithDebInfo\anomaly-cli.exe'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\data\entity-scans'
}

$resolvedCli = [IO.Path]::GetFullPath($Cli)
if (-not (Test-Path -LiteralPath $resolvedCli -PathType Leaf)) {
    throw "Analyzer client not found: $resolvedCli"
}

function Convert-HexToUInt64 {
    param([Parameter(Mandatory)][string]$Value)

    $text = $Value.Trim()
    if ($text.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
        $text = $text.Substring(2)
    }
    return [Convert]::ToUInt64($text, 16)
}

function Invoke-Analyzer {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Command)

    $lines = @(& $resolvedCli --pid $ProcessId @Command 2>&1 |
        ForEach-Object { [string]$_ })
    if ($LASTEXITCODE -ne 0) {
        throw ($lines -join [Environment]::NewLine)
    }
    $result = ($lines -join [Environment]::NewLine) | ConvertFrom-Json
    if (-not $result.ok) {
        throw [string]$result.error
    }
    return $result
}

function Read-Pointer {
    param([Parameter(Mandatory)][UInt64]$Address)

    return Convert-HexToUInt64 (Invoke-Analyzer ptr ('0x{0:X}' -f $Address)).pointer
}

function Read-Bytes {
    param(
        [Parameter(Mandatory)][UInt64]$Address,
        [Parameter(Mandatory)][ValidateRange(1, 4096)][int]$Size
    )

    $result = Invoke-Analyzer read ('0x{0:X}' -f $Address) $Size
    return [byte[]]($result.bytes -split ' ' | ForEach-Object {
        [Convert]::ToByte($_, 16)
    })
}

function Get-PropertyValue {
    param(
        [Parameter(Mandatory)][object]$Object,
        [Parameter(Mandatory)][string]$Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Get-RequiredLayoutValue {
    param(
        [Parameter(Mandatory)][object]$Layout,
        [Parameter(Mandatory)][string]$Name
    )

    $value = Get-PropertyValue $Layout $Name
    if ($null -eq $value) {
        throw "Active NTE profile is missing layout.$Name"
    }
    try {
        return [Int64]$value
    } catch {
        throw "Active NTE profile has an invalid layout.$Name"
    }
}

function Get-ValidatedNamePoolLayout {
    param([Parameter(Mandatory)][object]$Runtime)

    $profileSource = [string]$Runtime.profileSource
    if ([string]::IsNullOrWhiteSpace($profileSource) -or
        -not (Test-Path -LiteralPath $profileSource -PathType Leaf)) {
        throw 'The active NTE profile source is unavailable'
    }

    try {
        $profile = Get-Content -LiteralPath $profileSource -Raw -Encoding utf8 | ConvertFrom-Json -Depth 32
    } catch {
        throw "Unable to parse the active NTE profile: $profileSource"
    }

    if (-not [string]::Equals([string]$profile.game, 'nte', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The active profile is not an NTE profile'
    }

    $expectedHash = [string]$Runtime.profileHash
    $actualHash = (Get-FileHash -LiteralPath $profileSource -Algorithm SHA256).Hash
    if ([string]::IsNullOrWhiteSpace($expectedHash) -or
        -not [string]::Equals($actualHash, $expectedHash, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The active profile changed after runtime validation'
    }

    $profileSymbols = Get-PropertyValue $profile 'symbols'
    $profileNamePool = if ($null -eq $profileSymbols) {
        $null
    } else {
        Get-PropertyValue $profileSymbols 'ue5.FNamePool'
    }
    if ($null -eq $profileNamePool -or
        -not (@(Get-PropertyValue $profileNamePool 'validators') -contains 'name-pool-v1')) {
        throw 'The active profile does not validate ue5.FNamePool with name-pool-v1'
    }

    $profileFeatures = Get-PropertyValue $profile 'features'
    $nameFeature = if ($null -eq $profileFeatures) {
        @()
    } else {
        @(Get-PropertyValue $profileFeatures 'ue5.names')
    }
    if ($nameFeature -notcontains 'ue5.FNamePool') {
        throw 'The active profile does not gate ue5.names on ue5.FNamePool'
    }

    $layout = Get-PropertyValue $profile 'layout'
    if ($null -eq $layout) {
        throw 'The active profile has no layout'
    }

    $nameLayout = [pscustomobject][ordered]@{
        BlocksOffset = Get-RequiredLayoutValue $layout 'names.blocksOffset'
        BlockBits = Get-RequiredLayoutValue $layout 'names.blockBits'
        EntryStride = Get-RequiredLayoutValue $layout 'names.entryStride'
        HeaderLengthShift = Get-RequiredLayoutValue $layout 'names.headerLengthShift'
        ObjectClassOffset = Get-RequiredLayoutValue $layout 'object.class'
        ObjectNameOffset = Get-RequiredLayoutValue $layout 'object.nameOffset'
    }
    if ($nameLayout.BlocksOffset -lt 0 -or $nameLayout.BlockBits -le 0 -or
        $nameLayout.BlockBits -ge 31 -or $nameLayout.EntryStride -le 0 -or
        $nameLayout.HeaderLengthShift -le 0 -or $nameLayout.HeaderLengthShift -ge 16 -or
        $nameLayout.ObjectClassOffset -lt 0 -or $nameLayout.ObjectNameOffset -lt 0 -or
        $nameLayout.ObjectClassOffset -gt 4096 -or $nameLayout.ObjectNameOffset -gt 4096) {
        throw 'The active profile has an invalid FName or UObject layout'
    }
    return $nameLayout
}

$ue = Invoke-Analyzer ue
if (-not [string]::Equals([string]$ue.state, 'ready', [StringComparison]::OrdinalIgnoreCase) -or
    [string]::IsNullOrWhiteSpace([string]$ue.profileHash)) {
    throw 'Entity scanning requires a ready validated NTE profile'
}

$worldSymbol = @($ue.symbols | Where-Object id -eq 'ue5.GWorld') | Select-Object -First 1
$namePoolSymbol = @($ue.symbols | Where-Object id -eq 'ue5.FNamePool') | Select-Object -First 1
if ($null -eq $worldSymbol -or -not $worldSymbol.available -or
    [string]::IsNullOrWhiteSpace([string]$worldSymbol.address)) {
    throw 'ue5.GWorld is unavailable'
}
if ($null -eq $namePoolSymbol -or -not $namePoolSymbol.available -or
    [string]::IsNullOrWhiteSpace([string]$namePoolSymbol.address)) {
    throw 'ue5.FNamePool is unavailable'
}

$nameLayout = Get-ValidatedNamePoolLayout $ue
$namePool = Convert-HexToUInt64 $namePoolSymbol.address
$objectClassOffset = [int]$nameLayout.ObjectClassOffset
$objectNameOffset = [int]$nameLayout.ObjectNameOffset
$actorHeaderSize = [Math]::Max(464, [Math]::Max($objectClassOffset + 8, $objectNameOffset + 8))
if ($actorHeaderSize -gt 4096) {
    throw 'The active profile requires an unsupported actor header size'
}

$gworld = Convert-HexToUInt64 $worldSymbol.address
$world = Read-Pointer $gworld
$worldBytes = Read-Bytes $world 568
$level = [BitConverter]::ToUInt64($worldBytes, 0x30)
$gameInstance = [BitConverter]::ToUInt64($worldBytes, 0x230)

$localPawn = [UInt64]0
try {
    $players = Read-Bytes ($gameInstance + 0x38) 16
    $playersArray = [BitConverter]::ToUInt64($players, 0)
    if ($playersArray -ne 0 -and [BitConverter]::ToInt32($players, 8) -gt 0) {
        $localPlayer = Read-Pointer $playersArray
        $controller = Read-Pointer ($localPlayer + 0x30)
        $localPawn = Read-Pointer ($controller + 0x308)
    }
} catch {
    $localPawn = [UInt64]0
}

$levelBytes = Read-Bytes $level 176
$actorArray = [BitConverter]::ToUInt64($levelBytes, 0xA0)
$actorCount = [BitConverter]::ToInt32($levelBytes, 0xA8)
$actorCapacity = [BitConverter]::ToInt32($levelBytes, 0xAC)
if ($actorArray -eq 0 -or $actorCount -lt 1 -or $actorCount -gt 100000 -or
    $actorCapacity -lt $actorCount) {
    throw "Invalid actor array: 0x$($actorArray.ToString('X')), $actorCount/$actorCapacity"
}

$actorByteCount = $actorCount * 8
$actorBytes = [byte[]]::new($actorByteCount)
for ($offset = 0; $offset -lt $actorByteCount; $offset += 4096) {
    $size = [Math]::Min(4096, $actorByteCount - $offset)
    $chunk = Read-Bytes ($actorArray + $offset) $size
    [Array]::Copy($chunk, 0, $actorBytes, $offset, $size)
}

$actors = @(for ($index = 0; $index -lt $actorCount; ++$index) {
    $actor = [BitConverter]::ToUInt64($actorBytes, $index * 8)
    if ($actor -ne 0) {
        [pscustomobject]@{ Index = $index; Actor = $actor }
    }
})

$headers = @($actors | ForEach-Object -Parallel {
    $cliPath = $using:resolvedCli
    $targetPid = $using:ProcessId
    $actor = [UInt64]$_.Actor
    $classOffset = $using:objectClassOffset
    $nameOffset = $using:objectNameOffset
    $headerSize = $using:actorHeaderSize
    $raw = @(& $cliPath --pid $targetPid read ('0x{0:X}' -f $actor) $headerSize 2>$null)
    try {
        $result = ($raw -join [Environment]::NewLine) | ConvertFrom-Json
        if (-not $result.ok) { return }
        [byte[]]$bytes = $result.bytes -split ' ' | ForEach-Object {
            [Convert]::ToByte($_, 16)
        }
        if ($bytes.Length -lt $headerSize) { return }
        [pscustomobject]@{
            Index      = $_.Index
            Actor      = $actor
            Class      = [BitConverter]::ToUInt64($bytes, $classOffset)
            NameId     = [BitConverter]::ToUInt32($bytes, $nameOffset)
            NameNumber = [BitConverter]::ToUInt32($bytes, $nameOffset + 4)
            Root       = [BitConverter]::ToUInt64($bytes, 0x1C8)
        }
    } catch {
    }
} -ThrottleLimit $ThrottleLimit)

$classPointers = @($headers | ForEach-Object { [UInt64]$_.Class } |
    Where-Object { $_ -ne 0 } | Sort-Object -Unique)
$classHeaderSize = [Math]::Max(32, $objectNameOffset + 8)
$classHeaders = @($classPointers | ForEach-Object -Parallel {
    $cliPath = $using:resolvedCli
    $targetPid = $using:ProcessId
    $nameOffset = $using:objectNameOffset
    $headerSize = $using:classHeaderSize
    $class = [UInt64]$_
    $raw = @(& $cliPath --pid $targetPid read ('0x{0:X}' -f $class) $headerSize 2>$null)
    try {
        $result = ($raw -join [Environment]::NewLine) | ConvertFrom-Json
        if (-not $result.ok) { return }
        [byte[]]$bytes = $result.bytes -split ' ' | ForEach-Object {
            [Convert]::ToByte($_, 16)
        }
        if ($bytes.Length -lt $headerSize) { return }
        [pscustomobject]@{
            Class = $class
            NameId = [BitConverter]::ToUInt32($bytes, $nameOffset)
            NameNumber = [BitConverter]::ToUInt32($bytes, $nameOffset + 4)
        }
    } catch {
    }
} -ThrottleLimit $ThrottleLimit)

$classNameIds = @($classHeaders | ForEach-Object { [UInt32]$_.NameId } |
    Where-Object { $_ -ne 0 } | Sort-Object -Unique)
$nameBlockPointers = @{}
$nameEntries = [Collections.Generic.List[object]]::new()
$nameBlockBits = [int]$nameLayout.BlockBits
$nameBlockMask = (([UInt64]1 -shl $nameBlockBits) - [UInt64]1)
foreach ($nameId in $classNameIds) {
    $nameId64 = [UInt64]$nameId
    $blockIndex = $nameId64 -shr $nameBlockBits
    $blockKey = $blockIndex.ToString([Globalization.CultureInfo]::InvariantCulture)
    if (-not $nameBlockPointers.ContainsKey($blockKey)) {
        try {
            $blockSlot = $namePool + ($blockIndex * [UInt64]8) + [UInt64]$nameLayout.BlocksOffset
            $nameBlockPointers[$blockKey] = Read-Pointer $blockSlot
        } catch {
            $nameBlockPointers[$blockKey] = [UInt64]0
        }
    }
    $block = [UInt64]$nameBlockPointers[$blockKey]
    if ($block -eq 0) { continue }
    $entry = $block + (($nameId64 -band $nameBlockMask) * [UInt64]$nameLayout.EntryStride)
    $nameEntries.Add([pscustomobject]@{ NameId = [UInt32]$nameId; Entry = [UInt64]$entry })
}

$nameLengthShift = [int]$nameLayout.HeaderLengthShift
$decodedNames = @($nameEntries | ForEach-Object -Parallel {
    $cliPath = $using:resolvedCli
    $targetPid = $using:ProcessId
    $entry = [UInt64]$_.Entry
    $nameId = [UInt32]$_.NameId
    $lengthShift = $using:nameLengthShift
    $headerRaw = @(& $cliPath --pid $targetPid read ('0x{0:X}' -f $entry) 2 2>$null)
    try {
        $headerResult = ($headerRaw -join [Environment]::NewLine) | ConvertFrom-Json
        if (-not $headerResult.ok) { return }
        [byte[]]$headerBytes = $headerResult.bytes -split ' ' | ForEach-Object {
            [Convert]::ToByte($_, 16)
        }
        if ($headerBytes.Length -ne 2) { return }
        $header = [BitConverter]::ToUInt16($headerBytes, 0)
        $length = $header -shr $lengthShift
        if (($header -band 1) -ne 0 -or $length -eq 0 -or $length -gt 1024) { return }

        $valueRaw = @(& $cliPath --pid $targetPid read ('0x{0:X}' -f ($entry + 2)) $length 2>$null)
        $valueResult = ($valueRaw -join [Environment]::NewLine) | ConvertFrom-Json
        if (-not $valueResult.ok) { return }
        [byte[]]$valueBytes = $valueResult.bytes -split ' ' | ForEach-Object {
            [Convert]::ToByte($_, 16)
        }
        if ($valueBytes.Length -ne $length) { return }
        $value = [Text.UTF8Encoding]::new($false, $true).GetString($valueBytes)
        [pscustomobject]@{ NameId = $nameId; Value = $value }
    } catch {
    }
} -ThrottleLimit $ThrottleLimit)

$decodedNameById = @{}
foreach ($decodedName in $decodedNames) {
    $decodedNameById[[UInt32]$decodedName.NameId] = [string]$decodedName.Value
}
$classMetadataByPointer = @{}
foreach ($classHeader in $classHeaders) {
    $classKey = ([UInt64]$classHeader.Class).ToString('X', [Globalization.CultureInfo]::InvariantCulture)
    $nameId = [UInt32]$classHeader.NameId
    $classMetadataByPointer[$classKey] = [pscustomobject]@{
        NameId = $nameId
        NameNumber = [UInt32]$classHeader.NameNumber
        Name = $decodedNameById[$nameId]
    }
}

$rooted = @($headers | Where-Object Root -ne 0)
$firstSample = @($rooted | ForEach-Object -Parallel {
    $cliPath = $using:resolvedCli
    $targetPid = $using:ProcessId
    $root = [UInt64]$_.Root
    $raw = @(& $cliPath --pid $targetPid read ('0x{0:X}' -f ($root + 0xF4)) 76 2>$null)
    try {
        $result = ($raw -join [Environment]::NewLine) | ConvertFrom-Json
        [byte[]]$bytes = $result.bytes -split ' ' | ForEach-Object {
            [Convert]::ToByte($_, 16)
        }
        $x = [BitConverter]::ToDouble($bytes, 0x24)
        $y = [BitConverter]::ToDouble($bytes, 0x2C)
        $z = [BitConverter]::ToDouble($bytes, 0x34)
        if ([double]::IsFinite($x) -and [double]::IsFinite($y) -and
            [double]::IsFinite($z) -and [Math]::Abs($x) -lt 1e9 -and
            [Math]::Abs($y) -lt 1e9 -and [Math]::Abs($z) -lt 1e9) {
            [pscustomobject]@{
                Index = $_.Index; Actor = $_.Actor; Class = $_.Class
                NameId = $_.NameId; NameNumber = $_.NameNumber; Root = $root
                Mobility = [int]$bytes[0]; X = $x; Y = $y; Z = $z
            }
        }
    } catch {
    }
} -ThrottleLimit $ThrottleLimit)

$sampledActors = @($firstSample)
$movableActors = @($sampledActors | Where-Object Mobility -eq 2)
Start-Sleep -Milliseconds $SampleDelayMs
$secondSample = @($movableActors | ForEach-Object -Parallel {
    $cliPath = $using:resolvedCli
    $targetPid = $using:ProcessId
    $root = [UInt64]$_.Root
    $raw = @(& $cliPath --pid $targetPid f64 ('0x{0:X}' -f ($root + 0x118)) 3 2>$null)
    try {
        $result = ($raw -join [Environment]::NewLine) | ConvertFrom-Json
        [pscustomobject]@{
            Root = $root
            X = [double]$result.values[0]
            Y = [double]$result.values[1]
            Z = [double]$result.values[2]
        }
    } catch {
    }
} -ThrottleLimit $ThrottleLimit)

$secondByRoot = @{}
foreach ($sample in $secondSample) {
    $secondByRoot[[UInt64]$sample.Root] = $sample
}

$entities = @($sampledActors | Sort-Object Index | ForEach-Object {
    $second = $secondByRoot[[UInt64]$_.Root]
    $dx = if ($null -eq $second) { 0.0 } else { $second.X - $_.X }
    $dy = if ($null -eq $second) { 0.0 } else { $second.Y - $_.Y }
    $dz = if ($null -eq $second) { 0.0 } else { $second.Z - $_.Z }
    $displacement = [Math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
    $classKey = ([UInt64]$_.Class).ToString('X', [Globalization.CultureInfo]::InvariantCulture)
    $classMetadata = $classMetadataByPointer[$classKey]
    $className = if ($null -eq $classMetadata) { $null } else { [string]$classMetadata.Name }
    $mobilityName = switch ($_.Mobility) {
        0 { 'Static' }
        1 { 'Stationary' }
        2 { 'Movable' }
        default { 'Unknown' }
    }
    $isBankBox = -not [string]::IsNullOrWhiteSpace($className) -and
        $className.StartsWith('BankBox_', [StringComparison]::OrdinalIgnoreCase)
    [pscustomobject][ordered]@{
        ActorIndex = $_.Index
        Actor = '0x{0:X}' -f $_.Actor
        RootComponent = '0x{0:X}' -f $_.Root
        Class = '0x{0:X}' -f $_.Class
        ClassName = $className
        ClassNameId = if ($null -eq $classMetadata) { $null } else { $classMetadata.NameId }
        ClassNameNumber = if ($null -eq $classMetadata) { $null } else { $classMetadata.NameNumber }
        IsBankBox = $isBankBox
        NameId = $_.NameId
        NameNumber = $_.NameNumber
        IsLocalPlayer = $_.Actor -eq $localPawn
        Mobility = $_.Mobility
        MobilityName = $mobilityName
        Position = [ordered]@{ X = $_.X; Y = $_.Y; Z = $_.Z }
        SecondSample = if ($null -eq $second) {
            $null
        } else {
            [ordered]@{ X = $second.X; Y = $second.Y; Z = $second.Z }
        }
        Displacement = $displacement
        MovedDuringSample = $null -ne $second -and $displacement -gt 0.01
    }
})

$groups = @($entities | Group-Object -Property {
    if ([string]::IsNullOrWhiteSpace([string]$_.ClassName)) { $_.Class } else { $_.ClassName }
} | ForEach-Object {
    $first = $_.Group | Select-Object -First 1
    [pscustomobject][ordered]@{
        Class = $first.Class
        ClassName = $first.ClassName
        Count = $_.Count
        BankBoxCount = @($_.Group | Where-Object IsBankBox).Count
        StaticCount = @($_.Group | Where-Object Mobility -eq 0).Count
        StationaryCount = @($_.Group | Where-Object Mobility -eq 1).Count
        MovableCount = @($_.Group | Where-Object Mobility -eq 2).Count
        UnknownMobilityCount = @($_.Group | Where-Object {
            $_.Mobility -ne 0 -and $_.Mobility -ne 1 -and $_.Mobility -ne 2
        }).Count
        MovingCount = @($_.Group | Where-Object MovedDuringSample).Count
    }
} | Sort-Object -Property @{ Expression = 'Count'; Descending = $true }, 'ClassName')

$worldEnd = Read-Pointer $gworld
$levelEndBytes = Read-Bytes $level 176
$actorCountEnd = [BitConverter]::ToInt32($levelEndBytes, 0xA8)
$document = [ordered]@{
    SchemaVersion = 2
    TimestampUtc = [DateTime]::UtcNow.ToString('O')
    ProcessId = $ProcessId
    BuildId = [string]$ue.buildId
    ProfileHash = [string]$ue.profileHash
    Mode = 'hide-and-seek'
    SampleDelayMs = $SampleDelayMs
    Layout = [ordered]@{
        PersistentLevel = '0x30'; Actors = '0xA0'; RootComponent = '0x1C8'
        Mobility = '0xF4'; Location = '0x118'
        ObjectClass = '0x{0:X}' -f $objectClassOffset
        ObjectName = '0x{0:X}' -f $objectNameOffset
        NamePoolBlocks = '0x{0:X}' -f $nameLayout.BlocksOffset
        NamePoolBlockBits = $nameLayout.BlockBits
        NamePoolEntryStride = $nameLayout.EntryStride
        NamePoolHeaderLengthShift = $nameLayout.HeaderLengthShift
    }
    NamePool = [ordered]@{
        Address = '0x{0:X}' -f $namePool
        RuntimeSymbol = 'ue5.FNamePool'
    }
    Scene = [ordered]@{
        World = '0x{0:X}' -f $world
        Level = '0x{0:X}' -f $level
        ActorArray = '0x{0:X}' -f $actorArray
        ActorCountStart = $actorCount
        ActorCountEnd = $actorCountEnd
        WorldStable = $worldEnd -eq $world
    }
    Summary = [ordered]@{
        NonNullActors = $actors.Count
        ReadableHeaders = $headers.Count
        ValidCoordinates = $firstSample.Count
        RetainedActors = $entities.Count
        Static = @($entities | Where-Object Mobility -eq 0).Count
        Stationary = @($entities | Where-Object Mobility -eq 1).Count
        Movable = @($entities | Where-Object Mobility -eq 2).Count
        UnknownMobility = @($entities | Where-Object {
            $_.Mobility -ne 0 -and $_.Mobility -ne 1 -and $_.Mobility -ne 2
        }).Count
        DecodedClassNames = $decodedNameById.Count
        BankBoxes = @($entities | Where-Object IsBankBox).Count
        MovedDuringSample = @($entities | Where-Object MovedDuringSample).Count
    }
    LocalPlayer = if ($localPawn -eq 0) { $null } else { '0x{0:X}' -f $localPawn }
    ClassGroups = $groups
    Entities = $entities
}

$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
$path = Join-Path $resolvedOutput (
    'nte-hide-seek-{0}.json' -f [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
$document | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8

[pscustomobject]@{
    Path = $path
    Scene = $document.Scene
    Summary = $document.Summary
    LocalPlayer = $document.LocalPlayer
    LargestClasses = @($groups | Select-Object -First 10)
    BankBoxes = @($entities | Where-Object IsBankBox |
        Select-Object ActorIndex, Actor, ClassName, MobilityName, Position)
    Moving = @($entities | Where-Object MovedDuringSample |
        Select-Object ActorIndex, Actor, Class, ClassName, MobilityName, Position, Displacement)
} | ConvertTo-Json -Depth 7
