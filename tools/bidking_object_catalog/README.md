# BidKing Object Catalog

这个诊断包在有界的 game-thread 批次中枚举已验证的 UE5 对象注册表。它记录名称含 `BidKing` 的对象，并把 JSON 快照写入包状态目录，命名为 `bidking_objects.json`。

该包不安装 Hook、不请求裸内存访问，也不修改游戏。每个命中项包含其 handle 中编码的 UE 对象注册表 index 与 serial。这些字段可通过只读 Runtime pipe 解析，用于检查对应的 `UFunction`、class 与 outer 对象。它刻意排除独立的持久化 Auction 系统。

UE5 服务是可选的，因此 `anomaly-test-host` 可以在没有游戏进程时验证该包。运行时它会从 `on_update` 重试服务发现，因为 feature-gated 的 UE5 表可能在插件激活之后才出现。
