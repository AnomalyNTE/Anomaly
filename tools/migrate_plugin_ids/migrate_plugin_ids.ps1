# 插件 id 迁移：把运行时数据里的旧 id 换成 `<作者>.<插件名>`。
# 双击用同目录的 Migrate-Plugin-Ids.cmd；使用说明见同目录 README.md。
# 本目录应当与 AnomalyLauncher.exe 同级，省略 -Root 时自动使用脚本所在目录（及其下的 Anomaly/）。
# 文件以 UTF-8 BOM 保存，Windows 自带的 PowerShell 5.1 与 PowerShell 7 都能直接运行。
#
# 迁移对象（全部位于 -Root 指向的 Anomaly 数据根）：
#   1. config/plugin-enablement.json 的 plugins 键；
#   2. config/plugins/<旧 id>/ -> config/plugins/<新 id>/（目录内文件逐项合并）；
#   3. state/plugins/<旧 id>/  -> state/plugins/<新 id>/（含 signature-knowledge 与插件私有状态）；
#   4. config/plugins/<id>/config-<旧配置键>.json -> config-<新配置键>.json（只有改过配置键的插件需要）。
#
# 三个插件（BetterPose、DamageDashboard）的配置键跟着 id 变了，其余插件配置键不变。
# 已从仓库移除的插件只报告、不迁移；无法识别的 id 默认报告，加 -IncludeThirdParty 时也报告为第三方。
#
# 用法:
#   pwsh -NoProfile -File tools/migrate_plugin_ids.ps1 -Root "D:\Games\NTE\Anomaly"
#   pwsh -NoProfile -File tools/migrate_plugin_ids.ps1 -Root "D:\Games\NTE" -WhatIf
# -Root 可以指向 Anomaly 数据根，也可以指向含 Anomaly 子目录的游戏根。
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Root,
    [string]$Repo,
    [switch]$IncludeThirdParty,
    [switch]$Interactive,
    [switch]$SilentPreviewNotice
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# 旧 id -> 当前 id。覆盖仓库历史里出现过的全部插件 id，以及本轮改名过程中间态。
$renames = [ordered]@{
    # 2024 来源前缀期：anomaly.builtin.* / anomaly.local.* / local.* / 反域名
    'anomaly.builtin.better-pose'              = 'anomaly.better-pose'
    'anomaly.builtin.dll-loader'               = 'anomaly.dll-loader'
    'anomaly.builtin.entity-esp'               = 'anomaly.entity-esp'
    'anomaly.builtin.hifi-vehicle-music'       = 'anomaly.hifi-vehicle-music'
    'anomaly.builtin.map-spawn-exporter'       = 'anomaly.map-spawn-exporter'
    'anomaly.builtin.memory-dump'              = 'anomaly.memory-dump'
    'anomaly.builtin.model-scale'              = 'anomaly.model-scale'
    'anomaly.builtin.nte-position'             = 'anomaly.nte-position'
    'anomaly.builtin.nte-teleport'             = 'anomaly.nte-teleport'
    'anomaly.builtin.nte-quick-ultimate'       = 'shikari.quick-ultimate'
    'anomaly.builtin.nte-time-accel'           = 'wawmew.time-accel'
    'anomaly.builtin.nte.deserted'             = 'wawmew.deserted'
    'anomaly.builtin.pink-paw-heist-esp'       = 'anomaly.pink-paw-heist-esp'
    'anomaly.builtin.teleport-landmarks-probe' = 'anomaly.teleport-landmarks-probe'
    'anomaly.local.box-auto'                   = 'ccyellowstar.box-auto'
    'anomaly.local.combat-box'                 = 'ccyellowstar.combat-box'
    'anomaly.local.damage-dashboard'           = 'haaland-a.damage-dashboard'
    'anomaly.local.nte-interactbox-collector'  = 'anomaly.wallet-collector'
    'anomaly.local.nte-navmesh-demo'           = 'anomaly.nte-navmesh-demo'
    'anomaly.local.nte-pickup-demo'            = 'anomaly.nte-pickup-demo'
    'anomaly.local.nte-vehicle'                = 'anomaly.nte-vehicle'
    'anomaly.local.nte.camera-blur-fix'        = 'anomaly.camera-blur-fix'
    'anomaly.local.nte.camera-tools'           = 'anomaly.camera-tools'
    'anomaly.local.nte.character-studio'       = 'anomaly.character-studio'
    'anomaly.local.nte.fake-uid'               = 'anomaly.fake-uid'
    'anomaly.local.nte.movement-hold-probe'    = 'anomaly.nte-movement-hold'
    'anomaly.local.nte.night999'               = 'wawmew.night999'
    'anomaly.local.nte.ensemble'               = 'anomaly.character-studio'
    'anomaly.local.memory-dump'                = 'anomaly.memory-dump'
    'anomaly.plugin.jelly'                     = 'shikari.jelly'
    'local.clone-enter'                        = 'ccyellowstar.clone-enter'
    'local.nte.free-fly'                       = 'mou-mou-sama.nte-free-fly'
    'local.nte.npc-summon'                     = 'ouo.npc-summon'
    'local.nte.summon-lab'                     = 'anomaly.summon-lab'
    'local.nte.training-dummy'                 = 'anomaly.training-dummy'
    'local.status-bar'                         = 'b1ank.status-bar'
    'local.ui-buttons'                         = 'anomaly.ui-buttons'
    'com.nailong.costume'                      = 'yi621yi.nailong'
    'local.nte.nte-combat-tracker'             = 'shikari.nte-combat-tracker'

    # 改名过程中的中间态（未提交，但本地构建过，可能已经产生数据目录）
    'anomaly.nte-camera-blur-fix'              = 'anomaly.camera-blur-fix'
    'anomaly.nte-camera-tools'                 = 'anomaly.camera-tools'
    'anomaly.nte-character-studio'             = 'anomaly.character-studio'
    'anomaly.nte-fake-uid'                     = 'anomaly.fake-uid'
    'anomaly.nte-movement-hold-probe'          = 'anomaly.nte-movement-hold'
    'anomaly.nte-wallet-collector'             = 'anomaly.wallet-collector'
    'anomaly.nte-navigation'                   = 'anomaly.nte-navmesh-demo'
    'anomaly.nte-nearby-pickup'                = 'anomaly.nte-pickup-demo'
    'anomaly.nte-deserted'                     = 'wawmew.deserted'
    'anomaly.nte-summon-lab'                   = 'anomaly.summon-lab'
    'anomaly.nte-training-dummy'               = 'anomaly.training-dummy'
    'anomaly.nte-ui-buttons'                   = 'anomaly.ui-buttons'
    'wawmew.nte-night999'                      = 'wawmew.night999'
    'mou-mou-sama.free-fly'                    = 'mou-mou-sama.nte-free-fly'
    'nailong.costume'                          = 'yi621yi.nailong'

    # SDK 示例与测试夹具
    'anomaly.example.hello-ui'                 = 'anomaly.hello-ui'
    'anomaly.example.nte-combat-demo'          = 'anomaly.nte-combat-demo'
    'anomaly.example.nte-inspector'            = 'anomaly.nte-inspector'
    'anomaly.example.reliable-config'          = 'anomaly.reliable-config'
    'anomaly.example.tick-counter'             = 'anomaly.tick-counter'
    'anomaly.test.hook-lifecycle'              = 'anomaly.hook-lifecycle'
}

