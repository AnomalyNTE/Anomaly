# 更好的地图

在游戏大地图显示谕石和「21 的赠礼」，点位与图标直接读取游戏数据。默认显示两类的全部点位。勾选「隐藏已完成」后会按当前账号的收集进度过滤，两类显示开关仍可分别调整。

## 使用

在 Anomaly 插件管理中启用「更好的地图」，进入主场景后打开游戏大地图。标记会分批登记，使用原生图标资源，显示范围由插件单独设置。勾选「隐藏已完成」时，插件会定期读取进度，暂时无法确认的点位继续显示。停用或卸载前可先关闭已开启的显示开关，清除当前场景中的标记。

勾选「显示敌影清剿」可查看敌人图鉴中的主世界固定刷新点。标记使用图鉴提供的头像和名称。同一位置可能有多类敌人。「隐藏已完成」只筛选收集物。

点击「筛选怪物」展开双列名单，可按名称搜索或翻页。取消勾选会隐藏该怪物的全部点位，重新勾选即可显示。「全选」和「全不选」对所有怪物生效。显示开关和筛选会自动保存。

勾选「显示城市违规行径」，可在大地图上用游戏原生的手铐图标查看可能发生违规事件的位置，包括「市民受到威胁」。事件是否正在发生，以游戏中的附近提示为准。

「启用地图点击传送」默认关闭。开启后，在主场景大地图上按住 Ctrl 并点击鼠标左键即可传送。也可以点击「选择传送位置」，再用鼠标左键点击目标。底图和图标都可点击。目标地面尚未加载时，插件会短暂加载目标区域，确认能落脚后再传送。加载过程中可点击「取消地图传送」。

点击「传送快捷键」可额外绑定一个键。把鼠标移到大地图目标上，再按该键即可传送。设置后，按钮显示自定义键，绑定会自动保存。设置时按 Esc 取消，按退格键恢复默认显示。

开启地图点击传送期间，游戏大地图点击时的「创建图钉」弹窗会被屏蔽。关闭开关后即可照常创建图钉。

## 分类导出点位

进入主场景后点击「分类导出全部点位」。导出覆盖谕石、21 的赠礼、怪物图鉴固定刷新点和城市违规行径，
不受显示开关、怪物筛选或「隐藏已完成」影响。数据来自现有的静态数据表读取逻辑，不扫描附近实体，
不导出账号收集进度。游戏线程复制坐标和标识，Worker 负责序列化与原子写文件。

文件直接写入运行包的 `Anomaly/state/plugins/b1ank.better-map/`，首次导出无需手动创建子目录：

```text
b1ank.better-map/
  index.json              # 本次文件清单及各类数据表是否可用
  oracle-stone.json        # 谕石
  gift-21.json             # 21 的赠礼
  city-crime.json          # 城市违规行径
  monster-<稳定标识>.json  # 每种怪物一个文件，名称与图鉴行名见文件内部
```

`index.json` 仅保存清单，不聚合点位。每个分类文件使用 `schemaVersion: 1`、
`source: "b1ank.better-map"`、`coordinateSystem: "ue-world-centimeters"`，包含
`kind`、`groupId`、`name` 和 `points`。每条点位保留双精度 `x/y/z`、稳定的游戏行名标识、
`level`、区域与楼层信息。怪物身份由图鉴行名和坐标构成，数组重排不改变身份；同一怪物的重复坐标只导出一次。
城市事件的坐标与插件地图图标一致，包含游戏配置的图标偏移（`positionKind: "map-icon"`），
表示事件可能发生的位置，不能用来判断事件正在发生。

部分数据表不可用时仍可导出其他分类，界面会提示，清单的 `datasets` 对应项为 `false`。
没有任何数据可导出时不写文件。重复导出直接覆盖当前分类文件；导入必须以当前 `index.json` 为准，
不要枚举目录中旧的分类文件。当前路径不包含宝箱的其他种类、电话亭、打卡和随机刷新资源。

在 MaaNTE-Map 仓库根目录执行：

```powershell
npm run import:bettermap -- "<运行包>/Anomaly/state/plugins/b1ank.better-map" --dry-run
npm run import:bettermap -- "<运行包>/Anomaly/state/plugins/b1ank.better-map"
npm run merge:locations -- output/imports/bettermap --dry-run
npm run merge:locations -- output/imports/bettermap
```

也可以只传入某个分类 JSON。转换结果按地图分类分文件，保留已有点位的 ID、名称、截图和备注。
首次按同类点位的邻近坐标匹配，默认阈值为 1700 厘米；多候选或多条源数据争用同一旧点位时跳过并报告，
可通过 `--match-distance` 调整。后续优先按记录的 `betterMapId` 匹配，默认模式不删除导出中缺失的旧点位。

需要完全以本次游戏数据为准时，转换命令加 `--authoritative`。该模式按最近的一一对应关系复用旧记录，
全部采用游戏坐标，并清理已导出分类中没有对应源点位的旧记录；未导出的分类保持原样。

## 构建

在 Anomaly 仓库根目录执行以下命令。

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --target anomaly_builtin_better_map
```

产物位于 `.build/windows-vs2022/bin/RelWithDebInfo/builtin-plugins/BetterMap`。
