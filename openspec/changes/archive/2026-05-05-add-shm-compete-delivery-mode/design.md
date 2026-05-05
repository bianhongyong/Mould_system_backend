## Context

当前 SHM Bus（`ShmBusRuntime` + `RingLayoutView`）的核心是基于共享内存环形缓冲区的 pub/sub 实现：

- **广播模式**：`PublishWithStatus()` 遍历所有在线 `ConsumerCursor`，向每个消费者的 `event_fd` 写入通知。
- **消费者处理**：UnifiedSubscriberPump 通过 epoll 唤醒后，调用 `TryDispatchOneRingMessage()`，使用 `TryReadNextForConsumer` + `CompleteConsumerSlot` 推进**单个消费者**的 `read_sequence`。
- **Reclaim**：`MinActiveReadSequence()` 取所有在线消费者游标的最小值，决定哪些 committed slot 可以回收。
- **现有约束**：`consumer_capacity` 由 `ResolveShmRingConsumerCapacity` 决定（支持多槽位抢占），但当前所有消费者游标独立推进，**没有组级“已消费”概念**。

本变更引入**抢占投递模式（compete）**，解决”生产者消息只能被消费一次”的需求，同时保持现有广播模式完全兼容。提案中已明确最小改动原则：不重构 RingLayoutView 核心回收协议。

参考 proposal.md 和现有 specs（如 `linux-shm-pubsub-runtime`、`multi-consumer-cursor-reclaim`）。

## Goals / Non-Goals

**Goals:**

- 支持通道级 `delivery_mode=compete` 参数（默认 `broadcast`）
- 生产者通知所有在线消费者，消费者通过原子 CAS 竞争槽级 `claimed` 标志来决定谁处理消息，实现”抢占消费”
- 每个消费者独立推进自己的游标（无论是否抢到），确保 `MinActiveReadSequence()` 和 reclaim 逻辑正确
- 移除 `SyncPeerConsumerCursors`，消除高吞吐场景下的游标并发安全风险
- 所有改动单元测试覆盖（使用 GTest，添加到对应 *_test.cpp）
- broadcast 模式不受影响
- 最小代码改动，优先修改 runtime 层

**Non-Goals:**

- 不实现复杂的负载均衡策略（不再需要轮询选择消费者）
- 不改变 SHM 总布局大小（`claimed` 利用已有 padding，SlotMeta 保持 24 字节）
- 不修改 supervisor 消费槽分配机制
- 不引入新的 protobuf 消息格式或 API 变更

## Decisions

### 1. 配置方式：使用通道拓扑参数 `delivery_mode`

**选择**：在 `ChannelTopologyEntry.params` 中添加 `delivery_mode=broadcast|compete`。
**理由**：与现有 `shm_consumer_slots`、`slot_payload_bytes` 一致，利用 `Resolve`* 函数扩展即可。无需新配置文件格式。
**备选**：在 `SubscriberEntry` 运行时注册时指定（更灵活但配置分散，不符合拓扑驱动原则）。
**影响**：需更新 `channel_topology_config.cpp` 的解析/Resolve 函数（最小改动）。

### 2. 生产者通知策略

**选择**：在 `ShmBusRuntime::PublishWithStatus()` 中，broadcast 和 compete 统一通知所有在线消费者的 event_fd。竞争在消费端解决。

**理由**：
- 简化发布路径：不再需要区分模式、维护 round-robin ticket、处理 notify 失败回退
- 消除旧方案 `SyncPeerConsumerCursors` 的竞态风险：旧方案中发布端只通知一个消费者，该消费者处理完后需要强行推进其余消费者的游标，导致高吞吐下的并发安全问题和漏消息/重复消费
- 新方案中所有消费者同时被唤醒，通过 `SlotMeta.claimed` 的原子 CAS 来决定谁处理消息，未抢到的消费者仅推进游标（不重复处理）

**备选**：旧方案的生产者轮询 + SyncPeerConsumerCursors（已被验证存在并发安全风险）。
**风险**：通知所有消费者引入了不必要的 eventfd 写入（在消费者数量多时）。但在典型场景中消费者数量很少（2-4 个），且系统已经使用 eventfd 的 epoll edge-triggered 模式，开销可忽略。

### 3. 消费者竞争处理与游标推进（核心解决点）

**选择**：在 `SlotMeta` 中增加 `std::atomic<std::uint32_t> claimed` 标志（利用已有的 4 字节 padding）。`TryDispatchOneRingMessage()` 读取消息后：

