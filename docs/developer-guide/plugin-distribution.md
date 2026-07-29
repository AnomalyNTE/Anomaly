# 发布第三方插件

这页接着[插件开发](plugin-development.md)往下讲：插件已经能编译和运行，接下来怎样把它放进 Anomaly 的 **Plugins > 可用** 页面。

在线分发涉及发布端和使用端两份 JSON：

```text
发布端
  pluginmaster.json              插件索引，列出版本和 ZIP 下载地址
  插件 ZIP                        manifest.json、plugin.dll 和资源
  图标                            可选，供 IconUrl 直接访问

使用端
  Anomaly\plugin-repositories.json  记录一个或多个 pluginmaster.json 地址
```

发布者维护并托管 `pluginmaster.json`；用户通常在界面中添加它的 URL，Runtime 会把该 URL 保存到 `plugin-repositories.json`。插件列表、ZIP 和图标都通过 HTTPS 提供。它们不走 Profile 使用的 `repository.json` 签名索引，也不要打成 `.anomaly-package`。

## 1. 先把插件包测通

下面假设 CMake 已经把目录包输出到 `build\package\MyPlugin`：

```powershell
anomaly-plugin validate .\build\package\MyPlugin
anomaly-plugin pack .\build\package\MyPlugin --output .\dist\packed
anomaly-test-host --plugin .\dist\packed\com.example.my-plugin --reload 10 --ticks 60
```

`anomaly-plugin pack` 会按 Manifest ID 创建目录，并生成 `package.sha256`。它的输出还是目录，不是第三方插件页面能下载的 ZIP。