# 已从仓库移除、没有新 id 的插件：只报告，不动数据，由用户决定是否删除。
$removed = @(
    'anomaly.builtin.telemetry'
    'anomaly.builtin.pukaland-tickets'
    'anomaly.builtin.nte-equipment-probe'
    'anomaly.local.equipment-manager'
    'anomaly.local.nte-fast-equip'
    'anomaly.local.nte.free-camera'
    'anomaly.local.nte.neural-upscale'
    'local.nte.combat-stats'
    'local.nte.craft'
    'local.button-kit'
    'buttonkit'
)

# 脱离仓库独立运行时用来判定"未知 id"的合法清单，与仓库 manifest 同步维护。
$knownIds = @(
    'anomaly.better-pose'
    'anomaly.camera-blur-fix'
    'anomaly.camera-tools'
    'anomaly.character-studio'
    'anomaly.dll-loader'
    'anomaly.entity-esp'
    'anomaly.fake-uid'
    'anomaly.hifi-vehicle-music'
    'anomaly.map-spawn-exporter'
    'anomaly.memory-dump'
    'anomaly.model-scale'
    'anomaly.nte-movement-hold'
    'anomaly.nte-navmesh-demo'
    'anomaly.nte-pickup-demo'
    'anomaly.nte-position'
    'anomaly.nte-teleport'
    'anomaly.nte-vehicle'
    'anomaly.pink-paw-heist-esp'
    'anomaly.summon-lab'
    'anomaly.teleport-landmarks-probe'
    'anomaly.training-dummy'
    'anomaly.ui-buttons'
    'anomaly.wallet-collector'
    'b1ank.autofish'
    'b1ank.autotaxi'
    'b1ank.scene-loader'
    'b1ank.skin-changer'
    'b1ank.status-bar'
    'ccyellowstar.box-auto'
    'ccyellowstar.clone-enter'
    'ccyellowstar.combat-box'
    'haaland-a.damage-dashboard'
    'mou-mou-sama.nte-free-fly'
    'ouo.npc-summon'
    'shikari.jelly'
    'shikari.quick-ultimate'
    'wawmew.deserted'
    'wawmew.night999'
    'wawmew.time-accel'
    'yi621yi.nailong'
    'anomaly.hello-ui'
    'anomaly.nte-combat-demo'
    'anomaly.nte-inspector'
    'anomaly.reliable-config'
    'anomaly.tick-counter'
    'anomaly.hook-lifecycle'
)

