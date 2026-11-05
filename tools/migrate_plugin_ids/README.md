# 插件配置迁移 / Plugin config migration

插件 id 改版了。双击 `Migrate-Plugin-Ids.cmd`，把旧的启用状态和配置搬到新版本上，不用一个个重新设置。
Plugin ids were changed. Double-click `Migrate-Plugin-Ids.cmd` to move your existing enablement state and settings onto the new ids.

[中文](#中文) · [English](#english)

---

## 中文

### 怎么放

把这三个文件放到**与 `AnomalyLauncher.exe` 同一个目录**：

```
AnomalyLauncher.exe
Migrate-Plugin-Ids.cmd      ← 双击这个
migrate_plugin_ids.ps1
README.md
Anomaly\                    ← 你的配置和状态就在这里面
```

### 怎么用

1. **先退出游戏。**
2. 双击 `Migrate-Plugin-Ids.cmd`。
3. 窗口会先显示一份**预览**，不会改动任何文件。
4. 看完预览，输入 `Y` 回车，开始迁移。
5. 看到“完成”就可以关窗口，启动游戏。

重复双击是安全的：已经迁过的会自动跳过。

迁移的内容：插件启用开关、插件设置、插件状态数据，以及界面里的插件收藏和窗口位置。

### 输出怎么看

| 看到 | 意思 |
| --- | --- |
| `rename` / `move` | 正常迁移，不用管 |
| `merge` | 新旧两边都有数据，已经合到一起 |
| `conflict` | 新 id 下你已经设置过，保留你现在的设置 |
| `kept` | 旧目录里还有和新目录同名的文件，脚本不删，留着给你看 |
| `removed` | 这个插件已经被移除，数据没动 |
| `unknown` | 不认识的第三方插件，数据没动 |

汇总里 `conflict`、`kept`、`removed`、`unknown` 大于 0 不会损坏任何东西，脚本只是不替你决定删不删。想清理的话对着上面几行的名字手工删即可。

### 出问题怎么办

- **报“找不到 config/ 或 state/”**：三个文件没放在 `AnomalyLauncher.exe` 旁边。放对位置再双击。
- **报“找不到 PowerShell”**：系统异常，正常 Windows 10/11 自带 PowerShell。
- **迁移后插件设置还是空的**：确认双击前游戏已经完全退出。
- **脚本不会自动备份**：重要数据请先自己复制一份 `Anomaly\config` 和 `Anomaly\state`。

### 命令行用法（进阶）

需要脚本化或只跑预览时：

```powershell
# 只看计划，不改文件
pwsh -NoProfile -File migrate_plugin_ids.ps1 -WhatIf

# 直接执行
pwsh -NoProfile -File migrate_plugin_ids.ps1

# 指定别的目录（默认用脚本所在目录）
pwsh -NoProfile -File migrate_plugin_ids.ps1 -Root "D:\Games\NTE\Anomaly"
```

Windows 自带的 PowerShell 5.1 和 PowerShell 7 都能跑。

---

## English

### Where to put it

Put these three files **in the same folder as `AnomalyLauncher.exe`**:

```
AnomalyLauncher.exe
Migrate-Plugin-Ids.cmd      ← double-click this
migrate_plugin_ids.ps1
README.md
Anomaly\                    ← your config and state live in here
```

### How to use it

1. **Close the game first.**
2. Double-click `Migrate-Plugin-Ids.cmd`.
3. The window first prints a **preview**; nothing is written yet.
4. Type `Y` and press Enter to apply.
5. When it says it is done, close the window and start the game.

Double-clicking again is safe — already-migrated data is skipped.

What gets migrated: per-plugin enable state, plugin settings, plugin state data, plus your plugin favourites and window
positions from the UI.

### Reading the output

| You see | Meaning |
| --- | --- |
| `rename` / `move` | normal migration, nothing to do |
| `merge` | both old and new had data, it was combined |
| `conflict` | you already configured the new id, your current settings win |
| `kept` | the old folder still holds same-named files; the script will not delete them |
| `removed` | that plugin no longer exists, its data was left alone |
| `unknown` | an unrecognised third-party plugin, its data was left alone |

If `conflict`, `kept`, `removed` or `unknown` are above zero nothing is broken — the script simply will not decide for you what to delete. Clean up by hand using the names printed above.

### If something goes wrong

- **“no config/ or state/ found”**: the three files are not next to `AnomalyLauncher.exe`. Move them and double-click again.
- **“PowerShell was not found”**: unusual — Windows 10/11 ships PowerShell by default.
- **Settings still empty after migrating**: make sure the game was fully closed before double-clicking.
- **There is no automatic backup.** Copy `Anomaly\config` and `Anomaly\state` first if the data matters.

### Command line (advanced)

For scripting or preview-only runs:

```powershell
# preview, nothing is written
pwsh -NoProfile -File migrate_plugin_ids.ps1 -WhatIf

# apply
pwsh -NoProfile -File migrate_plugin_ids.ps1

# target another folder (defaults to the script's own folder)
pwsh -NoProfile -File migrate_plugin_ids.ps1 -Root "D:\Games\NTE\Anomaly"
```

Both the built-in Windows PowerShell 5.1 and PowerShell 7 work.