具体构建和 TestHost 用法见[插件开发](plugin-development.md#8-本地开发循环)。

## 2. 制作下载 ZIP

压缩的是包目录里的**内容**，不要把最外层目录一起套进去：

```powershell
$Packed = '.\dist\packed\com.example.my-plugin'
$Zip = '.\dist\com.example.my-plugin-1.2.0.zip'
Compress-Archive -Path "$Packed\*" -DestinationPath $Zip -Force
```

打开 ZIP 时，第一层应该直接看到：

```text
manifest.json
plugin.dll
package.sha256
assets\               可选
locales\              可选
其他 native DLL       可选
```

下面这种结构不能安装，因为 `manifest.json` 不在 ZIP 根目录：

```text
com.example.my-plugin\
  manifest.json
  plugin.dll
```

发布前再核对一次 Manifest：

- `id` 是稳定的点分小写 ID，后续版本不要更换。
- `version` 是有效的 SemVer，并且与准备发布的版本一致。
- `games` 包含 `nte`，`api.major` 与目标 Runtime 一致。
- `entry` 指向 ZIP 内实际存在的 DLL。
- `license`、依赖、服务和 capability 都已写全。

## 3. 编写 pluginmaster.json

`pluginmaster.json` 的根必须是 JSON 数组。通常一个插件 ID 只放当前版本：

```json
[
  {
    "Name": "My Plugin",
    "InternalName": "com.example.my-plugin",
    "Version": "1.2.0",
    "Author": "Example Author",
    "Punchline": "在游戏内显示一项实用信息。",
    "Description": "说明插件解决什么问题、怎么使用，以及已知限制。",
    "Games": ["nte"],
    "ApiMajor": 1,
    "Tags": ["ui", "utility"],
    "RepoUrl": "https://github.com/example/my-plugin",
    "IconUrl": "https://example.com/my-plugin/icon.png",
    "DownloadLinkInstall": "https://github.com/example/my-plugin/releases/download/v1.2.0/com.example.my-plugin-1.2.0.zip",
    "DownloadLinkUpdate": "https://github.com/example/my-plugin/releases/download/v1.2.0/com.example.my-plugin-1.2.0.zip",
    "AcceptsFeedback": true
  }
]
```

JSON 字段名区分大小写，不支持注释或尾随逗号。索引中有多个插件时，在同一个数组中继续添加对象，不要给根节点套 `plugins` 或其他字段。

可参考 [Anomaly 插件模板的 `pluginmaster.json`](https://raw.githubusercontent.com/AnomalyNTE/Anomaly-Plugin-Template/main/pluginmaster.json) 查看一份可直接被 Runtime 读取的在线示例。

### 必填字段

| 字段 | 要求 |
| --- | --- |
| `Name` | 给用户看的插件名，不能为空。 |
| `InternalName` | 必须与 `manifest.json` 的 `id` 完全相同。 |
| `Version` | 必须与 Manifest 版本完全相同，并且是有效 SemVer。 |
| `DownloadLinkInstall` | 可直接下载 ZIP 的 HTTPS 地址。 |

### 建议填写

| 字段 | 用途 |
| --- | --- |
| `Author` | 作者或维护者。 |
| `Punchline` | 列表中的一句话简介，尽量直接说用途。 |
| `Description` | 更完整的功能、使用方法和限制。 |
| `Games` | 目前应为 `["nte"]`，用于在下载前判断兼容性。 |
| `ApiMajor` | 目前为 `1`，用于在下载前判断兼容性。 |
| `Tags` | 描述插件类别的短标签元数据。 |
| `RepoUrl` | 源码、问题反馈或项目主页。 |
| `IconUrl` | 插件图标的 HTTPS 地址。 |
| `AcceptsFeedback` | 是否接受用户反馈。 |

解析器也接受 `DownloadLinkUpdate`。当前安装和更新都以 `DownloadLinkInstall` 为下载地址，因此可以不写，或者与 `DownloadLinkInstall` 保持一致。

列表里缺字段的单项会被跳过；整个文件不是合法 JSON 数组时，该插件源会刷新失败。字段合同以 [`include/anomaly/plugin_list.hpp`](../../include/anomaly/plugin_list.hpp) 为准。

### 与 manifest.json 对应

以下字段描述的是同一个发布包。身份、版本和 API 主版本会在安装时核对；游戏兼容范围也应保持一致：

| `pluginmaster.json` | `manifest.json` / 发布产物 | 要求 |
| --- | --- | --- |
| `InternalName` | `id` | 必须完全相同。 |
| `Version` | `version` | 必须完全相同，并使用 SemVer。 |
| `Games` | `games` | 两处都应包含目标游戏；当前为 `nte`。 |
| `ApiMajor` | `api.major` | 应完全相同，并与目标 Runtime 的 Plugin API 主版本一致。 |
| `DownloadLinkInstall` | 发布的 ZIP | 必须是可直接下载该版本 ZIP 的 HTTPS URL。 |

`Name`、`Author` 和描述字段用于在线目录展示；安装后以包内 Manifest 为准。修改版本时，至少同时更新 Manifest 的 `version`、索引的 `Version` 和 ZIP URL。

## 4. 托管在线插件源

索引可以放在 GitHub 仓库、GitHub Pages 或任何能直接返回文件内容的 HTTPS 静态站点。以 GitHub 为例，推荐使用下面的分工：

```text
example/anomaly-plugins
  pluginmaster.json       提交到默认分支，URL 保持稳定
  assets/my-plugin.png    可选图标

example/my-plugin
  Releases/v1.2.0
    com.example.my-plugin-1.2.0.zip
```

对应 URL 应使用直接内容或直接下载地址：

| 用途 | URL 示例 |
| --- | --- |
| 插件源 | `https://raw.githubusercontent.com/example/anomaly-plugins/main/pluginmaster.json` |
| 图标 | `https://raw.githubusercontent.com/example/anomaly-plugins/main/assets/my-plugin.png` |
| 插件 ZIP | `https://github.com/example/my-plugin/releases/download/v1.2.0/com.example.my-plugin-1.2.0.zip` |
| 项目主页 | `https://github.com/example/my-plugin` |

不要把 `https://github.com/example/anomaly-plugins/blob/main/pluginmaster.json` 用作插件源；这是返回 HTML 的网页地址，不是原始 JSON。ZIP 也应使用 Release asset 下载地址，不能指向 Release 详情页。

上传后从公网 URL 重新获取索引和 ZIP，确认索引能解析、目标条目存在，且下载结果确实是压缩包：

```powershell
$IndexUrl = 'https://raw.githubusercontent.com/example/anomaly-plugins/main/pluginmaster.json'
$PluginId = 'com.example.my-plugin'

$Entries = @(Invoke-RestMethod -Uri $IndexUrl)
$Entry = $Entries | Where-Object { $_.InternalName -eq $PluginId }
if ($null -eq $Entry) { throw "plugin is missing from the online index: $PluginId" }

Invoke-WebRequest -Uri $Entry.DownloadLinkInstall -OutFile .\dist\online-package.zip
Expand-Archive .\dist\online-package.zip .\dist\online-package -Force
Test-Path .\dist\online-package\manifest.json
```

最后一行必须输出 `True`。再对解压目录运行 `anomaly-plugin validate`，可以同时发现 Manifest 与包结构问题。

## 5. 配置 Runtime 使用在线插件源

用户侧推荐在 **Plugins > 第三方插件** 中点击“添加插件源”，填入 `pluginmaster.json` 的 HTTPS URL 并应用。Runtime 会生成等价的 `Anomaly\plugin-repositories.json`：

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "allowInsecureSources": false,
  "repositories": [
    {
      "url": "https://raw.githubusercontent.com/example/anomaly-plugins/main/pluginmaster.json",
      "enabled": true
    }
  ]
}
```

| 字段 | 含义 |
| --- | --- |
| `schemaVersion` | 固定为 `1`。 |
| `enabled` | 第三方插件功能总开关。为 `false` 时不刷新任何源。 |
| `allowInsecureSources` | 公开分发保持 `false`；只在本地 `file://` 测试时设为 `true`。 |
| `repositories` | 插件源列表，每项包含索引 URL 和独立启用开关。 |
| `repositories[].url` | `pluginmaster.json` 的直接 HTTPS 地址。 |
| `repositories[].enabled` | 是否刷新该插件源。 |

多个在线插件源就在 `repositories` 中添加多个对象。应用后回到 **Plugins > 可用** 刷新；不需要把发布者的 `pluginmaster.json` 复制到 Runtime 目录，也不要把插件源写进 `repository.json`。

## 6. 发布新版本的顺序

一次正常发布按这个顺序走：

1. 更新 Manifest 版本，构建 Release 包。
2. 运行 `validate` 和 `anomaly-test-host`。
3. 生成 ZIP，确认根目录结构。
4. 把 ZIP 上传到一个不会被覆盖的版本地址，例如 GitHub Release asset。
5. 用浏览器重新下载一次，确认链接返回的确实是 ZIP，不是登录页或 HTML 错误页。
6. 最后更新 `pluginmaster.json` 的版本和下载地址。

先发布 ZIP、最后改列表，可以避免用户在文件尚未就绪时看到新版本。

不要复用并覆盖旧版本 URL。不可变的版本链接更容易排错，也能让用户知道自己拿到的是哪一版。

## 7. 本地测试插件源

`file://` 只用于受控的本地测试。先退出游戏，再临时修改 Runtime 目录中的 `Anomaly\plugin-repositories.json`：

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "allowInsecureSources": true,
  "repositories": [
    {
      "url": "file:///C:/work/my-plugin/repository/pluginmaster.json",
      "enabled": true
    }
  ]
}
```

本地 `pluginmaster.json` 里的 `DownloadLinkInstall` 也可以使用 `file:///C:/.../plugin.zip`。重新启动 Runtime 后，在 **Plugins > 可用** 刷新并走一遍安装、启用、更新和卸载。