# 配置键跟着插件 id 变的只有这两个：旧键 -> 新键。
$configKeyRenames = [ordered]@{
    'anomaly.builtin.character-pose.settings' = 'anomaly.better-pose.settings'
    'damage-dashboard.settings'               = 'haaland-a.damage-dashboard.settings'
}

function Resolve-AnomalyRoot {
    param([string]$Path)

    if (-not $Path) {
        throw '缺少 -Root。示例：-Root "D:\Games\NTE\Anomaly" 或 -Root "D:\Games\NTE"'
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "路径不存在：$Path"
    }

    $candidates = @($Path, (Join-Path $Path 'Anomaly'))
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        $hasConfig = Test-Path -LiteralPath (Join-Path $candidate 'config')
        $hasState = Test-Path -LiteralPath (Join-Path $candidate 'state')
        if ($hasConfig -or $hasState) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "在 $Path 下找不到 config/ 或 state/。请把 -Root 指向 Anomaly 数据根（含 config 与 state 的那层）。"
}

function Get-CurrentPluginIds {
    param([string]$RepositoryRoot)

    $result = @{}
    if (-not $RepositoryRoot) {
        foreach ($id in $knownIds) { $result[$id] = $true }
        return $result
    }
    $patterns = @(
        (Join-Path $RepositoryRoot 'plugins'),
        (Join-Path $RepositoryRoot 'examples')
    )
    $manifests = @()
    foreach ($folder in $patterns) {
        if (-not (Test-Path -LiteralPath $folder)) { continue }
        $manifests += Get-ChildItem -LiteralPath $folder -Directory |
            ForEach-Object { Join-Path $_.FullName 'manifest.json' } |
            Where-Object { Test-Path -LiteralPath $_ }
    }
    $fixture = Join-Path $RepositoryRoot 'apps/test_host/hook_lifecycle_fixture_manifest.json'
    if (Test-Path -LiteralPath $fixture) { $manifests += $fixture }

    foreach ($manifest in $manifests) {
        try {
            $document = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($document.id) { $result[$document.id] = $true }
        } catch {
            Write-Warning "跳过无法解析的 manifest：$manifest"
        }
    }

    if ($result.Count -eq 0) {
        foreach ($id in $knownIds) { $result[$id] = $true }
    }
    return $result
}

function Get-RenamedConfigFileName {
    param([string]$Name)

    if (-not $Name.StartsWith('config-') -or -not $Name.EndsWith('.json')) { return $Name }
    $key = $Name.Substring('config-'.Length)
    $key = $key.Substring(0, $key.Length - '.json'.Length)

    if ($script:configKeyRenames.Contains($key)) {
        return 'config-' + $script:configKeyRenames[$key] + '.json'
    }
    foreach ($oldId in $script:renames.Keys) {
        if ($key -eq $oldId -or $key.StartsWith($oldId + '.')) {
            return 'config-' + $script:renames[$oldId] + $key.Substring($oldId.Length) + '.json'
        }
    }
    return $Name
}

