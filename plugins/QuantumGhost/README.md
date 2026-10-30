# 瞬秒-量子幽灵计划（Quantum Ghost）

面壁者计划量子幽灵：攻击命中后直接清空血条，让目标在观察者眼里已经死去，但决定生死的观测权不在客户端，所以它只是看起来死了。目标因此停在非死非活的量子叠加态。

- 插件 id `anomaly.local.quantum-ghost`，版本 `7.3.0`，`audience: developer`。
- 不修改宿主框架，不新增宿主服务。
- 依赖服务：`anomaly.ui`、`anomaly.core`、`anomaly.interop.signature`、`anomaly.interop.hook`、`anomaly.nte.combat`（后四个可选）。

## 实现

```
RVA 0x7CEEBE8   movss xmm1, [r14+0x10]   ; r14 = FHTDamageEvent*，挂钩点
RVA 0x7CEEBEE   lea   r9,  [r14+0x148]
RVA 0x7CEEDAA   call  CharacterOnDamaged
```

定位顺序：解析 `CharacterOnDamaged`（特征扫描，失败退回 RVA `0x7CB1710`）→ 扫 `.text` 找它的调用点 → 在调用点前 `0x400` 字节内匹配特征 `F3 41 0F 10 4E 10 4D 8D 8E 48 01 00 00 48 89 6C 24 30`。挂钩由 `interop.hook` 创建，停止时释放。

`scale_stub.asm`：

1. 把 `[r14+0x10]` 乘倍率写回，通知与飘字读到放大值；
2. 记录 `(倍率-1) × 原始伤害` 与受击角色指针，供下一帧补扣。

补扣在 `on_update` 执行：`ASC = [victim+0x940]`，写 `[ASC+0x1A38]`（`HPCurrent`）并同步 `[ASC+0x1A40]`。伤害调用栈内直接写会被结算覆盖，故推迟一帧。

补扣后进入压制：`HoldHp()` 每帧只往下压回目标值，持续 60 秒，血量归零即结束。

Stub 不动栈、不改 `rcx`/`rdx`/`r8`/`r9`，无需 `UNWIND_INFO`。

## 设置

界面只有一个总开关「启动量子幽灵计划」和上面的描述。倍率固定 400，同时写基础血量（`1A40`），压制 60 秒，均不可调整。

## 日志

```
quantum-ghost: 目标 0x…（…） 调用点=1
quantum-ghost: 结算取值点 0x…（RVA 0x7CEEBE8）
quantum-ghost: 挂钩 code=0 original=0x… 目标=0x…
quantum-ghost: 命中#1 本次 2789.00->8367.00 目标HP=387124.0 补扣=1 目标=0x…
quantum-ghost: 调用=1 放大=1 倍率=3.00 上次 2789.00->8367.00 伤害事件=1 最近=8367
quantum-ghost: 属性窗口 命中前 1A00=… 1A38=… 1A3C=… 1A40=…
quantum-ghost: 压制中 帧=30 hp=675954.4 目标=675954.4 重写=1
```

`属性窗口` 为 `ASC+0x1A00` 起 24 个 float：`1A38` 当前血量，`1A3C`/`1A40` 为 Max/Base。`压制中` 的 `重写` 恒为 1 表示窗口内游戏未回写。

## 构建与部署

```powershell
cmake -S plugins/AnomalyDamageChain -B "$env:TEMP\dmg-recall" -G "Visual Studio 17 2022" -A x64 `
      -DAnomalySDK_DIR="<SDK>/lib/cmake/AnomalySDK"
cmake --build "$env:TEMP\dmg-recall" --config RelWithDebInfo
```

产物 `package/AnomalyDamageChain/`（`plugin.dll` + `manifest.json`）复制到 `<运行时>/Anomaly/plugins/AnomalyDamageChain/`。热加载签名是「相对路径 + 大小 + 修改时间」，`Copy-Item` 保留源时间戳，字节相同的复制不会触发重载。

## 已知限制

- 血量修改只作用于客户端，服务端仍按自身结算判定击杀与掉落。
- 血量是复制属性，压制窗口结束后会被权威值覆盖。

## 文件

| 文件 | 作用 |
| --- | --- |
| `plugin.cpp` | 生命周期、定位与挂钩、补扣与压制、面板、诊断 |
| `scale_stub.asm` | 挂钩点 MASM 探针（放大 + 记录待补扣） |
| `manifest.json` | 服务与能力声明 |
| `CMakeLists.txt` | 独立构建（只面向已安装的运行时 SDK） |
