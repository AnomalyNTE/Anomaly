[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-Json {
    param([Parameter(Mandatory)] [string]$Path)

    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-TreeHashes {
    param([Parameter(Mandatory)] [string]$Root)

    $hashes = [ordered]@{}
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName) {
        $relative = $file.FullName.Substring($Root.Length).TrimStart('\', '/')
        $hashes[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $hashes
}

$sourceRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'nte-build-dict-test-' + [guid]::NewGuid().ToString('N'))

try {
    $null = New-Item -ItemType Directory -Path $temporaryRoot
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'data/datatable') `
        -Destination (Join-Path $temporaryRoot 'data/datatable') -Recurse
    $null = New-Item -ItemType Directory -Path (
        Join-Path $temporaryRoot 'data') -Force
    Copy-Item -LiteralPath (
        Join-Path $sourceRoot 'data/enemies.json') `
        -Destination (Join-Path $temporaryRoot 'data/enemies.json')
    $null = New-Item -ItemType Directory -Path (
        Join-Path $temporaryRoot 'script') -Force
    foreach ($scriptName in @(
            'build-dict.ps1',
            'extract-character-data.ps1',
            'character-data-ownership.json')) {
        Copy-Item -LiteralPath (Join-Path $sourceRoot "script/$scriptName") `
            -Destination (Join-Path $temporaryRoot "script/$scriptName")
    }

    & (Join-Path $temporaryRoot 'script/build-dict.ps1') `
        -Write `
        -RepositoryRoot $temporaryRoot
    $firstHashes = Get-TreeHashes -Root (Join-Path $temporaryRoot 'assets/dict')

    & (Join-Path $temporaryRoot 'script/build-dict.ps1') `
        -Write `
        -RepositoryRoot $temporaryRoot
    $secondHashes = Get-TreeHashes -Root (Join-Path $temporaryRoot 'assets/dict')
    if (($firstHashes | ConvertTo-Json -Compress) -ne
        ($secondHashes | ConvertTo-Json -Compress)) {
        throw 'Repeated dict generation produced different bytes.'
    }

    & (Join-Path $temporaryRoot 'script/build-dict.ps1') `
        -Check `
        -RepositoryRoot $temporaryRoot

    $characterDirectory = Join-Path $temporaryRoot 'assets/dict/character'
    $characterFiles = @(Get-ChildItem -LiteralPath $characterDirectory -Filter '*.json' -File)
    if ($characterFiles.Count -ne 29) {
        throw "Expected 29 character files, got $($characterFiles.Count)."
    }
    foreach ($file in $characterFiles) {
        $document = Read-Json -Path $file.FullName
        $dataSections = @($document.data.PSObject.Properties.Name)
        if ($document.format_version -ne 1 -or
            $null -eq $document.source.assets_git_revision -or
            $dataSections -contains 'gameplay_effect_semantics') {
            throw "Invalid generated character document: $($file.Name)"
        }
        foreach ($section in @(
                'characters',
                'ability_tips',
                'skill_damage',
                'gameplay_effect_mapping')) {
            if ($dataSections -notcontains $section) {
                throw "Missing $section in $($file.Name)"
            }
        }
    }

    $sagiri = Read-Json -Path (Join-Path $characterDirectory '1003-sagiri.json')
    if ($sagiri.data.characters.'1003'.codename -cne 'Sagiri' -or
        $sagiri.data.characters.'1003'.name_zh -cne '早雾' -or
        $sagiri.data.characters.'1003'.attribute -cne '咒') {
        throw 'Generated Sagiri identity differs from the DataTable source.'
    }
    $hathor = Read-Json -Path (Join-Path $characterDirectory '1025-hathor.json')
    if ($hathor.data.ability_tips.'GA_Hathor_Skill2'.name_zh -cne
            '变轨技能：轮转打击') {
        throw 'Ability tip rich text was not removed.'
    }

    $monster = Read-Json -Path (Join-Path $temporaryRoot 'assets/dict/monster.json')
    if (@($monster.enemies).Count -ne 623 -or
        $monster.enemies[0].config_id -cne 'Boss_016_BP' -or
        $monster.enemies[0].name_zh -cne '随心泥') {
        throw 'Generated monster dictionary differs from data/enemies.json.'
    }

    Write-Host 'All build-dict tests passed.'
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
