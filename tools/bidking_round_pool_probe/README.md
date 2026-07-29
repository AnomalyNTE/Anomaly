# BidKing Round Pool Probe

这个由签名驱动的诊断包 Hook 了多人 BidKing 活动真实的 `ClientBidKingRPC` 响应分发器。它记录每一个有效的 BidKing 响应，使得在一轮切换之后才 attach 的会话仍能捕获实时活动状态。

在原始响应 handler 返回之后，该包读取有界的 `HTOpAct_BidKing + 0xDB0` 玩家记录数组，以及每条已验证玩家记录的 `+0x30` round-cache 数组。这些是诊断缓存，而非声称完整的物品池或物品价值来源。一个 player 快照最多支持 4096 条记录，嵌套 cache 快照独立设界。JSON 会标记被截断的数据并保留逐记录读取失败，因此零填充字节绝不会被当作真实 BidKing 数据上报。

对于已验证 BidKing 分发器接受的每一种响应类型，该包还在原始 handler 运行前最多复制 4 KiB 的信封载荷。这仅把 match-start 与 terminal/result 流量作为原始发现证据保存：它记录声明大小、已复制字节、截断与读取失败，而不指派字段 layout，也不声称该载荷是完整的池。

该包还为 `HTUI_BidKingTurnWidget::StartLotteryWithTarget` 提供第二个只读观察者。在委托给原始 UI 方法之前，观察者把 `+0x680` 处有界的 `ItemListArray` 以 `int64` 值快照下来。这被明确报告为 UI carousel 状态：其内容与数量必须与实时对局关联后才能视为完整池，且不含任何推断的物品估值。

该包通过 `anomaly.ue5.build` 记录活动 Profile 哈希，然后在安装 Hook 前解析其插件自有的唯一签名。它不比较 Build ID。BidKing 专用符号刻意不纳入 Core NTE Profile feature matrix。输出包含所选 Profile 哈希。捕获的响应历史写入 `bidking_round_pool.json`；当前生命周期状态单独写入 `bidking_round_pool_status.json`，使得空的热重载 generation 不能覆盖上一场对局的捕获。

该包不使用、检查或推断独立持久化 Auction 系统的数据。它不写入或 patch 游戏内存。裸记录字节与小端 `u32` 视图是刻意的发现证据，直至每个 BidKing 记录字段都有已验证的语义名称。
