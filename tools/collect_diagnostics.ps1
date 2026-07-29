param(
    [string]$RuntimeDirectory = '.\Anomaly',
    [string]$Output = ".\anomaly-diagnostics-$((Get-Date).ToString('yyyyMMdd-HHmmss')).zip"
)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7+ (pwsh.exe) is required; invoke this script with pwsh -NoProfile -File.'
}

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$runtime = [IO.Path]::GetFullPath($RuntimeDirectory)
if ($runtime.Length -gt [IO.Path]::GetPathRoot($runtime).Length) {
    $runtime = $runtime.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}
if (-not (Test-Path -LiteralPath $runtime -PathType Container)) {
    throw "runtime directory not found: $runtime"
}
$outputPath = [IO.Path]::GetFullPath($Output)
$stage = Join-Path ([IO.Path]::GetTempPath()) (
    'anomaly-diagnostics-' + [guid]::NewGuid().ToString('N'))

$sensitiveKey = '(?:token|secret|password|api[-_]?key|authorization|access[-_]?token|refresh[-_]?token|client[-_]?secret)'
$sensitiveName = [regex]::new("^(?i:$sensitiveKey)$", [Text.RegularExpressions.RegexOptions]::CultureInvariant)
$quotedSecret = [regex]::new(
    ('(?im)(?<prefix>"' + $sensitiveKey + '"\s*:\s*).*$'),
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
$keyValueSecret = [regex]::new(
    ('(?im)(?<prefix>(?<![A-Za-z0-9_])' + $sensitiveKey + '\s*[:=]\s*).*$'),
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
$windowsHome = [regex]::new(
    '(?i)(?:(?:[A-Z]:)|(?:[\\/]{2,}[^\\/\s"''<>|]+))[\\/]+Users[\\/]+[^\\/"''<>|\r\n]+',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)
$posixHome = [regex]::new(
    '(?i)(?<![A-Za-z0-9_])/(?:home|Users)/[^/\\"''<>|\r\n]+',
    [Text.RegularExpressions.RegexOptions]::CultureInvariant)

function Protect-JsonNode {
    param([AllowNull()][object]$Node)

    if ($null -eq $Node -or $Node -is [string] -or $Node.GetType().IsValueType) {
        return $Node
    }
    if ($Node -is [Collections.IDictionary]) {
        foreach ($key in @($Node.Keys)) {
            if ($sensitiveName.IsMatch([string]$key)) {
                $Node[$key] = '<redacted>'
            } else {
                $Node[$key] = Protect-JsonNode $Node[$key]
            }
        }
        return $Node
    }
    if ($Node -is [Collections.IList]) {
        for ($index = 0; $index -lt $Node.Count; ++$index) {
            $Node[$index] = Protect-JsonNode $Node[$index]
        }
        return ,$Node
    }
    foreach ($property in @($Node.PSObject.Properties)) {
        if (-not $property.IsSettable) { continue }
        if ($sensitiveName.IsMatch($property.Name)) {
            $property.Value = '<redacted>'
        } else {
            $property.Value = Protect-JsonNode $property.Value
        }
    }
    return $Node
}

function Protect-HomePaths {
    param([AllowEmptyString()][string]$Text)

    $result = $windowsHome.Replace($Text, '<user-home>')
    $result = $posixHome.Replace($result, '<user-home>')
    foreach ($homePath in @($env:USERPROFILE, $HOME,
            ($env:HOMEDRIVE + $env:HOMEPATH))) {
        if ([string]::IsNullOrWhiteSpace($homePath)) { continue }
        $fullHome = [IO.Path]::GetFullPath($homePath)
        foreach ($variant in @($fullHome, ($fullHome -replace '\\', '/'),
                $fullHome.Replace('\', '\\'))) {
            $result = [regex]::Replace(
                $result, [regex]::Escape($variant), '<user-home>',
                [Text.RegularExpressions.RegexOptions]::IgnoreCase)
        }
    }
    return $result
}

function Protect-FallbackText {
    param([AllowEmptyString()][string]$Text)

    $result = $quotedSecret.Replace($Text, {
        param($match)
        return $match.Groups['prefix'].Value + '"<redacted>"'
    })
    $result = $keyValueSecret.Replace($result, {
        param($match)
        return $match.Groups['prefix'].Value + '<redacted>'
    })
    return Protect-HomePaths $result
}

function Protect-JsonText {
    param([AllowEmptyString()][string]$Text, [switch]$Compress)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return Protect-FallbackText $Text
    }
    try {
        $document = $Text | ConvertFrom-Json -AsHashtable
        $document = Protect-JsonNode $document
        $parameters = @{ Depth = 100 }
        if ($Compress) { $parameters.Compress = $true }
        return Protect-HomePaths (ConvertTo-Json -InputObject $document @parameters)
    } catch {
        return Protect-FallbackText $Text
    }
}

function Protect-JsonLines {
    param([AllowEmptyString()][string]$Text)

    $lines = [regex]::Split($Text, '\r?\n')
    $protected = [Collections.Generic.List[string]]::new()
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            $protected.Add($line)
        } else {
            $protected.Add((Protect-JsonText $line -Compress))
        }
    }
    return [string]::Join("`n", $protected)
}

function Remove-DuplicateLines {
    param([AllowEmptyString()][string]$Text)

    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $lines = [regex]::Split($Text, '\r?\n')
    $deduplicated = [Collections.Generic.List[string]]::new()
    foreach ($line in $lines) {
        if ($seen.Add($line)) { $deduplicated.Add($line) }
    }
    return [string]::Join("`n", $deduplicated)
}

$candidates = [Collections.Generic.Dictionary[string, object]]::new(
    [StringComparer]::OrdinalIgnoreCase)

function Add-AllowlistedFile {
    param([string]$Path, [long]$MaximumBytes)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return }
    $fullPath = [IO.Path]::GetFullPath($item.FullName)
    $relative = [IO.Path]::GetRelativePath($runtime, $fullPath)
    if ([IO.Path]::IsPathRooted($relative) -or
        $relative -eq '..' -or $relative.StartsWith("..$([IO.Path]::DirectorySeparatorChar)")) {
        return
    }

    $directory = $item.Directory
    while ($null -ne $directory -and
           -not [string]::Equals($directory.FullName, $runtime, [StringComparison]::OrdinalIgnoreCase)) {
        if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return }
        $directory = $directory.Parent
    }
    if ($null -eq $directory) { return }
    if ($item.Length -gt $MaximumBytes) { return }

    $portableRelative = $relative.Replace([IO.Path]::DirectorySeparatorChar, '/')
    $candidates[$fullPath] = [pscustomobject]@{
        Source = $item
        Relative = $portableRelative
    }
}