- 如果 `broadcast`：所有消费者正常处理消息（与现有逻辑一致）
- 如果 `compete`：调用 `ring.TryClaimSlot(slot_index)`（CAS claimed 0→1）
  - 成功：该消费者”抢到”消息，执行 handler
  - 失败：跳过 handler（其他消费者已抢到）
- **无论是否抢到，都推进自己的游标**（`CompleteConsumerSlot`）
- 移除 `SyncPeerConsumerCursors`

**理由**：
- 彻底消除”一个消费者推进其他消费者游标”的并发竞态——每个消费者只推进自己的游标
- 每个消费者独立推进，`MinActiveReadSequence()` 自然反映全局最慢消费者进度，不会导致 reclaim 阻塞
- 原子 CAS 确保 “at-most-once” 交付语义

**备选**：旧方案 SyncPeerConsumerCursors ——已被验证在高吞吐下存在消费者游标被另一个消费者并发推进导致漏消息/重复消费的严重并发问题。

**实现位置**：
- `common/comm/include/shm_ring_buffer.hpp`：SlotMeta 增加 `claimed` 字段，`kRingLayoutVersion` 升至 8
- `common/comm/src/shm_ring_buffer.cpp`：新增 `TryClaimSlot()`，`TryReclaimSlotIfSafe()` 和 `ResetSlot()` 中重置 `claimed`，`AdvanceConsumerCursor` 拒绝回归
- `common/comm/src/shm_bus_runtime.cpp`：`TryDispatchOneRingMessage` 增加竞争逻辑，移除 `SyncPeerConsumerCursors`

### 4. DeliveryMode 枚举与存储

**选择**：在 `ShmBusRuntime` 中新增 `enum class DeliveryMode { kBroadcast, kCompete };`，在 `ChannelRuntime` 或 per-channel map 中缓存（从 topology.params 解析）。
**理由**：运行时高效查询，避免每次 publish 都查 topology。
**位置**：`shm_bus_runtime.hpp` 增加枚举和成员。

### 5. 测试策略

**选择**：所有改动都在现有测试文件扩展：

- `multi_consumer_cursor_reclaim_test.cpp`：新增 TestCompeteModeCursorSync、TestCompeteModeReclaimCorrectness 等 GTest。
- `split_shm_bus_control_runtime_test.cpp`：新增集成测试验证两种模式下交付次数和游标一致性。
- 每个测试包含：setup topology with delivery_mode、multiple subscribers、publish loop、assert delivered count / cursor values / no backpressure。
**理由**：符合 workspace rule “所有单元测试统一利用Gtest框架写”，且“只要相关的改动都添加测试，测试代码添加到改动代码部分对应的test.cpp里”。最后一个 task 必须详尽描述这些测试。

## Risks / Trade-offs

- **[Risk] 选中消费者崩溃** → Mitigation: 生产者下次 publish 会选择其他在线消费者，消息不会永久丢失（现有 reclaim 机制保护）。
- **[Risk] 游标同步开销** → Mitigation: 只在 compete 模式且 dispatch 成功时执行，broadcast 路径无影响；扫描消费者数量通常很少（<10）。
- **[Risk] 并发竞争（多生产者+多消费者）** → Mitigation: 现有 seq + CAS 协议 + 统一游标推进可保证 at-most-once；新增测试覆盖。
- **[Risk] 拓扑参数解析失败** → Mitigation: 默认 broadcast，添加 validation。
- Trade-off: 通知所有消费者 vs 只通知一个（当前选择通知所有，简化发布路径、消除并发风险）。

## Migration Plan

1. 更新 launch_plan.json / 模块配置文件，添加 `delivery_mode=compete` 到需要抢占的 output 通道。
2. 重新 ProvisionChannelTopology（control plane 会重建 ring）。
3. 现有广播通道无需修改。
4. 部署后观察 metrics（新增 compete 相关计数器可选）。
5. Rollback：移除参数或改回 broadcast，重启模块。

## Open Questions

- 是否需要在 topology 中增加 `compete_consumer_preference` 参数支持优先级？
- 长期是否需要 producer-side “sticky consumer” 机制？
- compete 模式下 dedup 窗口如何共享（当前 per-consumer，可能需调整）？

设计符合“最小改动”原则，重点在 runtime 层扩展，单元测试将全面覆盖。