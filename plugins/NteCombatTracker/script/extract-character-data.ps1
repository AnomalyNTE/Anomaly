[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Write')]
    [switch]$Write,

    [Parameter(Mandatory, ParameterSetName = 'Check')]
    [switch]$Check,

    [Parameter()]
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$generatorName = 'extract-character-data.ps1'
$generatorVersion = 2
$sourceTables = @(
    'data/datatable/Character/DT_Character.json',
    'data/datatable/skill/DT_GameplayAbilityTipsData.json',
    'data/datatable/skill/DT_SkillDamageData.json',
    'data/datatable/skill/DT_GameplayEffectMappingData.json'
)
$sectionNames = @(
    'characters',
    'ability_tips',
    'skill_damage',
    'gameplay_effect_mapping'
)

function Read-StrictJson {
    param([Parameter(Mandatory)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required JSON input is unavailable: $Path"
    }
    $strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
    try {
        $content = $strictUtf8.GetString([System.IO.File]::ReadAllBytes($Path))
    } catch {
        throw "JSON input must be valid UTF-8: $Path"
    }
    $options = [System.Text.Json.JsonDocumentOptions]::new()
    $options.MaxDepth = 256
    $document = [System.Text.Json.JsonDocument]::Parse($content, $options)

    function Assert-UniqueProperties {
        param(
            [Parameter(Mandatory)] [System.Text.Json.JsonElement]$Element,
            [Parameter(Mandatory)] [string]$JsonPath
        )

        if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
            $seen = [System.Collections.Generic.HashSet[string]]::new(
                [System.StringComparer]::Ordinal)
            foreach ($property in $Element.EnumerateObject()) {
                if (-not $seen.Add($property.Name)) {
                    throw "Duplicate JSON key '$($property.Name)' at '$JsonPath' in '$Path'."
                }
                Assert-UniqueProperties -Element $property.Value `
                    -JsonPath "$JsonPath.$($property.Name)"
            }
        } elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
            $index = 0
            foreach ($item in $Element.EnumerateArray()) {
                Assert-UniqueProperties -Element $item -JsonPath "$JsonPath[$index]"
                ++$index
            }
        }
    }

    try {
        Assert-UniqueProperties -Element $document.RootElement -JsonPath '$'
    } finally {
        $document.Dispose()
    }
    return $content | ConvertFrom-Json -AsHashtable -Depth 256
}

function ConvertTo-DeterministicJson {
    param([Parameter(Mandatory)] [object]$Value)

    $text = $Value | ConvertTo-Json -Depth 100
    return $text.Replace("`r`n", "`n") + "`n"
}

function ConvertTo-Slug {
    param([Parameter(Mandatory)] [string]$Value)

    return $Value.ToLowerInvariant() -replace '[^a-z0-9]+', '-'
}

function Get-DeterministicRevision {
    param(
        [Parameter(Mandatory)] [string]$RepositoryRoot,
        [Parameter(Mandatory)] [string[]]$RelativePaths,
        [Parameter(Mandatory)] [int]$Version
    )

    $builder = [System.Text.StringBuilder]::new()
    $null = $builder.Append('nte-dict-v').Append($Version).Append("`n")
    foreach ($relative in $RelativePaths | Sort-Object) {
        $path = Join-Path $RepositoryRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required source table is unavailable: $relative"
        }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $null = $builder.Append($relative).Append("`n").Append($hash).Append("`n")
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($builder.ToString())
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $revisionHash = $sha.ComputeHash($bytes)
    } finally {
        $sha.Dispose()
    }
    return 'sha256:' + (
        [System.BitConverter]::ToString($revisionHash).Replace('-', '').ToLowerInvariant())
}

function Get-LocalizedString {
    param([AllowNull()] [object]$Value)

    if ($null -eq $Value) { return '' }
    $localized = if ($Value -is [System.Collections.IDictionary] -and
        $Value.Contains('LocalizedString')) {
        [string]$Value.LocalizedString
    } elseif ($Value.PSObject.Properties.Name -contains 'LocalizedString') {
        [string]$Value.LocalizedString
    } else {
        ''
    }
    if (-not [string]::IsNullOrWhiteSpace($localized)) { return $localized }
    if ($Value -is [System.Collections.IDictionary] -and $Value.Contains('SourceString')) {
        return [string]$Value.SourceString
    }
    if ($Value.PSObject.Properties.Name -contains 'SourceString') {
        return [string]$Value.SourceString
    }
    return ''
}

function Remove-GameRichText {
    param([AllowNull()] [string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
    return $Value -replace '<[^>]+>', ''
}

function ConvertTo-CharacterAttribute {
    param([Parameter(Mandatory)] [string]$ElementType)

    $map = @{
        'CHARACTER_ELEMENT_TYPE_INCANTATION' = '咒'
        'CHARACTER_ELEMENT_TYPE_CHAOS' = '暗'
        'CHARACTER_ELEMENT_TYPE_LAKSHANA' = '相'
        'CHARACTER_ELEMENT_TYPE_COSMOS' = '光'
        'CHARACTER_ELEMENT_TYPE_PSYCHE' = '魂'
        'CHARACTER_ELEMENT_TYPE_NATURE' = '灵'
    }
    foreach ($entry in $map.GetEnumerator()) {
        if ($ElementType -like ('*' + $entry.Key)) { return $entry.Value }
    }
    throw "Unknown character element type: $ElementType"
}

function ConvertFrom-CharacterDataTable {
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary]$Rows,
        [Parameter(Mandatory)] [System.Collections.IDictionary]$Registered
    )

    $characters = [ordered]@{}
    foreach ($id in @($Registered.Keys | Sort-Object)) {
        $row = $Rows[[string]$id]
        if ($null -eq $row) {
            throw "Registered character is missing from DT_Character: $id"
        }
        $actor = [string]$row.ElementData.CharacterActorClass.AssetPathName
        $actorMatch = [regex]::Match($actor, '\.(?<name>[^/.]+)_C$')
        if (-not $actorMatch.Success) {
            throw "Character actor path is invalid for ${id}: $actor"
        }
        $codename = $actorMatch.Groups['name'].Value -replace '^(?i:player_[0-9]+_)', ''
        $nameZh = Get-LocalizedString -Value $row.ItemName
        if ([string]::IsNullOrWhiteSpace($codename) -or
            [string]::IsNullOrWhiteSpace($nameZh)) {
            throw "Character identity is incomplete in DT_Character: $id"
        }
        $characters[[string]$id] = [ordered]@{
            attribute = ConvertTo-CharacterAttribute -ElementType (
                [string]$row.ElementData.CharacterElementType)
            codename = $codename
            name_zh = $nameZh
        }
    }
    return $characters
}

function ConvertFrom-AbilityTipsDataTable {
    param([Parameter(Mandatory)] [System.Collections.IDictionary]$Rows)

    $abilities = [ordered]@{}
    foreach ($entry in $Rows.GetEnumerator() | Sort-Object Key) {
        $name = Remove-GameRichText -Value (Get-LocalizedString -Value $entry.Value.Name)
        $abilities[[string]$entry.Key] = [ordered]@{
            name_zh = $name
        }
    }
    return $abilities
}

function New-DataSections {
    $result = [ordered]@{}
    foreach ($section in $sectionNames) {
        $result[$section] = [ordered]@{}
    }
    return $result
}

function New-ReferenceSummary {
    return [ordered]@{
        unresolved_ability_tips = [ordered]@{}
    }
}

function Test-ExactStringSet {
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]]$Actual,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]]$Expected
    )

    if ($Actual.Count -ne $Expected.Count) { return $false }
    $actualSet = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($value in $Actual) {
        if (-not $actualSet.Add([string]$value)) { return $false }
    }
    foreach ($value in $Expected) {
        if (-not $actualSet.Contains([string]$value)) { return $false }
    }
    return $true
}

