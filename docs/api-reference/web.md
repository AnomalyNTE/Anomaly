# 本机网页服务

`anomaly.web` v1（capability：`web`）提供 Runtime 所有的本机 HTTP
端点，头文件为 `anomaly/sdk/services/web.h`。它不依赖游戏和 Profile。

服务图中的 `anomaly.internal.web-server` 是可选监听器初始化任务；其 Ready
表示初始化任务已结束，不表示网页端点可用。公开 `anomaly.web` 只在版本化 provider
成功发布后可查询。监听或发布失败会记录具体原因，其他 Runtime 服务继续启动。

插件在 Lifecycle 或 Worker 查询服务并调用 `register_application`，传入当前包内的
相对资源目录（例如 `assets/web`）、回调和用户指针。每个插件 generation 可注册一个
应用。宿主在 Worker 预载资源，要求存在 `index.html`，最多 64 个文件、总计 8 MiB；
拒绝越界目录与 reparse。网页加载阶段可能短暂返回 503。静态文件只从预载快照提供。

`application_url` 使用两段式 NUL 结尾缓冲协议，返回
`http://127.0.0.1:<port>/plugins/<plugin-id>/#token=<session-token>`。
端口由系统分配。令牌仅属于本次注册，不写入配置或日志。网页从 URL fragment 取令牌，
在所有 `/api/` 请求中发送 `X-Anomaly-Token`；宿主同时检查 Host 和存在时的 Origin。
静态资源不需要令牌，API 不开放 CORS。

回调收到应用内路径（如 `/api/state`）、GET/POST 方法与最多 64 KiB 的请求体。
调用通过 Runtime Worker Dispatcher 执行，持有 PluginScope generation lease，
不得直接访问 Game/Render 对象。游戏操作应入队，在 Game update 执行；GET 应读取已发布快照。

每次请求只调用回调一次。宿主提供 1 MiB 可写响应缓冲，插件填写 `body_size`、
`http_status`（200..599）与 JSON/TEXT 内容类型。插件不得替换缓冲指针或保留请求/响应引用。
RPC/业务成功由插件协议定义；HTTP 200 本身不证明游戏操作已生效。

`unregister_application` 仅在 Lifecycle/Worker 调用，关闭入口并排空已进入的回调。
在该应用自己的 HTTP 回调内注销返回 `CONFLICT`，应向 Lifecycle 投递停止意图。
宿主停用/热重载会通过 Scope ledger 自动撤销遗漏资源，旧 generation 不再接收请求。
请求等待超时返回 504；已开始的回调可能已产生业务效果，调用方不能据此自动重发。
