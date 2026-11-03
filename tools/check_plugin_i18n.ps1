# 插件 i18n 一致性门禁。
#  - locales/zh-CN.json 的形状、key 合法性、消息非空、花括号转义，全部按宿主解析规则复刻；
#  - 代码里 localizer.Text/Format/Label("key","English") 与静态 *_keys 表的 key 是否都在目录里；
#  - 中文消息与英文回退的 {N} 占位符掩码是否完全一致（不一致宿主会静默回退英文）；
#  - 非测试源码是否还残留中文字符串字面量；manifest 是否已交出英文回退并声明 anomaly.localization。
# 用法: pwsh -NoProfile -File tools/check_plugin_i18n.ps1 [-Plugin AutoFish,NteVehicle]
param([string[]]$Plugin)

$Plugin = @($Plugin | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$root = Join-Path (Split-Path $PSScriptRoot -Parent) 'plugins'
if ($Plugin.Count -eq 0) {
    # 省略 -Plugin 时检查 plugins/ 下所有带 manifest 的插件。
    $Plugin = Get-ChildItem -Path $root -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'manifest.json') } |
        ForEach-Object { $_.Name }
}

$root = Join-Path (Split-Path $PSScriptRoot -Parent) 'plugins'
$anyFailed = $false
$inline = 511  # 栈上快路径；超过会走两段式重试，不算失败

# 纯数据文件：CJK 字面量是源数据（MMD 骨骼/表情名等），不是用户可见文案，不参与残留检查。
$dataOnly = @{
    'BetterPose' = @('retarget/*', 'mmd_morph_map.hpp')
}

function Get-ArgumentMask([string]$Text) {
    $mask = 0
    foreach ($match in [regex]::Matches($Text, '\{([0-7])\}')) { $mask = $mask -bor (1 -shl [int]$match.Groups[1].Value) }
    return $mask
}

function Test-Format([string]$Text) {
    for ($index = 0; $index -lt $Text.Length;) {
        if ($Text[$index] -eq '{') {
            if ($index + 1 -lt $Text.Length -and $Text[$index + 1] -eq '{') { $index += 2; continue }
            if ($index + 2 -lt $Text.Length -and $Text[$index + 1] -match '[0-7]' -and $Text[$index + 2] -eq '}') { $index += 3; continue }
            return $false
        }
        if ($Text[$index] -eq '}') {
            if ($index + 1 -lt $Text.Length -and $Text[$index + 1] -eq '}') { $index += 2; continue }
            return $false
        }
        $index++
    }
    return $true
}