function Add-OwnerEvidence {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.HashSet[string]]$Set,
        [AllowNull()] [string]$Owner
    )

    if (-not [string]::IsNullOrWhiteSpace($Owner) -and $Owner -ne 'shared') {
        $null = $Set.Add($Owner)
    }
}

function Select-Owner {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.HashSet[string]]$Evidence
    )

    if ($Evidence.Count -eq 1) {
        return @($Evidence)[0]
    }
    return 'shared'
}

function Test-Prefix {
    param(
        [Parameter(Mandatory)] [string]$Value,
        [Parameter(Mandatory)] [object[]]$Prefixes
    )

    foreach ($prefix in $Prefixes) {
        if ($Value.StartsWith([string]$prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Get-IdentifierOwner {
    param(
        [Parameter(Mandatory)] [string]$TechnicalName,
        [Parameter(Mandatory)] [hashtable]$IdentifierOwners
    )

    $stem = $TechnicalName -replace '^(?i:GA_|GE_Player_|GE_|Buff_Player_|Buff_)', ''
    foreach ($alias in @($IdentifierOwners.Keys | Sort-Object Length -Descending)) {
        if ($stem.Equals($alias, [System.StringComparison]::OrdinalIgnoreCase) -or
            $stem.StartsWith(
                $alias + '_', [System.StringComparison]::OrdinalIgnoreCase)) {
            return [string]$IdentifierOwners[$alias]
        }
    }
    return $null
}

function Get-AbilityAssetAlias {
    param([Parameter(Mandatory)] [string]$AssetPath)

    $match = [regex]::Match(
        $AssetPath,
        '/Ability_([^/]+)/',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($match.Success) { return $match.Groups[1].Value }
    return $null
}

function Get-CharacterIdOwner {
    param(
        [Parameter(Mandatory)] [string]$TechnicalName,
        [Parameter(Mandatory)]
        [System.Collections.Generic.HashSet[string]]$RegisteredIds
    )

    foreach ($match in [regex]::Matches($TechnicalName, '(?<![0-9])(1[0-9]{3})(?![0-9])')) {
        $id = $match.Groups[1].Value
        if ($RegisteredIds.Contains($id)) {
            return 'registered:' + $id
        }
    }
    return $null
}

function Get-EvidenceDescription {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.HashSet[string]]$Evidence,
        [Parameter(Mandatory)] [string]$Fallback
    )

    if ($Evidence.Count -gt 1) {
        return 'conflicting owners: ' + ((@($Evidence) | Sort-Object) -join ', ')
    }
    return $Fallback
}

$resolvedRepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$configuration = Read-StrictJson -Path (
    Join-Path $resolvedRepositoryRoot 'script/character-data-ownership.json')
if ($configuration.format_version -ne 1) {
    throw 'Unsupported character ownership configuration format.'
}

$characterDocument = Read-StrictJson -Path (
    Join-Path $resolvedRepositoryRoot $sourceTables[0])
$abilityTipsDocument = Read-StrictJson -Path (
    Join-Path $resolvedRepositoryRoot $sourceTables[1])
$skillDamageDocument = Read-StrictJson -Path (
    Join-Path $resolvedRepositoryRoot $sourceTables[2])
$effectMappingDocument = Read-StrictJson -Path (
    Join-Path $resolvedRepositoryRoot $sourceTables[3])

if ($null -eq $characterDocument.Rows -or
    $null -eq $abilityTipsDocument.Rows -or
    $null -eq $skillDamageDocument.Rows -or
    $null -eq $effectMappingDocument.Rows) {
    throw 'Character extraction inputs have invalid root objects.'
}
$characters = ConvertFrom-CharacterDataTable `
    -Rows $characterDocument.Rows `
    -Registered $configuration.registered
$abilityTips = ConvertFrom-AbilityTipsDataTable -Rows $abilityTipsDocument.Rows
$skillDamage = $skillDamageDocument.Rows
$effectMapping = $effectMappingDocument.Rows
$sourceRevision = Get-DeterministicRevision `
    -RepositoryRoot $resolvedRepositoryRoot `
    -RelativePaths $sourceTables `
    -Version $generatorVersion

foreach ($entry in $abilityTips.GetEnumerator()) {
    if (-not ([string]$entry.Key).StartsWith(
            'GA_', [System.StringComparison]::OrdinalIgnoreCase) -or
        -not $entry.Value.Contains('name_zh') -or
        $entry.Value.name_zh -isnot [string]) {
        throw "Invalid ability tip entry: $($entry.Key)"
    }
}
foreach ($entry in $skillDamage.GetEnumerator()) {
    if ([string]$entry.Key -notmatch '^[A-Za-z][A-Za-z0-9_]*$' -or
        -not $entry.Value.Contains('GAName') -or
        $entry.Value.GAName -isnot [string] -or
        [string]$entry.Value.GAName -notmatch '^(None|[A-Za-z][A-Za-z0-9_]*)$') {
        throw "Invalid GE to GA reference: $($entry.Key)"
    }
}

$outputs = [ordered]@{}
$assetOwners = @{}
$identifierOwners = @{}
$registeredIds = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
$characterCodenames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)

foreach ($entry in $characters.GetEnumerator() | Sort-Object Key) {
    $id = [string]$entry.Key
    if (-not $configuration.registered.Contains($id)) {
        throw "Registered character lacks ownership configuration: $id"
    }
    if (-not $registeredIds.Add($id)) {
        throw "Duplicate registered character id: $id"
    }
    $rule = $configuration.registered[$id]
    $codename = [string]$entry.Value.codename
    if ([string]::IsNullOrWhiteSpace($codename)) {
        throw "Registered character lacks a codename: $id"
    }
    if (-not $characterCodenames.Add($codename)) {
        throw "Duplicate character codename: $codename"
    }
    $roleKey = 'registered:' + $id
    $fileName = $id + '-' + (ConvertTo-Slug -Value $codename) + '.json'
    $characterData = [ordered]@{}
    $characterData[$id] = $entry.Value
    $data = New-DataSections
    $data.characters = $characterData
    $outputs[$roleKey] = [ordered]@{
        file_name = $fileName
        document = [ordered]@{
            format_version = 1
            source = [ordered]@{
                generator = $generatorName
                assets_git_revision = $sourceRevision
                tables = $sourceTables
            }
            identity = [ordered]@{
                kind = 'registered'
                character_id = [uint32]$id
                codename = $codename
                name_zh = [string]$entry.Value.name_zh
                asset_aliases = @($rule.asset_aliases)
                identifier_aliases = @($rule.identifier_aliases)
            }
            data = $data
            references = New-ReferenceSummary
        }
    }
    foreach ($alias in $rule.asset_aliases) {
        $key = ([string]$alias).ToLowerInvariant()
        if ($assetOwners.ContainsKey($key)) { throw "Duplicate asset alias: $alias" }
        $assetOwners[$key] = $roleKey
    }
    foreach ($alias in $rule.identifier_aliases) {
        $key = [string]$alias
        if ($identifierOwners.ContainsKey($key)) { throw "Duplicate identifier alias: $alias" }
        $identifierOwners[$key] = $roleKey
    }
}
if ($registeredIds.Count -ne $configuration.registered.Count) {
    throw 'Ownership configuration contains an unknown registered character id.'
}

$requiredProvisional = [ordered]@{
    '018' = [ordered]@{
        codename = 'Alphard'
        asset_aliases = @('018_Alphard')
        identifier_aliases = @('Alphard')
    }
    '040' = [ordered]@{
        codename = 'Mismo'
        asset_aliases = @('040_Mismo')
        identifier_aliases = @('Mismo')
    }
    '042' = [ordered]@{
        codename = 'BlackBird'
        asset_aliases = @('042_BlackBird', '042_BlackBird_Evolve')
        identifier_aliases = @('BlackBird_Evolve', 'BlackBird')
    }
    '057' = [ordered]@{
        codename = 'Akane'
        asset_aliases = @('057_Akane')
        identifier_aliases = @('Akane')
    }
}
if ($configuration.provisional.Count -ne $requiredProvisional.Count) {
    throw 'Character extraction requires exactly four provisional characters.'
}
foreach ($rule in $configuration.provisional) {
    $assetNumber = [string]$rule.asset_number
    $codename = [string]$rule.codename
    if (-not $requiredProvisional.Contains($assetNumber) -or
        $requiredProvisional[$assetNumber].codename -cne $codename) {
        throw "Unexpected provisional character: $assetNumber/$codename"
    }
    if (-not (Test-ExactStringSet -Actual @($rule.asset_aliases) `
            -Expected @($requiredProvisional[$assetNumber].asset_aliases)) -or
        -not (Test-ExactStringSet -Actual @($rule.identifier_aliases) `
            -Expected @($requiredProvisional[$assetNumber].identifier_aliases))) {
        throw "Provisional character aliases differ from the fixed contract: $assetNumber"
    }
    if (-not $characterCodenames.Add($codename)) {
        throw "Duplicate character codename: $codename"
    }
    $roleKey = 'provisional:' + $assetNumber + ':' + $codename.ToLowerInvariant()
    $data = New-DataSections
    $outputs[$roleKey] = [ordered]@{
        file_name = 'provisional-' + $assetNumber + '-' + (
            ConvertTo-Slug -Value $codename) + '.json'
        document = [ordered]@{
            format_version = 1
            source = [ordered]@{
                generator = $generatorName
                assets_git_revision = $sourceRevision
                tables = $sourceTables
            }
            identity = [ordered]@{
                kind = 'provisional'
                character_id = $null
                codename = $codename
                name_zh = $null
                asset_aliases = @($rule.asset_aliases)
                identifier_aliases = @($rule.identifier_aliases)
            }
            data = $data
            references = New-ReferenceSummary
        }
    }
    foreach ($alias in $rule.asset_aliases) {
        $key = ([string]$alias).ToLowerInvariant()
        if ($assetOwners.ContainsKey($key)) { throw "Duplicate asset alias: $alias" }
        $assetOwners[$key] = $roleKey
    }
    foreach ($alias in $rule.identifier_aliases) {
        $key = [string]$alias
        if ($identifierOwners.ContainsKey($key)) { throw "Duplicate identifier alias: $alias" }
        $identifierOwners[$key] = $roleKey
    }
}

$sharedData = New-DataSections
$outputs.shared = [ordered]@{
    file_name = 'shared.json'
    document = [ordered]@{
        format_version = 1
        source = [ordered]@{
            generator = $generatorName
            assets_git_revision = $sourceRevision
            tables = $sourceTables
        }
        identity = [ordered]@{
            kind = 'shared'
            character_id = $null
            codename = $null
            name_zh = $null
            asset_aliases = @($configuration.shared_asset_aliases)
            identifier_aliases = @()
        }
        data = $sharedData
        references = New-ReferenceSummary
        ownership_notes = [ordered]@{}
    }
}
foreach ($alias in $configuration.shared_asset_aliases) {
    $key = ([string]$alias).ToLowerInvariant()
    if ($assetOwners.ContainsKey($key)) { throw "Duplicate shared asset alias: $alias" }
    $assetOwners[$key] = 'shared'
}

if ($outputs.Count -ne 29) {
    throw "Expected 29 output documents, prepared $($outputs.Count)."
}

$mappingOwners = @{}
$playerMappingKeys = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($entry in $effectMapping.GetEnumerator() | Sort-Object Key) {
    $path = [string]$entry.Value.GameplayEffectClass.AssetPathName
    if (-not $path.StartsWith(
            '/Game/Blueprints/Abilities/Player/',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $null = $playerMappingKeys.Add([string]$entry.Key)
    $alias = Get-AbilityAssetAlias -AssetPath $path
    $owner = 'shared'
    $reason = 'player ability asset has no registered or provisional owner'
    if (-not [string]::IsNullOrWhiteSpace($alias)) {
        $aliasKey = $alias.ToLowerInvariant()
        if ($assetOwners.ContainsKey($aliasKey)) {
            $owner = [string]$assetOwners[$aliasKey]
            $reason = if ($owner -eq 'shared') {
                'configured shared player ability asset'
            } else { '' }
        }
    }
    $mappingOwners[[string]$entry.Key] = $owner
    $outputs[$owner].document.data.gameplay_effect_mapping[[string]$entry.Key] = $entry.Value
    if ($owner -eq 'shared') {
        $outputs.shared.document.ownership_notes[
            'gameplay_effect_mapping/' + [string]$entry.Key] = $reason
    }
}

$abilityEvidence = @{}
foreach ($ability in $abilityTips.Keys) {
    $set = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    Add-OwnerEvidence -Set $set -Owner (
        Get-IdentifierOwner -TechnicalName ([string]$ability) `
            -IdentifierOwners $identifierOwners)
    $abilityEvidence[[string]$ability] = $set
}
foreach ($entry in $skillDamage.GetEnumerator()) {
    $ability = [string]$entry.Value.GAName
    if ([string]::IsNullOrWhiteSpace($ability) -or $ability -eq 'None' -or
        -not $abilityEvidence.ContainsKey($ability)) {
        continue
    }
    if ($mappingOwners.ContainsKey([string]$entry.Key)) {
        Add-OwnerEvidence -Set $abilityEvidence[$ability] `
            -Owner ([string]$mappingOwners[[string]$entry.Key])
    }
}

$abilityOwners = @{}
$eligibleAbilities = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($entry in $abilityTips.GetEnumerator() | Sort-Object Key) {
    $ability = [string]$entry.Key
    $evidence = $abilityEvidence[$ability]
    $referencedByPlayerEffect = $false
    foreach ($damageEntry in $skillDamage.GetEnumerator()) {
        if ([string]$damageEntry.Value.GAName -eq $ability -and
            $playerMappingKeys.Contains([string]$damageEntry.Key)) {
            $referencedByPlayerEffect = $true
            break
        }
    }
    $sharedPrefix = Test-Prefix -Value $ability `
        -Prefixes @($configuration.shared_ability_prefixes)
    if ($evidence.Count -eq 0 -and -not $referencedByPlayerEffect -and -not $sharedPrefix) {
        continue
    }
    $null = $eligibleAbilities.Add($ability)
    $owner = Select-Owner -Evidence $evidence
    $abilityOwners[$ability] = $owner
    $outputs[$owner].document.data.ability_tips[$ability] = $entry.Value
    if ($owner -eq 'shared') {
        $fallback = if ($sharedPrefix) {
            'configured shared player ability'
        } else {
            'player ability has no unique owner'
        }
        $outputs.shared.document.ownership_notes['ability_tips/' + $ability] =
            Get-EvidenceDescription -Evidence $evidence -Fallback $fallback
    }
}

$sharedEffects = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($effect in $configuration.shared_effects) {
    $null = $sharedEffects.Add([string]$effect)
}
foreach ($entry in $skillDamage.GetEnumerator() | Sort-Object Key) {
    $effect = [string]$entry.Key
    $evidence = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    if ($mappingOwners.ContainsKey($effect)) {
        Add-OwnerEvidence -Set $evidence -Owner ([string]$mappingOwners[$effect])
    }
    $ability = [string]$entry.Value.GAName
    if ($abilityOwners.ContainsKey($ability)) {
        Add-OwnerEvidence -Set $evidence -Owner ([string]$abilityOwners[$ability])
    }
    Add-OwnerEvidence -Set $evidence -Owner (
        Get-IdentifierOwner -TechnicalName $effect -IdentifierOwners $identifierOwners)
    if ($evidence.Count -eq 0) {
        Add-OwnerEvidence -Set $evidence -Owner (
            Get-CharacterIdOwner -TechnicalName $effect -RegisteredIds $registeredIds)
    }
    $sharedAbilityReference = Test-Prefix -Value $ability `
        -Prefixes @($configuration.shared_ability_prefixes)
    $eligible = $evidence.Count -gt 0 -or $mappingOwners.ContainsKey($effect) -or
        $eligibleAbilities.Contains($ability) -or
        $effect.StartsWith('GE_Player_', [System.StringComparison]::OrdinalIgnoreCase) -or
        $sharedAbilityReference -or $sharedEffects.Contains($effect)
    if (-not $eligible) { continue }
    $owner = Select-Owner -Evidence $evidence
    $outputs[$owner].document.data.skill_damage[$effect] = $entry.Value
    if ($owner -eq 'shared') {
        $outputs.shared.document.ownership_notes['skill_damage/' + $effect] =
            Get-EvidenceDescription -Evidence $evidence `
                -Fallback 'player damage effect has no unique owner'
    }
}

foreach ($output in $outputs.Values) {
    foreach ($section in $sectionNames) {
        $sorted = [ordered]@{}
        foreach ($key in @($output.document.data[$section].Keys | Sort-Object)) {
            $sorted[$key] = $output.document.data[$section][$key]
        }
        $output.document.data[$section] = $sorted
    }
}
$sortedNotes = [ordered]@{}
foreach ($key in @($outputs.shared.document.ownership_notes.Keys | Sort-Object)) {
    $sortedNotes[$key] = $outputs.shared.document.ownership_notes[$key]
}
$outputs.shared.document.ownership_notes = $sortedNotes

foreach ($output in $outputs.Values) {
    $unresolved = @{}
    foreach ($entry in $output.document.data.skill_damage.GetEnumerator()) {
        $ability = [string]$entry.Value.GAName
        if ($ability -eq 'None' -or $abilityTips.Contains($ability)) { continue }
        if (-not $unresolved.ContainsKey($ability)) {
            $unresolved[$ability] = [System.Collections.Generic.List[string]]::new()
        }
        $unresolved[$ability].Add([string]$entry.Key)
    }
    $sortedUnresolved = [ordered]@{}
    foreach ($ability in @($unresolved.Keys | Sort-Object)) {
        $sortedUnresolved[$ability] = @($unresolved[$ability] | Sort-Object -Unique)
    }
    $output.document.references.unresolved_ability_tips = $sortedUnresolved
}

$emittedRows = @{}
foreach ($section in $sectionNames) {
    $emittedRows[$section] = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
}
foreach ($output in $outputs.Values) {
    foreach ($section in $sectionNames) {
        foreach ($key in $output.document.data[$section].Keys) {
            if (-not $emittedRows[$section].Add([string]$key)) {
                throw "Source row was emitted more than once: $section/$key"
            }
        }
    }
}
foreach ($output in $outputs.Values) {
    $roleData = $output.document.data
    foreach ($entry in $roleData.skill_damage.GetEnumerator()) {
        $ability = [string]$entry.Value.GAName
        if ($ability -eq 'None') { continue }
        # The upstream damage table legitimately contains technical GA references
        # with no ability-tip row. Model those identifiers without fabricating data.
        if (-not $abilityTips.Contains($ability)) {
            if (-not $output.document.references.unresolved_ability_tips.Contains($ability) -or
                $output.document.references.unresolved_ability_tips[$ability] `
                    -notcontains [string]$entry.Key) {
                throw "Damage effect has an unmodeled ability-tip reference: $($entry.Key) -> $ability"
            }
            continue
        }
        if (-not $emittedRows.ability_tips.Contains($ability)) {
            throw "Emitted damage effect references an omitted ability: $($entry.Key) -> $ability"
        }
    }
}

$stagingDirectory = Join-Path ([System.IO.Path]::GetTempPath()) (
    'nte-character-data-' + [guid]::NewGuid().ToString('N'))
$targetDirectory = [System.IO.Path]::GetFullPath(
    (Join-Path $resolvedRepositoryRoot 'assets/dict/character'))
$expectedTargetPrefix = [System.IO.Path]::GetFullPath(
    (Join-Path $resolvedRepositoryRoot 'assets/dict')) + [System.IO.Path]::DirectorySeparatorChar
if (-not $targetDirectory.StartsWith(
        $expectedTargetPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Character output directory is outside dict: $targetDirectory"
}

try {
    $null = New-Item -ItemType Directory -Path $stagingDirectory
    foreach ($output in $outputs.Values) {
        $path = Join-Path $stagingDirectory $output.file_name
        [System.IO.File]::WriteAllText(
            $path,
            (ConvertTo-DeterministicJson -Value $output.document),
            [System.Text.UTF8Encoding]::new($false, $true))
    }

    if ($Write) {
        $null = New-Item -ItemType Directory -Path $targetDirectory -Force
        $expectedNames = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        foreach ($fileName in $outputs.Values.file_name) {
            $null = $expectedNames.Add([string]$fileName)
        }
        foreach ($existing in @(Get-ChildItem -LiteralPath $targetDirectory -Filter '*.json' -File)) {
            if ($expectedNames.Contains($existing.Name)) { continue }
            $generated = $false
            try {
                $existingDocument = Read-StrictJson -Path $existing.FullName
                $generated = $existingDocument.source.generator -eq $generatorName
            } catch {
                $generated = $false
            }
            if ($generated) {
                Remove-Item -LiteralPath $existing.FullName -Force
            }
        }
        foreach ($output in $outputs.Values) {
            Copy-Item -LiteralPath (Join-Path $stagingDirectory $output.file_name) `
                -Destination (Join-Path $targetDirectory $output.file_name) -Force
        }
        Write-Host "Wrote $($outputs.Count) character data files to $targetDirectory."
    } else {
        if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
            throw "Character data output is unavailable: $targetDirectory"
        }
        $actualFiles = @(Get-ChildItem -LiteralPath $targetDirectory -Filter '*.json' -File |
            Sort-Object Name)
        $expectedFiles = @(Get-ChildItem -LiteralPath $stagingDirectory -Filter '*.json' -File |
            Sort-Object Name)
        if ($actualFiles.Count -ne $expectedFiles.Count -or
            [string]::Join("`n", @($actualFiles.Name)) -ne
            [string]::Join("`n", @($expectedFiles.Name))) {
            throw 'Generated character data file set is stale.'
        }
        foreach ($expected in $expectedFiles) {
            $actualPath = Join-Path $targetDirectory $expected.Name
            $actualBytes = [System.IO.File]::ReadAllBytes($actualPath)
            $expectedBytes = [System.IO.File]::ReadAllBytes($expected.FullName)
            if ($actualBytes.Length -ne $expectedBytes.Length -or
                [Convert]::ToBase64String($actualBytes) -ne
                [Convert]::ToBase64String($expectedBytes)) {
                throw "Generated character data differs from source tables: $($expected.Name)"
            }
        }
        Write-Host "Verified $($outputs.Count) character data files in $targetDirectory."
    }
} finally {
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
}
