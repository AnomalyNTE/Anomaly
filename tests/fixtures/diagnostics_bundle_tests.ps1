param(
    [string]$Collector = (Join-Path $PSScriptRoot '..\..\tools\collect_diagnostics.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-FixtureFile {
    param([string]$Path, [AllowEmptyString()][string]$Content)

    New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($Path)) | Out-Null
    Set-Content -LiteralPath $Path -Value $Content -Encoding utf8NoBOM -NoNewline
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$root = Join-Path ([IO.Path]::GetTempPath()) (
    'anomaly-diagnostics-fixture-' + [guid]::NewGuid().ToString('N'))
$runtime = Join-Path $root 'runtime'
$expanded = Join-Path $root 'expanded'
$bundle = Join-Path $root 'diagnostics.zip'

try {
    Write-FixtureFile (Join-Path $runtime 'anomaly.ini') @'
[remote]
token=ini-token-value
apiKey="ini-api-key"
home=C:\Users\FixtureUser\AppData\Roaming
'@
    Write-FixtureFile (Join-Path $runtime 'repository.json') @'
{"schemaVersion":1,"enabled":false,"allowFileSources":false,"withdrawalPolicy":"block-new","freshness":{"maximumClockSkewSeconds":300,"maximumIndexAgeSeconds":86400,"maximumOfflineAgeSeconds":604800,"downgradePolicy":"reject"},"sources":[],"trustKeys":[]}
'@
    Write-FixtureFile (Join-Path $runtime 'anomaly-platform.log') @'
Authorization: Bearer platform-authorization-value
event={"token":"embedded-json-token-value","public":"discarded-after-secret"}
repeat-me
repeat-me
home=C:/Users/FixtureUser/Documents/runtime.log
secondHome=C:\Users\Fixture User\Desktop\runtime.log
'@
    Write-FixtureFile (Join-Path $runtime 'anomaly-runtime.log') @'
password=runtime-password-value
path=/home/fixture-user/.config/anomaly
'@
    Write-FixtureFile (Join-Path $runtime 'logs\runtime.log') @'
secret='log-secret-value'
public=kept
'@
    Write-FixtureFile (Join-Path $runtime 'logs\events.jsonl') @'
{"token":"jsonl-token-value","nested":{"apiKey":"jsonl-api-key-value"},"path":"C:\\Users\\FixtureUser\\trace"}
{"authorization":"Bearer jsonl-authorization-value","password":"jsonl-password-value"}
'@
    Write-FixtureFile (Join-Path $runtime 'profiles\nte\active.json') @'
{"schemaVersion":1,"buildId":"fixture","authorization":"profile-authorization-value"}
'@
    Write-FixtureFile (Join-Path $runtime 'profiles-local\nte\override.json') @'
{"schemaVersion":1,"buildId":"local","clientSecret":"local-client-secret-value"}
'@
    Write-FixtureFile (Join-Path $runtime 'state\profiles\managed\nte\managed.json') @'
{"schemaVersion":1,"buildId":"managed","access_token":"managed-access-token-value"}
'@
    Write-FixtureFile (Join-Path $runtime 'state\diagnostics-summary.json') @'
{"schemaVersion":1,"runtimeVersion":"1.0.0","runtimeState":"running","profile":{"ok":true,"state":"supported","buildId":"fixture-build","profileHash":"fixture-profile-hash","profileSource":"C:\\Users\\FixtureUser\\profile.json","token":"summary-token-value"}}
'@
    Write-FixtureFile (Join-Path $runtime 'plugins\Example\manifest.json') @'
{"schemaVersion":1,"id":"fixture.plugin","name":"Fixture","version":"1.0.0","entry":"plugin.dll","api":{"major":1,"minMinor":0,"maxMinor":0},"games":["nte"],"builds":["*"],"loadPhase":"game-ready","apiKey":"manifest-api-key-value"}
'@

    # These files deliberately contain unique sentinel secrets and must never enter the ZIP.
    Write-FixtureFile (Join-Path $runtime 'plugins\Example\plugin.dll') 'dll-private-value'
    Write-FixtureFile (Join-Path $runtime 'plugins\Example\private.json') 'plugin-private-value'
    Write-FixtureFile (Join-Path $runtime 'plugins\Example\nested\manifest.json') 'nested-manifest-private-value'
    Write-FixtureFile (Join-Path $runtime 'plugins\.cache\shadow\manifest.json') 'cache-manifest-private-value'
    Write-FixtureFile (Join-Path $runtime 'logs\nested\private.log') 'nested-log-private-value'
    Write-FixtureFile (Join-Path $runtime 'cache\private.log') 'cache-log-private-value'
    Write-FixtureFile (Join-Path $runtime 'state\profile-symbol-cache.json') 'profile-cache-private-value'
    Write-FixtureFile (Join-Path $runtime 'config\plugins\Example\config-settings.json') 'plugin-config-private-value'
    Write-FixtureFile (Join-Path $runtime 'crash.dmp') 'dump-private-value'
    Write-FixtureFile (Join-Path $runtime 'unexpected.json') 'unexpected-private-value'

    $resolvedCollector = [IO.Path]::GetFullPath($Collector)
    & $resolvedCollector -RuntimeDirectory $runtime -Output $bundle | Out-Null
    Assert-True (Test-Path -LiteralPath $bundle -PathType Leaf) 'diagnostic ZIP was not created'
    Expand-Archive -LiteralPath $bundle -DestinationPath $expanded

    $expectedPaths = @(
        'anomaly-platform.log'
        'anomaly.ini'
        'repository.json'
        'logs/events.jsonl'
        'logs/runtime.log'
        'manifest.json'
        'plugins/Example/manifest.json'
        'profiles-local/nte/override.json'
        'profiles/nte/active.json'
        'state/profiles/managed/nte/managed.json'
        'state/diagnostics-summary.json'
        'anomaly-runtime.log'
    ) | Sort-Object
    $actualPaths = @(Get-ChildItem -LiteralPath $expanded -Recurse -File | ForEach-Object {
        [IO.Path]::GetRelativePath($expanded, $_.FullName).Replace(
            [IO.Path]::DirectorySeparatorChar, '/')
    } | Sort-Object)
    $difference = @(Compare-Object -ReferenceObject $expectedPaths -DifferenceObject $actualPaths)
    Assert-True ($difference.Count -eq 0) (
        'diagnostic ZIP violated the strict allowlist: ' + ($difference | Out-String))

    $allText = [string]::Join("`n", @(Get-ChildItem -LiteralPath $expanded -Recurse -File |
        ForEach-Object { [string](Get-Content -LiteralPath $_.FullName -Raw) }))
    foreach ($sentinel in @(
        'ini-token-value', 'ini-api-key', 'platform-authorization-value',
        'embedded-json-token-value',
        'runtime-password-value', 'log-secret-value', 'jsonl-token-value',
        'jsonl-api-key-value', 'jsonl-authorization-value', 'jsonl-password-value',
        'profile-authorization-value', 'local-client-secret-value',
        'managed-access-token-value', 'manifest-api-key-value',
        'summary-token-value',
        'dll-private-value', 'plugin-private-value', 'nested-manifest-private-value',
        'cache-manifest-private-value', 'nested-log-private-value', 'cache-log-private-value',
        'profile-cache-private-value',
        'plugin-config-private-value', 'dump-private-value', 'unexpected-private-value')) {
        Assert-True (-not $allText.Contains($sentinel, [StringComparison]::OrdinalIgnoreCase)) (
            "diagnostic ZIP leaked sentinel: $sentinel")
    }
    Assert-True ($allText.Contains('<redacted>', [StringComparison]::Ordinal)) (
        'diagnostic ZIP did not contain redaction markers')
    Assert-True ($allText.Contains('<user-home>', [StringComparison]::Ordinal)) (
        'diagnostic ZIP did not contain user-home redaction markers')
    Assert-True (-not [regex]::IsMatch($allText,
        '(?i)(?:C:[\\/]+Users[\\/]+Fixture(?:User| User)|/(?:home|Users)/fixture-user)')) (
        'diagnostic ZIP leaked a user-home path')

    $summary = Get-Content -LiteralPath (
        Join-Path $expanded 'state\diagnostics-summary.json') -Raw | ConvertFrom-Json
    Assert-True ($summary.runtimeVersion -ceq '1.0.0') (
        'diagnostic summary dropped the runtime version')
    Assert-True ($summary.runtimeState -ceq 'running') (
        'diagnostic summary dropped the runtime state')
    Assert-True ($summary.profile.buildId -ceq 'fixture-build') (
        'diagnostic summary dropped the build ID')
    Assert-True ($summary.profile.profileHash -ceq 'fixture-profile-hash') (
        'diagnostic summary dropped the profile hash')
    $repository = Get-Content -LiteralPath (
        Join-Path $expanded 'repository.json') -Raw | ConvertFrom-Json
    Assert-True ($repository.schemaVersion -eq 1 -and -not $repository.enabled) (
        'diagnostic bundle dropped the repository configuration state')

    $platformLog = Get-Content -LiteralPath (Join-Path $expanded 'anomaly-platform.log') -Raw
    Assert-True (([regex]::Matches($platformLog, '(?m)^repeat-me$')).Count -eq 1) (
        'diagnostic log lines were not deduplicated')

    Get-ChildItem -LiteralPath $expanded -Recurse -Filter '*.json' -File | ForEach-Object {
        if ($_.Name -ne 'manifest.json' -or $_.DirectoryName -ne $expanded) {
            [void](Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json)
        }
    }
    Get-Content -LiteralPath (Join-Path $expanded 'logs\events.jsonl') | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    } | ForEach-Object { [void]($_ | ConvertFrom-Json) }
    $pluginManifest = Get-Content -LiteralPath (
        Join-Path $expanded 'plugins\Example\manifest.json') -Raw | ConvertFrom-Json
    Assert-True ($pluginManifest.games -is [array] -and $pluginManifest.games.Count -eq 1) (
        'diagnostic JSON redaction changed an array into a scalar')

    $manifestEntries = @(
        Get-Content -LiteralPath (Join-Path $expanded 'manifest.json') -Raw | ConvertFrom-Json)
    $manifestPaths = @($manifestEntries | ForEach-Object { $_.Path })
    Assert-True (($manifestPaths -join "`n") -ceq (($manifestPaths | Sort-Object) -join "`n")) (
        'diagnostic manifest is not sorted')
    Assert-True ($manifestEntries.Count -eq ($expectedPaths.Count - 1)) (
        'diagnostic manifest entry count is incorrect')
    foreach ($entry in $manifestEntries) {
        $file = Join-Path $expanded ($entry.Path -replace '/', '\')
        Assert-True (Test-Path -LiteralPath $file -PathType Leaf) (
            "diagnostic manifest references a missing file: $($entry.Path)")
        $item = Get-Item -LiteralPath $file
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-True ($item.Length -eq [long]$entry.Size) (
            "diagnostic manifest size mismatch: $($entry.Path)")
        Assert-True ($hash -ceq [string]$entry.Sha256) (
            "diagnostic manifest hash mismatch: $($entry.Path)")
    }

    Write-Output 'diagnostics bundle fixture passed'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
