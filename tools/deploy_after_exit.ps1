param(
    [Parameter(Mandatory = $true)]
    [int]$GameProcessId,

    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$TargetDirectory,

    [Parameter(Mandatory = $true)]
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'

function Copy-WithRetry {
    param([string]$Source, [string]$Destination)

    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        try {
            Copy-Item -LiteralPath $Source -Destination $Destination -Force
            return
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    throw "file remained locked: $Destination"
}

function Copy-DirectoryWithRetry {
    param([string]$Source, [string]$Destination)

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Recurse -Force | ForEach-Object {
        $relativePath = [IO.Path]::GetRelativePath($Source, $_.FullName)
        $targetPath = Join-Path $Destination $relativePath
        if ($_.PSIsContainer) {
            New-Item -ItemType Directory -Path $targetPath -Force | Out-Null
            return
        }

        $targetParent = Split-Path -Parent $targetPath
        New-Item -ItemType Directory -Path $targetParent -Force | Out-Null
        Copy-WithRetry $_.FullName $targetPath
    }
}

try {
    $sourceRuntimeDirectory = Join-Path $SourceDirectory 'Anomaly'
    foreach ($requiredSourceFile in @(
            (Join-Path $SourceDirectory 'dwmapi.dll'),
            (Join-Path $sourceRuntimeDirectory 'Anomaly.Core.dll'),
            (Join-Path $sourceRuntimeDirectory 'anomaly.ini'),
            (Join-Path $sourceRuntimeDirectory 'repository.json'),
            (Join-Path $sourceRuntimeDirectory 'plugin-repositories.json'))) {
        if (-not (Test-Path -LiteralPath $requiredSourceFile -PathType Leaf)) {
            throw "deployment source not found: $requiredSourceFile"
        }
    }
    $sourceProfileDirectory = Join-Path $sourceRuntimeDirectory 'profiles\nte'
    if (-not (Test-Path -LiteralPath $sourceProfileDirectory -PathType Container)) {
        throw "deployment source not found: $sourceProfileDirectory"
    }
    $sourcePluginRoot = Join-Path $sourceRuntimeDirectory 'plugins'
    if (-not (Test-Path -LiteralPath $sourcePluginRoot -PathType Container)) {
        throw "deployment source not found: $sourcePluginRoot"
    }
    $sourcePluginDirectories = @(Get-ChildItem -LiteralPath $sourcePluginRoot -Directory |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName 'plugin.dll') -PathType Leaf) -or
            (Test-Path -LiteralPath (Join-Path $_.FullName 'manifest.json') -PathType Leaf)
        })
    if ($sourcePluginDirectories.Count -eq 0) {
        throw "deployment source not found: $sourcePluginRoot"
    }
    foreach ($sourcePluginDirectory in $sourcePluginDirectories) {
        foreach ($file in @('plugin.dll', 'manifest.json')) {
            $requiredSource = Join-Path $sourcePluginDirectory.FullName $file
            if (-not (Test-Path -LiteralPath $requiredSource -PathType Leaf)) {
                throw "deployment source not found: $requiredSource"
            }
        }
    }

    while (Get-Process -Id $GameProcessId -ErrorAction SilentlyContinue) {
        Start-Sleep -Milliseconds 500
    }
    $runtimeDirectory = Join-Path $TargetDirectory 'Anomaly'
    $pluginRoot = Join-Path $runtimeDirectory 'plugins'
    $nteProfileDirectory = Join-Path $runtimeDirectory 'profiles\nte'
    New-Item -ItemType Directory -Path $pluginRoot -Force | Out-Null
    if (Test-Path -LiteralPath $nteProfileDirectory) {
        Remove-Item -LiteralPath $nteProfileDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Path $nteProfileDirectory -Force | Out-Null

    Copy-WithRetry (Join-Path $SourceDirectory 'Anomaly\Anomaly.Core.dll') `
        (Join-Path $runtimeDirectory 'Anomaly.Core.dll')
    Copy-WithRetry (Join-Path $SourceDirectory 'Anomaly\anomaly.ini') `
        (Join-Path $runtimeDirectory 'anomaly.ini')
    Copy-WithRetry (Join-Path $SourceDirectory 'Anomaly\repository.json') `
        (Join-Path $runtimeDirectory 'repository.json')
    Copy-WithRetry (Join-Path $SourceDirectory 'Anomaly\plugin-repositories.json') `
        (Join-Path $runtimeDirectory 'plugin-repositories.json')
    foreach ($sourcePluginDirectory in $sourcePluginDirectories) {
        $targetPluginDirectory = Join-Path $pluginRoot $sourcePluginDirectory.Name
        Copy-DirectoryWithRetry $sourcePluginDirectory.FullName $targetPluginDirectory
    }

    Get-ChildItem -LiteralPath $sourceProfileDirectory -File |
        ForEach-Object {
            Copy-WithRetry $_.FullName (Join-Path $nteProfileDirectory $_.Name)
        }

    $sourceDll = Join-Path $SourceDirectory 'dwmapi.dll'
    $targetDll = Join-Path $TargetDirectory 'dwmapi.dll'
    Copy-WithRetry $sourceDll $targetDll

    "$(Get-Date -Format o) deployed Anomaly after PID $GameProcessId exited" |
        Out-File -LiteralPath $LogPath -Encoding utf8 -Append
} catch {
    "$(Get-Date -Format o) deployment failed: $($_.Exception.Message)" |
        Out-File -LiteralPath $LogPath -Encoding utf8 -Append
    exit 1
}
