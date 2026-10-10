<#
.SYNOPSIS
Verifies a Jelly plugin package before it is shipped.

.DESCRIPTION
Checks what the build system cannot: the actual imports of the packaged binary and
the package manifest that sits beside it.

  * the binary must not depend on a debug or private CRT (a package that does is
    refused by the host's native-dependency preflight, which reports the plugin as
    faulted with "plugin activation failed");
  * the dependencies must be the release CRT and Windows system DLLs only;
  * from Anomaly 2.3.0 on the host maps plugin images with its own mapper, whose
    preflight also refuses an image that
      - imports _CxxThrowException without exporting AnomalyPluginCxxThrowV1
        (code missing-cxx-throw-bridge; the SDK ships that bridge as
        sdk/cpp/plugin_cxx_throw_bridge.cpp and anomaly_add_plugin compiles it in),
      - links the CRT statically (RtlPcToFileHeader import, code static-crt),
      - carries a TLS directory (code static-tls), or
      - has a writable and executable section (code writable-executable-section);
    all of those are checked here so the same class of failure is caught before the
    package is handed to a user;
  * package.sha256, when present, must match the files it lists.

Run it on the package directory, and again on a directory the release ZIP was
unpacked into, so the artefact that is handed to a user is the one that was
checked.

.EXAMPLE
pwsh -NoProfile -File plugins/Jelly/tools/verify_package.ps1 -Package output/jelly/packed/anomaly.plugin.jelly
#>
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Dumpbin
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Package -PathType Container)) {
    throw "package directory not found: $Package"
}

if (-not $Dumpbin) {
    $pattern = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe'
    $Dumpbin = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin -PathType Leaf)) {
    throw 'dumpbin.exe was not found; pass -Dumpbin <path>'
}

$binary = Join-Path $Package 'plugin.dll'
if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "plugin.dll not found in $Package"
}

function Get-PeFacts([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40) { throw "image is too small: $Path" }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or $peOffset + 24 -ge $bytes.Length) { throw 'PE header is out of range' }
    if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45) { throw 'image is not a PE file' }
    $coff = $peOffset + 4
    $sectionCount = [BitConverter]::ToUInt16($bytes, $coff + 2)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $coff + 16)
    $optional = $coff + 20
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    $directories = switch ($magic) {
        0x20B { $optional + 112 }   # PE32+
        0x10B { $optional + 96 }    # PE32
        default { throw 'unsupported optional header magic' }
    }
    $tlsRva = [BitConverter]::ToUInt32($bytes, $directories + 9 * 8)
    $entries = @()
    $sections = $optional + $optionalSize
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $offset = $sections + $index * 40
        if ($offset + 40 -gt $bytes.Length) { throw 'section table is out of range' }
        $entries += [pscustomobject]@{
            Name = ([System.Text.Encoding]::ASCII.GetString($bytes, $offset, 8)).TrimEnd([char]0)
            Execute = (([BitConverter]::ToUInt32($bytes, $offset + 36)) -band 0x20000000) -ne 0
            Write = (([BitConverter]::ToUInt32($bytes, $offset + 36)) -band 0x80000000) -ne 0
        }
    }
    return [pscustomobject]@{ TlsRva = $tlsRva; Sections = $entries }
}

$dependencies = @(& $Dumpbin /dependents $binary 2>&1 |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ -match '^[A-Za-z0-9_.\-]+\.dll$' })
if ($dependencies.Count -eq 0) {
    throw "no imports could be read from $binary"
}

$importText = (& $Dumpbin /imports $binary 2>&1) -join "`n"
$exportText = (& $Dumpbin /exports $binary 2>&1) -join "`n"
# dumpbin lists an imported symbol as "<hint> <name>"; ordinal-only imports have no
# second column and the DLL/table headers have no leading hint, so this selects
# exactly the named imports.
$importedSymbols = @($importText -split "`n" |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ -match '^[0-9A-Fa-f]{1,8}\s+[A-Za-z_?@][\w?@$]*$' } |
    ForEach-Object { ($_ -split '\s+')[1] })
$importsCxxThrow = $importedSymbols -contains '_CxxThrowException'
$importsPcToFileHeader = $importedSymbols -contains 'RtlPcToFileHeader'

$problems = @()
foreach ($name in $dependencies) {
    if ($name -match 'D\.dll$' -or $name -match '^ucrtbased' -or $name -match '^concrt') {
        $problems += "debug or private runtime dependency: $name"
    } elseif ($name -notmatch '^(KERNEL32|MSVCP140|VCRUNTIME140|VCRUNTIME140_1)\.dll$' -and
              $name -notmatch '^api-ms-win-(crt|core)-') {
        $problems += "unexpected dependency: $name"
    }
}
if ($dependencies -notcontains 'VCRUNTIME140.dll') {
    $problems += 'the release CRT is not linked; the binary looks like a debug build'
}
if ($importsPcToFileHeader) {
    $problems += 'the image links the CRT statically (RtlPcToFileHeader import)'
}
if ($exportText -notmatch '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+AnomalyPluginEntryV1\b') {
    $problems += 'the image does not export AnomalyPluginEntryV1'
}
if ($importsCxxThrow -and
    $exportText -notmatch '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+AnomalyPluginCxxThrowV1\b') {
    $problems += 'the image imports _CxxThrowException without exporting AnomalyPluginCxxThrowV1'
}

$pe = Get-PeFacts $binary
if ($pe.TlsRva -ne 0) {
    $problems += 'the image has a TLS directory (loader-managed static TLS is not mappable)'
}
foreach ($section in $pe.Sections) {
    if ($section.Execute -and $section.Write) {
        $problems += "section $($section.Name) is both writable and executable"
    }
}

# The runtime validates this manifest against the SDK schema before it loads the package,
# and `pack` only warns about a field the schema caps, so the caps are checked here: a
# description over the schema's maximum is a package the host would refuse.
$manifestPath = Join-Path $Package 'manifest.json'
if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    $manifested = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifested.description -and $manifested.description.Length -gt 512) {
        $problems += "manifest description is longer than the schema allows " +
            "($($manifested.description.Length) > 512)"
    }
    if (-not $manifested.id -or -not $manifested.version -or -not $manifested.entry) {
        $problems += 'manifest does not carry id, version and entry'
    }
} else {
    $problems += 'manifest.json is missing'
}

$shaFile = Join-Path $Package 'package.sha256'
if (Test-Path -LiteralPath $shaFile -PathType Leaf) {
    foreach ($line in Get-Content -LiteralPath $shaFile) {
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+(\S.*)$') { continue }
        $expected = $Matches[1].ToLowerInvariant()
        $relative = $Matches[2].Trim() -replace '/', '\'
        $file = Join-Path $Package $relative
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            $problems += "package.sha256 lists a missing file: $relative"
            continue
        }
        $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) { $problems += "sha256 mismatch: $relative" }
    }
}

if ($problems.Count -ne 0) {
    foreach ($problem in $problems) { Write-Error $problem -ErrorAction Continue }
    exit 1
}

$manifest = Join-Path $Package 'manifest.json'
$id = if (Test-Path -LiteralPath $manifest) {
    (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).id
} else {
    '<no manifest>'
}
$exports = @($exportText -split "`n" |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ -match '^\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+\S+' } |
    ForEach-Object { ($_ -split '\s+')[3] -replace ' = .*$', '' })
$full = (Resolve-Path -LiteralPath $Package).Path
Write-Output "ok package=$full id=$id exports=$($exports -join ',') dependencies=$($dependencies -join ',')"
