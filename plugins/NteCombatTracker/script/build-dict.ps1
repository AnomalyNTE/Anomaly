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

function Read-StrictJson {
    param([Parameter(Mandatory)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required JSON input is unavailable: $Path"
    }
    $utf8 = [System.Text.UTF8Encoding]::new($false, $true)
    $content = $utf8.GetString([System.IO.File]::ReadAllBytes($Path))
    return $content | ConvertFrom-Json -AsHashtable -Depth 256
}

function ConvertTo-DeterministicJson {
    param([Parameter(Mandatory)] [object]$Value)

    return (($Value | ConvertTo-Json -Depth 100).Replace("`r`n", "`n") + "`n")
}

function Test-SameBytes {
    param(
        [Parameter(Mandatory)] [string]$Left,
        [Parameter(Mandatory)] [string]$Right
    )

    $leftBytes = [System.IO.File]::ReadAllBytes($Left)
    $rightBytes = [System.IO.File]::ReadAllBytes($Right)
    return $leftBytes.Length -eq $rightBytes.Length -and
        [Convert]::ToBase64String($leftBytes) -eq
        [Convert]::ToBase64String($rightBytes)
}

$resolvedRepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$extractScript = Join-Path $PSScriptRoot 'extract-character-data.ps1'
$extractParameters = @{
    RepositoryRoot = $resolvedRepositoryRoot
}
if ($Write) {
    $extractParameters.Write = $true
} else {
    $extractParameters.Check = $true
}
& $extractScript @extractParameters

$monsterSource = Join-Path $resolvedRepositoryRoot 'data/enemies.json'
$monsterDocument = Read-StrictJson -Path $monsterSource
if ($null -eq $monsterDocument.enemies -or
    $monsterDocument.enemies -isnot [System.Array]) {
    throw 'Monster source has an invalid enemies array.'
}
$monsterIds = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($enemy in $monsterDocument.enemies) {
    if ([string]::IsNullOrWhiteSpace([string]$enemy.config_id) -or
        [string]::IsNullOrWhiteSpace([string]$enemy.monster_id) -or
        [string]::IsNullOrWhiteSpace([string]$enemy.name_zh) -or
        -not $monsterIds.Add([string]$enemy.config_id)) {
        throw "Invalid or duplicate monster entry: $($enemy.config_id)"
    }
}

$targetDirectory = [System.IO.Path]::GetFullPath(
    (Join-Path $resolvedRepositoryRoot 'assets/dict'))
$targetPath = Join-Path $targetDirectory 'monster.json'
$stagingPath = Join-Path ([System.IO.Path]::GetTempPath()) (
    'nte-monster-dict-' + [guid]::NewGuid().ToString('N') + '.json')

try {
    [System.IO.File]::WriteAllText(
        $stagingPath,
        (ConvertTo-DeterministicJson -Value $monsterDocument),
        [System.Text.UTF8Encoding]::new($false, $true))

    if ($Write) {
        $null = New-Item -ItemType Directory -Path $targetDirectory -Force
        Copy-Item -LiteralPath $stagingPath -Destination $targetPath -Force
        Write-Host "Wrote monster dictionary to $targetPath."
    } else {
        if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
            throw "Monster dictionary is unavailable: $targetPath"
        }
        if (-not (Test-SameBytes -Left $targetPath -Right $stagingPath)) {
            throw 'Generated monster dictionary is stale.'
        }
        Write-Host "Verified monster dictionary in $targetPath."
    }
} finally {
    if (Test-Path -LiteralPath $stagingPath -PathType Leaf) {
        Remove-Item -LiteralPath $stagingPath -Force
    }
}
