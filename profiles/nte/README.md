# NTE Profile

Runtime 只加载最终扩展名为 `.json` 的文件。分层 catalog 按来源优先级（`local override > repository > bundled`）为 `nte` 选择一个活动 Profile；它不读取或匹配 Build identity。所选 Profile 在启动时绝不重新绑定或改写。

Runtime 不对游戏 PE 做 fingerprint 或哈希。Resolver 会先读取单份 RVA cache：符号 ID 存在、模块名相同且 RVA 仍在模块大小内时，就直接复用，不做 Build / Profile / Resolver 身份检查，也不重新验证该 Symbol。其它符号仍按活动 Profile 扫描和校验，然后写回 cache。符号装配之后，Feature validator、依赖与运行时 ABI / thread gate 仍然控制服务发布。
`nte-build-profile.json.example` 因最终扩展名不是 `.json` 而保持未激活。

```powershell
anomaly-profile fingerprint C:\path\to\HTGame.exe nte
anomaly-profile validate .\nte-build-profile.json
```

Bundled Runtime 发布一份活动声明 `nte-current.json`。Runtime 会直接选择该声明，并按上述条件复用 resolver cache 记录。Fingerprint 仅作为离线诊断命令。

当前证据覆盖：

- `nte-win64-5787dc83-10001000-7c6cfbdbe70626a0`：完整的 Phase 10 lifecycle smoke。
- `nte-win64-771eea6e-10008000-8b337d38619b1264`：2026-07-23 更新的实机签名、语义 layout、对象注册表、实体边界、相机与 outbound transform ABI preflight。
- `nte-current.json`（证据采集于 `nte-win64-e63ff9c7-10008000-19bb677e6b863805`）：只读的 player/session/ESP 证据，以及一次经 `anomaly.nte.player-teleport` 的实机成功 teleport 验证。同一 Build 的 `BankBox_Treasure_01_Lv3_C` RobBank 拾取证据只用于 Pink Paw 插件内置合同，不属于 Profile 或宿主 NTE 服务。单次动态验证不将该 Build 提升为 `Supported`。

在保存完整的重启后 lifecycle smoke 之前，活动声明仍低于仓库的 `Supported` 证据级别。证据标签描述“测过什么”，不参与运行时 Profile 选择。Runtime 不会自动使 RVA cache 失效。