function Convert-StableWindowId {
    param([string]$StableId)

    # 宿主把窗口 id 拼成 "<长度>;<插件 id>;<长度>;<窗口局部 id>;"（见 ui_resource_registry.cpp）。
    # 改插件 id 时长度前缀必须一起重算，否则窗口尺寸/开关状态全部对不上。
    $first = $StableId.IndexOf(';')
    if ($first -lt 0) { return $StableId }
    $second = $StableId.IndexOf(';', $first + 1)
    if ($second -lt 0) { return $StableId }
    $owner = $StableId.Substring($first + 1, $second - $first - 1)
    if (-not $script:renames.Contains($owner)) { return $StableId }
    $mapped = $script:renames[$owner]
    return ('{0};{1};{2}' -f $mapped.Length, $mapped, $StableId.Substring($second + 1))
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Merge-Directory {
    param(
        [string]$Source,
        [string]$Target,
        [string]$Kind,
        [ref]$Stats
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        # 上一次中断（Ctrl+C、管道被截断）可能已经把它搬走了，跳过即可。
        return
    }

    # -WhatIf 下前一个 move 不会落盘，所以目标是否存在要同时看磁盘和本批次的计划。
    $targetExists = (Test-Path -LiteralPath $Target) -or $script:plannedMoves.ContainsKey($Target)
    if (-not $targetExists) {
        if (-not $WhatIfPreference) {
            Move-Item -LiteralPath $Source -Destination $Target
        }
        $script:plannedMoves[$Target] = $true
        # 随目录一起搬走的文件也登记下来，后面逐文件合并时才能算出真实冲突。
        foreach ($moved in (Get-ChildItem -LiteralPath $Source -Recurse -File -ErrorAction SilentlyContinue)) {
            $script:plannedFiles[(Join-Path $Target $moved.FullName.Substring($Source.Length + 1))] = $true
        }
        $Stats.Value.Moved++
        Write-Host ("    move  {0} -> {1}" -f (Split-Path $Source -Leaf), (Split-Path $Target -Leaf))
        return
    }

    # 目标已存在：逐文件合并，同名文件保留目标侧并报告。
    Write-Host ("    merge   {0} -> {1}（目标已存在，逐文件合并）" -f (Split-Path $Source -Leaf), (Split-Path $Target -Leaf))
    $files = @(Get-ChildItem -LiteralPath $Source -Recurse -File -ErrorAction SilentlyContinue)
    $sourceRootLength = $Source.Length + 1
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($sourceRootLength)
        $leaf = Split-Path $relative -Leaf
        $renamedLeaf = Get-RenamedConfigFileName -Name $leaf
        if ($renamedLeaf -ne $leaf) {
            $parentRelative = Split-Path $relative -Parent
            if ($parentRelative) { $relative = Join-Path $parentRelative $renamedLeaf } else { $relative = $renamedLeaf }
        }
        $destination = Join-Path $Target $relative
        if ((Test-Path -LiteralPath $destination) -or $script:plannedFiles.ContainsKey($destination)) {
            $Stats.Value.Conflicts++
            Write-Host ("    conflict  {0} 已存在，保留目标侧" -f $relative) -ForegroundColor Yellow
            continue
        }
        $parent = Split-Path -Parent $destination
        if ($parent -and -not (Test-Path -LiteralPath $parent)) {
            if (-not $WhatIfPreference) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        }
        if (-not $WhatIfPreference) {
            Move-Item -LiteralPath $file.FullName -Destination $destination
        }
        $script:plannedFiles[$destination] = $true
        $Stats.Value.MergedFiles++
    }

    # 源目录搬空后删除；仍有同名冲突文件时保留并报告，交给人工处理。
    $remaining = @(Get-ChildItem -LiteralPath $Source -Recurse -File -ErrorAction SilentlyContinue)
    if ($remaining.Count -eq 0) {
        if (-not $WhatIfPreference) { Remove-Item -LiteralPath $Source -Recurse -Force }
    } else {
        $Stats.Value.KeptDirectories++
        Write-Host ("    kept    {0} 仍有 {1} 个文件与目标同名，旧目录保留待人工处理" -f
            (Split-Path $Source -Leaf), $remaining.Count) -ForegroundColor DarkYellow
    }
}