function Add-AllowlistedDirectoryFiles {
    param([string]$Directory, [string[]]$Extensions, [long]$MaximumBytes)

    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return }
    $directoryItem = Get-Item -LiteralPath $Directory -Force
    if (($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return }
    Get-ChildItem -LiteralPath $Directory -File -Force | Sort-Object Name | ForEach-Object {
        if ($_.Extension -in $Extensions) {
            Add-AllowlistedFile $_.FullName $MaximumBytes
        }
    }
}

# Exact root/runtime files. No recursive wildcard is used for any source path.
foreach ($name in @(
        'anomaly.ini',
        'repository.json',
        'plugin-repositories.json',
        'anomaly-platform.log',
        'anomaly-runtime.log',
        'anomaly-pipe-error.log')) {
    Add-AllowlistedFile (Join-Path $runtime $name) (16MB)
}
Add-AllowlistedFile (Join-Path $runtime 'state\diagnostics-summary.json') (1MB)
Add-AllowlistedDirectoryFiles (Join-Path $runtime 'logs') @('.log', '.jsonl') (16MB)
Add-AllowlistedDirectoryFiles (Join-Path $runtime 'profiles\nte') @('.json') (1MB)
Add-AllowlistedDirectoryFiles (Join-Path $runtime 'profiles-local\nte') @('.json') (1MB)
Add-AllowlistedDirectoryFiles (Join-Path $runtime 'state\profiles\managed\nte') @('.json') (1MB)

$pluginRoot = Join-Path $runtime 'plugins'
if (Test-Path -LiteralPath $pluginRoot -PathType Container) {
    $pluginRootItem = Get-Item -LiteralPath $pluginRoot -Force
    if (($pluginRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
        Get-ChildItem -LiteralPath $pluginRoot -Directory -Force | Sort-Object Name | ForEach-Object {
            if ($_.Name -ne '.cache' -and
                ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                Add-AllowlistedFile (Join-Path $_.FullName 'manifest.json') (1MB)
            }
        }
    }
}

if ($candidates.Count -gt 512) { throw 'diagnostic allowlist exceeded 512 files' }
[long]$totalBytes = 0
foreach ($candidate in $candidates.Values) { $totalBytes += $candidate.Source.Length }
if ($totalBytes -gt 64MB) {
    throw 'diagnostic allowlist exceeded 64 MiB'
}

New-Item -ItemType Directory -Force $stage | Out-Null
try {
    foreach ($candidate in @($candidates.Values | Sort-Object Relative)) {
        $destination = Join-Path $stage ($candidate.Relative -replace '/', '\')
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($destination)) | Out-Null
        Copy-Item -LiteralPath $candidate.Source.FullName -Destination $destination -Force
    }

    Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
        $extension = $_.Extension.ToLowerInvariant()
        if ($extension -notin @('.log', '.jsonl', '.ini', '.json')) { return }
        $text = [string](Get-Content -LiteralPath $_.FullName -Raw)
        switch ($extension) {
            '.json'  { $text = Protect-JsonText $text }
            '.jsonl' { $text = Protect-JsonLines $text }
            default  { $text = Protect-FallbackText $text }
        }
        if ($extension -in @('.log', '.jsonl')) {
            $text = Remove-DuplicateLines $text
        }
        Set-Content -LiteralPath $_.FullName -Value $text -Encoding utf8NoBOM -NoNewline
    }

    $manifest = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
        [pscustomobject][ordered]@{
            Path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace(
                [IO.Path]::DirectorySeparatorChar, '/')
            Size = $_.Length
            Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    } | Sort-Object Path)
    ConvertTo-Json -InputObject $manifest -Depth 4 | Set-Content -LiteralPath (
        Join-Path $stage 'manifest.json') -Encoding utf8NoBOM

    $outputDirectory = [IO.Path]::GetDirectoryName($outputPath)
    if (-not [string]::IsNullOrEmpty($outputDirectory)) {
        New-Item -ItemType Directory -Force $outputDirectory | Out-Null
    }
    if (Test-Path -LiteralPath $outputPath) { Remove-Item -LiteralPath $outputPath -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $outputPath `
        -CompressionLevel Optimal
    Write-Output $outputPath
} finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