测试结束后把 `allowInsecureSources` 改回 `false`。公开分发的配置和地址都必须使用 HTTPS。

## 8. 安装器会核对什么

用户点击安装后，Runtime 会：

1. 下载 ZIP，并限制压缩包与解压内容的大小。
2. 拒绝绝对路径、`..` 越界、大小写重复目标等危险 ZIP 条目。
3. 解析根目录的 Manifest。
4. 核对插件 ID、版本、游戏和 Plugin API。
5. 确认入口 DLL 位于包内。
6. 通过 staging 和备份目录替换旧版本；失败时保留原安装。

这些是格式和一致性检查，不是代码安全审计。平面 `pluginmaster.json` 插件源目前没有发布者签名，也不会验证 `package.sha256`。发布者应保护插件源仓库、使用不可变 Release 资源，并公开源码和版本说明，让用户能判断来源。

## 常见发布问题

| 现象 | 先检查 |
| --- | --- |
| 插件没有出现在“可用” | `pluginmaster.json` 是否为数组；四个必填字段是否齐全；ID 和版本格式是否正确。 |
| 整个插件源刷新失败 | 插件源 URL 是否直接返回 JSON，而不是 GitHub `blob` 页面、登录页或 404 HTML；JSON 是否有注释、尾随逗号或错误的字段层级。 |
| 插件显示不兼容 | `Games`、`ApiMajor` 是否与 Manifest 和目标 Runtime 一致。 |
| 下载后安装失败 | `DownloadLinkInstall` 是否直接下载 ZIP；ZIP 是否直接以 `manifest.json` 为根；ID、版本、入口 DLL 是否一致。 |
| 新版本没有出现在“更新” | 新 `Version` 是否真的高于已安装版本；列表和 Manifest 是否同时更新。 |
| 本地 `file://` 被拒绝 | 是否只在测试配置中启用了 `allowInsecureSources`，索引和 ZIP 是否都使用完整文件 URI。 |

## 相关文档

- [插件开发](plugin-development.md)
- [Manifest 与 capability](../api-reference/manifest-and-capabilities.md)
- [第三方插件用户指南](../user-guide/third-party-plugins.md)
- [配置参考](../user-guide/configuration.md)