function Get-PluginRoots {
    param([string]$AnomalyRoot)
    $roots = [ordered]@{}
    foreach ($name in 'config', 'state') {
        $root = Join-Path (Join-Path $AnomalyRoot $name) 'plugins'
        if (Test-Path -LiteralPath $root) { $roots[$name] = $root }
    }
    return $roots
}

# ---------------------------------------------------------------------------

if (-not $Root) { $Root = $PSScriptRoot }

if ($Interactive) {
    & $PSCommandPath -WhatIf -SilentPreviewNotice
    Write-Host ''
    Write-Host '以上是预览，还没有改动任何文件。' -ForegroundColor Cyan
    $answer = Read-Host '确认执行迁移? 输入 Y 继续，直接回车取消'
    if ($answer -notmatch '^(?i)\s*y') {
        Write-Host ''
        Write-Host '已取消，没有改动任何文件。' -ForegroundColor Yellow
        exit 0
    }
    Write-Host ''
    & $PSCommandPath
    exit $LASTEXITCODE
}

$anomalyRoot = Resolve-AnomalyRoot -Path $Root
$repoRoot = if ($Repo) { (Resolve-Path -LiteralPath $Repo).Path } else { Split-Path (Split-Path $PSScriptRoot -Parent) -Parent }
$currentIds = Get-CurrentPluginIds -RepositoryRoot $repoRoot

$plannedMoves = @{}
$plannedFiles = @{}
$stats = [pscustomobject]@{ Moved = 0; MergedFiles = 0; Conflicts = 0; EnablementRenamed = 0;
                            EnablementConflicts = 0; ConfigFilesRenamed = 0; Removed = 0; Unknown = 0; KeptDirectories = 0; EnablementMerged = 0; UiStateRenamed = 0 }

Write-Host "Anomaly 数据根: $anomalyRoot"
$fromRepo = $false
if ($repoRoot -and (Test-Path -LiteralPath (Join-Path $repoRoot 'plugins'))) { $fromRepo = $true }
if ($fromRepo) {
    Write-Host ("合法 id 清单: {0} 个（来自仓库 {1}）" -f $currentIds.Count, $repoRoot)
} else {
    Write-Host ("合法 id 清单: {0} 个（内置，独立运行）" -f $currentIds.Count)
}
Write-Host ''

# 1) plugin-enablement.json
$enablementPath = Join-Path (Join-Path $anomalyRoot 'config') 'plugin-enablement.json'
if (Test-Path -LiteralPath $enablementPath) {
    Write-Host '=== config/plugin-enablement.json ==='
    $document = Get-Content -LiteralPath $enablementPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $plugins = [ordered]@{}
    if ($null -ne $document.plugins) {
        foreach ($property in $document.plugins.PSObject.Properties) {
            $plugins[$property.Name] = [bool]$property.Value
        }
    }
    $changed = $false
        $squashed = @{}
        foreach ($oldId in @($plugins.Keys)) {
            if (-not $renames.Contains($oldId)) {
                if ($removed -contains $oldId) {
                    $stats.Removed++
                    Write-Host ("    removed   {0} 对应的插件已移除，启用状态保留待人工处理" -f $oldId) -ForegroundColor DarkYellow
                } elseif ($currentIds.Count -gt 0 -and -not $currentIds.ContainsKey($oldId)) {
                    $stats.Unknown++
                    Write-Host ("    unknown   {0} 不在仓库清单里，未处理" -f $oldId) -ForegroundColor DarkGray
                }
                continue
            }
            $newId = $renames[$oldId]
            if ($plugins.Contains($newId)) {
                if ($squashed.ContainsKey($newId)) {
                    # 目标键是本次由另一个旧 id 写进来的，两边都是同一插件的旧记录：启用优先。
                    $merged = [bool]$plugins[$newId] -or [bool]$plugins[$oldId]
                    $plugins[$newId] = $merged
                    $stats.EnablementMerged++
                    Write-Host ("    merge     {0} ({1}) -> {2} 与另一条旧记录合并，取 enabled={3}" -f
                        $oldId, $plugins[$oldId], $newId, $merged)
                } else {
                    $stats.EnablementConflicts++
                    Write-Host ("    conflict  {0} ({1}) -> {2} 已存在，保留新 id 的值 {3}" -f
                        $oldId, $plugins[$oldId], $newId, $plugins[$newId]) -ForegroundColor Yellow
                }
            } else {
                $plugins[$newId] = $plugins[$oldId]
                $squashed[$newId] = $true
                Write-Host ("    rename    {0} -> {1} (enabled={2})" -f $oldId, $newId, $plugins[$newId])
                $stats.EnablementRenamed++
            }
            $plugins.Remove($oldId)
            $changed = $true
        }
        if ($changed -and -not $WhatIfPreference) {
            $output = [ordered]@{}
            foreach ($property in $document.PSObject.Properties) {
                if ($property.Name -eq 'plugins') { $output['plugins'] = $plugins } else { $output[$property.Name] = $property.Value }
            }
            $json = $output | ConvertTo-Json -Depth 8
            Write-Utf8NoBom -Path $enablementPath -Text ($json + [Environment]::NewLine)
        }
        if (-not $changed) { Write-Host '    nothing to do' }
    Write-Host ''
}

