# Custom UID

`Custom UID 1.1.4` 只修改 NTE 左下角 UID 的本地显示文本，不修改账号数据、网络请求或
服务器状态。

## 使用

插件窗口提供以下控件：

- `Display UID`：直接输入 1 到 256 位数字。
- `Apply`：启用覆盖、立即应用并原子保存配置。
- `Revert to original`：使用自动保存的原始 UID 恢复显示，并关闭覆盖。

不需要输入源 UID。插件第一次看到原生 UID 文本时会自动识别并保存；切换账号后看到新的
原生 UID 时会更新保存值。配置 schema ID 为 `fake-uid-settings-v2`：

```json
{
  "enabled": true,
  "displayUid": "000000000000",
  "detectedUid": "216065736008"
}
```

`detectedUid` 是可选的内部缓存，不在编辑器中手工填写。它使热重载后的 Current UID 和
`Revert to original` 不再依赖再次打开原生 UID 界面。

## ESC 与界面生命周期

ESC 不是 UID 刷新按钮。某些小游戏或子界面没有创建 `/Game/UI/Blueprints/Common/BPUI_RoleID`
这一层，因此其中不存在可修改的左下角原生 UID 控件。ESC 返回父级 NTE 界面后，该层才创建或
恢复可见，插件随后自动发现 `TextBlock_RoleID` 并应用配置。

插件启用后不要求 Reload、再次点 Apply 或按 ESC。控件不存在时等待；控件创建后自动应用；
同一控件被游戏写回真实文本时，最多约半秒自动覆盖；退出登录或传送销毁控件后，失效句柄会被
丢弃并重新发现新实例。

## 实现边界

宿主提供通用的签名扫描、调度、UE 对象快照和名称解析服务。FakeUID 专用的函数签名、
`TextBlock` 布局以及临时解析单个对象槽所需的兼容常量由插件自行维护，不再读取或要求修改
`profiles/nte/nte-current.json`。这些数据只在已记录的游戏版本上验证过；签名不唯一、布局越界或
目标 vtable 不匹配时，插件拒绝写入并降级为 unavailable。

运行时方案不安装 Hook：

1. 通过 `anomaly.ue5.objects` 和 `anomaly.ue5.names` 寻找 `TextBlock_RoleID`；Reload 和应用时
   扫描完整对象表，周期检查只扫描最近对象窗口。
2. 只缓存带 registry generation 和 serial 的 opaque handle，不长期缓存 UObject 指针。
3. 每次读取或写入前用 `snapshot_by_handle` 验证句柄，再从单个 GObjects slot 临时解析对象。
4. 在游戏线程读取当前 FText，只在数字段与目标不同的时候调用 `UTextBlock::SetText`。
5. 按对象 serial 保留最近创建的 UID 控件，每 30 次 Update 核对已缓存文本，并周期性轮扫最近
   对象窗口以发现未增加对象总数的复用槽位。

插件不会 Hook 全局 `FText::FromString`、`UTextBlock::SetText` 或水印 SDK，也不会扫描全部对象后
保存裸指针。字符串由 UE 自己的 FString 分配路径创建，因此显示值可为任意 1 到 256 位数字。

## 构建与部署

```powershell
cmake --preset windows-vs2022
cmake --build .build\windows-vs2022 --config RelWithDebInfo `
  --target anomaly_builtin_fake_uid --parallel 1

.build\windows-vs2022\bin\RelWithDebInfo\anomaly-plugin.exe validate `
  .build\windows-vs2022\bin\RelWithDebInfo\builtin-plugins\FakeUID
```

包输出位于：

```text
.build/windows-vs2022/bin/RelWithDebInfo/builtin-plugins/FakeUID
```

部署目录为：

```text
<game>/HT/Binaries/Win64/Anomaly/plugins/FakeUID
```

直接文本输入使用唯一的 `anomaly.ui` V1 合同。

## 已撤回实现

早期原型 Hook 过全局 `FText::FromString`，并把非 UE 分配的缓冲交给 UE，导致
`FMallocBinned2 Attempt to realloc an unrecognized block`。ESC、退出登录和传送只是较容易暴露
堆损坏的时机，不是根因。当前版本没有这条 Hook 和跨分配器所有权路径。

## 实时验证

实时内存分析只能通过 `anomaly-cli`。最低检查项：

- 插件状态为 `active`，Hook 资源数为 `0`；
- 左下角显示值与 `Display UID` 一致；
- Reload 后 `Current UID` 从持久化缓存恢复；
- `Revert to original` 恢复自动保存的原始 UID；
- 退出登录、重新登录、ESC 和传送不产生新崩溃记录。