foreach ($name in $Plugin) {
    $dir = Join-Path $root $name
    $ok = $true
    Write-Host "`n=== $name ==="

    $catalogPath = Join-Path $dir 'locales\zh-CN.json'
    if (-not (Test-Path $catalogPath)) {
        # 没有目录文件的插件允许是纯英文实现；只要有中文字面量就必须有目录。
        $cjk = Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp |
            Where-Object { $_.Name -notmatch '_tests?\.cpp$|^tests?\.cpp$' } |
            Select-String -Pattern '"[^"]*[\u4e00-\u9fff][^"]*"'
        if ($cjk) { Write-Host "  缺少 locales\zh-CN.json，但源码有中文（$($cjk.Count) 处）"; $anyFailed = $true }
        else { Write-Host '  无目录（纯英文实现）' }
        continue
    }
    $document = Get-Content -Raw -Encoding UTF8 $catalogPath | ConvertFrom-Json
    if ($document.schemaVersion -ne 1) { Write-Host '  schemaVersion 必须是 1'; $ok = $false }
    if ($document.locale -ne 'zh-CN') { Write-Host '  locale 必须是 zh-CN'; $ok = $false }

    $catalogKeys = @{}
    foreach ($property in $document.messages.PSObject.Properties) {
        $key = $property.Name
        if ($key -notmatch '\A[a-z0-9._-]{1,255}\z') { Write-Host "  非法 key: $key"; $ok = $false; continue }
        if ($property.Value -isnot [string] -or $property.Value.Length -eq 0) { Write-Host "  消息为空或非字符串: $key"; $ok = $false; continue }
        if (-not (Test-Format $property.Value)) { Write-Host "  花括号未转义: $key"; $ok = $false; continue }
        $bytes = [Text.Encoding]::UTF8.GetByteCount($property.Value)
        if ($bytes -gt 65536) { Write-Host "  消息超过 64 KiB，超出 Localizer 上限: $key = $bytes"; $ok = $false } elseif ($bytes -gt $inline) { Write-Host "  提示: $key 超过 $inline 字节（走两段式缓冲，功能正常）" }
        $catalogKeys[$key] = [pscustomobject]@{ Text = $property.Value; Mask = Get-ArgumentMask $property.Value }
    }

    # 宿主只读 window.title（插件列表显示名）与 plugin.description；缺 window.title 时列表永远显示
    # manifest 的英文名，plugin.name 则是没人读的死 key。
    if (-not ($catalogKeys.ContainsKey('window.title'))) {
        Write-Host '  目录缺 window.title：插件列表会显示 manifest 英文名，中文名无效'; $ok = $false
    }
    if ($catalogKeys.ContainsKey('plugin.name')) {
        Write-Host '  提示: plugin.name 宿主不读取，显示名请用 window.title'
    }

    $used = @{}
    $english = @{}
    foreach ($file in Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp) {
        if ($file.Name -match 'tests') { continue }
        $source = Get-Content -Raw -Encoding UTF8 $file.FullName
        foreach ($match in [regex]::Matches($source, 'localizer\s*\.\s*(?:Text|Format|Label)\(\s*"([^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"')) {
            $used[$match.Groups[1].Value] = $file.Name
            $english[$match.Groups[1].Value] = $match.Groups[2].Value
        }
        foreach ($array in [regex]::Matches($source, '(?s)constexpr\s+std::array<[^>]*>\s+\w*keys\s*\{(.*?)\};')) {
            foreach ($item in [regex]::Matches($array.Groups[1].Value, '"([^"]+)"')) { $used[$item.Groups[1].Value] = $file.Name }
        }
    }

    $missing = @()
    $mismatch = @()
    foreach ($key in $used.Keys) {
        if (-not $catalogKeys.ContainsKey($key)) { $missing += $key; continue }
        if (-not $english.ContainsKey($key)) { continue }
        $fallback = $english[$key]
        if ((Get-ArgumentMask $fallback) -ne $catalogKeys[$key].Mask) {
            $mismatch += "$key (英文 $(Get-ArgumentMask $fallback) / 中文 $($catalogKeys[$key].Mask))"
        }
        if ($fallback -match '[\u4e00-\u9fff]') { $mismatch += "$key (英文回退含中文)" }
        if (-not (Test-Format $fallback)) { $mismatch += "$key (英文回退花括号未转义)" }
    }

    $unused = $catalogKeys.Keys | Where-Object { -not $used.ContainsKey($_) } | Sort-Object
    Write-Host ("  代码使用 key: {0} / 目录条目: {1}" -f $used.Count, $catalogKeys.Count)
    if ($missing) { Write-Host "  目录缺失: $($missing -join ', ')"; $ok = $false }
    if ($mismatch) { Write-Host "  占位符/回退不一致: $($mismatch -join '; ')"; $ok = $false }
    if ($unused) { Write-Host "  目录未使用: $($unused -join ', ')" }

    $allowed = @()
    if ($dataOnly.ContainsKey($name)) {
        foreach ($pattern in $dataOnly[$name]) {
            $allowed += (Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp | Where-Object {
                $rel = $_.FullName.Substring($dir.Length + 1).Replace('\', '/')
                $rel -like $pattern })
        }
        if ($allowed.Count -ne 0) { $allowed = $allowed | ForEach-Object { $_.FullName } }
    }
    $leftover = Get-ChildItem $dir -Recurse -File -Include *.cpp,*.hpp |
        Where-Object { $_.FullName -notin $allowed } |
        Select-String -Pattern '"[^"]*[\u4e00-\u9fff][^"]*"' |
        Where-Object {
            $_.Path -notmatch '\\locales\\' -and
            $_.Filename -notmatch '_tests?\.cpp$|^tests?\.cpp$' -and
            $_.Line -notmatch 'i18n-data'
        }
    if ($leftover) { Write-Host "  源码仍有中文字符串字面量（$($leftover.Count) 处，如 $($leftover[0].Filename):$($leftover[0].LineNumber)）"; $ok = $false }

    $manifest = Get-Content -Raw -Encoding UTF8 (Join-Path $dir 'manifest.json') | ConvertFrom-Json
    if ("$($manifest.name)$($manifest.description)" -match '[\u4e00-\u9fff]') { Write-Host '  manifest name/description 仍含中文'; $ok = $false }
    $usesLocalizer = (Select-String -Path (Join-Path $dir '*.cpp'), (Join-Path $dir '*.hpp') -Pattern 'Localizer|localization\.hpp' -ErrorAction SilentlyContinue)
    if ($usesLocalizer -and -not ($manifest.services.id -contains 'anomaly.localization')) {
        Write-Host '  代码用 Localizer 但 manifest 未声明 anomaly.localization'; $ok = $false
    }

    if ($ok) { Write-Host '  OK' } else { $anyFailed = $true }
}

if ($anyFailed) { exit 1 }