# 1b) state/ui-window-state.json：favorites、pluginWindows[].pluginId、windows[].stableId
$uiStatePath = Join-Path (Join-Path $anomalyRoot 'state') 'ui-window-state.json'
if (-not (Test-Path -LiteralPath $uiStatePath)) {
    Write-Host '=== state/ui-window-state.json ==='
    Write-Host '    未找到：这份数据没保存过插件收藏或窗口状态，跳过' -ForegroundColor DarkGray
    Write-Host ''
} else {
    Write-Host '=== state/ui-window-state.json ==='
    $document = Get-Content -LiteralPath $uiStatePath -Raw -Encoding UTF8 | ConvertFrom-Json
    $changed = $false

    if ($null -ne $document.favorites) {
        $favorites = @()
        foreach ($favoriteId in @($document.favorites)) {
            $mapped = if ($renames.Contains($favoriteId)) { $renames[$favoriteId] } else { $favoriteId }
            if ($mapped -ne $favoriteId) {
                Write-Host ("    rename    favorites: {0} -> {1}" -f $favoriteId, $mapped)
                $stats.UiStateRenamed++
                $changed = $true
            }
            $favorites += $mapped
        }
        $document.favorites = $favorites
    }

    if ($null -ne $document.pluginWindows) {
        foreach ($window in @($document.pluginWindows)) {
            if ($null -eq $window -or -not $window.PSObject.Properties['pluginId']) { continue }
            $oldId = $window.pluginId
            if (-not $renames.Contains($oldId)) { continue }
            $window.pluginId = $renames[$oldId]
            Write-Host ("    rename    pluginWindows: {0} -> {1}" -f $oldId, $window.pluginId)
            $stats.UiStateRenamed++
            $changed = $true
        }
    }

    if ($null -ne $document.windows) {
        foreach ($window in @($document.windows)) {
            if ($null -eq $window -or -not $window.PSObject.Properties['stableId']) { continue }
            $oldStable = $window.stableId
            $newStable = Convert-StableWindowId -StableId $oldStable
            if ($newStable -eq $oldStable) { continue }
            $window.stableId = $newStable
            Write-Host ("    rename    windows.stableId: {0} -> {1}" -f $oldStable, $newStable)
            $stats.UiStateRenamed++
            $changed = $true
        }
    }

    if ($changed -and -not $WhatIfPreference) {
        $json = $document | ConvertTo-Json -Depth 8
        Write-Utf8NoBom -Path $uiStatePath -Text ($json + [Environment]::NewLine)
    }
    if (-not $changed) { Write-Host '    nothing to do' }
    Write-Host ''
}

