# DX12 绑定

头文件：`anomaly/sdk/services/dx12.h`。服务 `anomaly.dx12.binding` v1，capability `dx12-binding`。

用于已有独立 DX12 渲染器的插件在晚加载时取得当前交换链及其对应的 DIRECT 命令队列。
它不共享宿主 ImGui 上下文，也不替插件安装 Hook、绘制或管理输入。

```json
"capabilities": ["dx12-binding"],
"services": [{"id": "anomaly.dx12.binding", "minVersion": 1, "optional": true}]
```

通过 `query_service` 获取 `AnomalyDx12BindingServiceV1`，检查 `struct_size`、
`service_version` 和 `acquire`。只允许在 Lifecycle / Worker 调用 `acquire(user, &swap, &queue)`。
Render / Game 或未登记线程调用返回 `UNAVAILABLE`；未授权查询返回 `PERMISSION_DENIED`。

- `OK`：输出分别为 `IDXGISwapChain*`、`ID3D12CommandQueue*`，各持有一份 COM 引用。
  调用方必须分别 `Release`，包括自身初始化失败的路径。没有要归还给宿主的回调或 handle。
- `UNAVAILABLE`：尚无经创建交换链或成功 `ResizeBuffers1` 确认的配对、D3D11、正在 resize、
  设备丢失或停止。宿主自身的最近提交队列推测不会通过此服务暴露。
- 输出地址为空或相同返回 `INVALID_ARGUMENT`；所有失败路径清空有效输出地址。

服务表属于当前插件 generation，不得在卸载后使用。取得的 COM 引用只保证对象存活，
不是设备有效性、窗口或渲染期的租约：插件须自行处理 Present / resize / device-loss，
不要跨 `ResizeBuffers` 保留 back buffer。插件停止时须先停止并排空独立渲染器，再释放引用。
宿主撤销当前配对不回收已交付给调用方的引用；设备或交换链变化后需重新获取。

旧宿主可能没有此服务；插件可声明可选依赖并报告不可用。服务表可查询不代表图形已就绪，
不得在 Render 回调中等待初始化。框架若在交换链创建后才注入，且未观察到后续有效的
`ResizeBuffers1`，该服务可以持续返回 `UNAVAILABLE`，不会猜测配对。