# 2) config/plugins/<id>/config-<旧配置键>.json（先改名，再迁移目录）
$planned = @{}
$configPlugins = Join-Path (Join-Path $anomalyRoot 'config') 'plugins'
if (Test-Path -LiteralPath $configPlugins) {
    Write-Host '=== 配置键重命名 ==='
    $renamedAny = $false
    foreach ($directory in Get-ChildItem -LiteralPath $configPlugins -Directory) {
        foreach ($file in Get-ChildItem -LiteralPath $directory.FullName -File -Filter 'config-*.json') {
            $key = $file.Name.Substring('config-'.Length); $key = $key.Substring(0, $key.Length - '.json'.Length)
            $newName = Get-RenamedConfigFileName -Name $file.Name
            if ($newName -eq $file.Name) { continue }
            $target = Join-Path $directory.FullName $newName
            if ($planned.ContainsKey($target)) {
                $stats.Conflicts++
                Write-Host ("    conflict  {0} 已被本批次另一处占用，保留先到者" -f (Split-Path $target -Leaf)) -ForegroundColor Yellow
                continue
            }
            if (Test-Path -LiteralPath $target) {
                $stats.Conflicts++
                Write-Host ("    conflict  {0} 已存在，保留目标侧" -f (Split-Path $target -Leaf)) -ForegroundColor Yellow
                continue
            }
            if (-not $WhatIfPreference) {
                Move-Item -LiteralPath $file.FullName -Destination $target
            }
            $planned[$target] = $true
            $stats.ConfigFilesRenamed++
            $renamedAny = $true
            Write-Host ("    rename    {0} -> {1}" -f $file.Name, (Split-Path $target -Leaf))
        }
    }
    if (-not $renamedAny) { Write-Host '    nothing to do' }
    Write-Host ''
}

# 3) config/plugins 与 state/plugins 目录迁移
foreach ($entry in (Get-PluginRoots -AnomalyRoot $anomalyRoot).GetEnumerator()) {
    $kind = $entry.Key
    $root = $entry.Value
    Write-Host "=== $kind/plugins ==="
    $names = @(Get-ChildItem -LiteralPath $root -Directory | ForEach-Object { $_.Name } | Sort-Object)
    $touched = $false

    foreach ($name in $names) {
        $source = Join-Path $root $name
        if ($renames.Contains($name)) {
            $touched = $true
            Merge-Directory -Source $source -Target (Join-Path $root $renames[$name]) -Kind $kind -Stats ([ref]$stats)
            continue
        }
        if ($removed -contains $name) {
            $stats.Removed++
            Write-Host ("    removed   {0} 对应的插件已移除，数据保留待人工处理" -f $name) -ForegroundColor DarkYellow
            continue
        }
        if ($currentIds.Count -gt 0 -and -not $currentIds.ContainsKey($name)) {
            $stats.Unknown++
            $label = if ($IncludeThirdParty) { 'third-party' } else { 'unknown' }
            Write-Host ("    {0,-10}{1} 不在仓库清单里，未处理" -f $label, $name) -ForegroundColor DarkGray
        }
    }
    if (-not $touched) { Write-Host '    nothing to do' }
    Write-Host ''
}

Write-Host '=== 汇总 ==='
Write-Host ("    启用状态键改名      {0}" -f $stats.EnablementRenamed)
Write-Host ("    启用状态键合并      {0}" -f $stats.EnablementMerged)
Write-Host ("    启用状态键冲突      {0}" -f $stats.EnablementConflicts)
Write-Host ("    目录整体迁移        {0}" -f $stats.Moved)
Write-Host ("    目录内文件合并      {0}" -f $stats.MergedFiles)
Write-Host ("    窗口/收藏状态改名   {0}" -f $stats.UiStateRenamed)
Write-Host ("    配置键文件改名      {0}" -f $stats.ConfigFilesRenamed)
Write-Host ("    文件冲突(保留新侧)  {0}" -f $stats.Conflicts)
Write-Host ("    已移除插件的残留    {0}" -f $stats.Removed)
Write-Host ("    合并后保留的旧目录  {0}" -f $stats.KeptDirectories)
Write-Host ("    未识别 id           {0}" -f $stats.Unknown)

if ($WhatIfPreference -and -not $SilentPreviewNotice) {
    Write-Host ''
    Write-Host '以上为预览，没有改动磁盘。确认无误后去掉 -WhatIf 再执行一次。' -ForegroundColor Cyan
}

if ($stats.Conflicts -gt 0 -or $stats.EnablementConflicts -gt 0) { exit 0 }
exit 0
