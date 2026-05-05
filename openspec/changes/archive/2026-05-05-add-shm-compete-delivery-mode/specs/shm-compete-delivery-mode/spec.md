## ADDED Requirements

### Requirement: 支持通道级抢占投递模式
系统 SHALL 支持在通道拓扑配置中通过 `delivery_mode=compete` 参数启用抢占模式。默认值为 `broadcast` 以保持完全兼容。`compete` 模式下，一条消息只能被一个消费者成功处理。

#### Scenario: 配置抢占模式通道
- **WHEN** 在模块配置文件中使用 `output some.channel delivery_mode=compete`
- **THEN** 拓扑解析后 ChannelTopologyEntry.params 包含 delivery_mode=compete，且 Resolve 函数返回正确值

#### Scenario: 默认广播模式
- **WHEN** 未指定 delivery_mode 或指定 broadcast
- **THEN** 行为与现有系统完全一致，所有消费者均收到消息

### Requirement: 生产者通知所有在线消费者
在 compete 模式下，PublishWithStatus SHALL 通知所有在线消费者的 event_fd（与 broadcast 相同），竞争在消费端通过 SlotMeta.claimed 原子 CAS 解决。

#### Scenario: compete 模式下生产者通知
- **WHEN** 生产者调用 Publish 且通道配置为 compete，有多个在线消费者
- **THEN** 所有在线消费者的 event_fd 均收到通知，消费者通过 CAS 抢占 claimed 标志决定谁处理消息

#### Scenario: broadcast 模式下生产者通知
- **WHEN** 通道配置为 broadcast
- **THEN** 所有在线消费者的 event_fd 均收到通知（行为不变）

### Requirement: 消费者通过 CAS 竞争消费，独立推进游标
消费者被唤醒后，SHALL 通过原子 CAS 尝试将 `SlotMeta.claimed` 从 0 置为 1。成功者处理消息，失败者跳过 handler。无论是否抢到，每个消费者 SHALL 独立推进自己的 read_sequence。

#### Scenario: 抢占模式下 CAS 竞争
- **WHEN** 多个消费者同时被唤醒，尝试消费同一槽位
- **THEN** 恰好一个消费者的 CAS 成功并执行 handler，其余消费者 CAS 失败并跳过 handler；所有消费者均推进游标

#### Scenario: reclaim 不被阻塞
- **WHEN** 部分消费者未抢到消息但游标已独立推进
- **THEN** MinActiveReadSequence() 正确反映全局最慢消费者进度，ReclaimCommittedSlots() 能正常回收已消费槽位，不发生背压或 slot 耗尽

#### Scenario: 移除 SyncPeerConsumerCursors
- **WHEN** compete 模式消费完成
- **THEN** 不再需要同步其他消费者游标（每个消费者独立推进），SyncPeerConsumerCursors 已移除，消除高吞吐下的并发竞态风险

### Requirement: SlotMeta.claimed 标志与版本升级
SlotMeta SHALL 在现有 4 字节 padding 中增加 `std::atomic<std::uint32_t> claimed` 字段，不改变结构体总大小（维持 24 字节）。`kRingLayoutVersion` SHALL 从 7 升至 8，反映 SlotMeta 布局语义变更。

#### Scenario: claimed 字段布局兼容
- **WHEN** SlotMeta 增加 claimed 字段
- **THEN** sizeof(SlotMeta) 不变，kRingLayoutVersion 升级导致旧 segment attach 快速失败

#### Scenario: 槽位回收重置 claimed
- **WHEN** TryReclaimSlotIfSafe 或 ResetSlot 回收/重置槽位
- **THEN** claimed 被重置为 0，新消息可被重新竞争

### Requirement: 单元测试全面覆盖
所有修改点 MUST 有对应的 GTest 测试用例，覆盖正常路径、边界、并发和回退场景。测试代码必须添加到对应的 *_test.cpp 文件中。

#### Scenario: 模式切换测试
- **WHEN** 分别使用 broadcast 和 compete 配置运行测试
- **THEN** 断言交付次数、游标值、reclaim 计数符合预期

#### Scenario: 多消费者竞争测试
- **WHEN** 多个消费者同时订阅 compete 通道并发布多条消息
- **THEN** 总消费次数等于发布次数，每条消息只被处理一次，游标一致

## MODIFIED Requirements

### Requirement: linux-shm-pubsub-runtime 发布消费流程
**原有**（来自 openspec/specs/linux-shm-pubsub-runtime/spec.md 摘录）：
The system SHALL support broadcast delivery where all online consumers receive every published message via their notification eventfd.

**更新后**：
The system SHALL support both `broadcast` (all consumers notified, each processes every message) and `compete` (all consumers notified, exactly one claims via CAS on SlotMeta.claimed, all advance cursors independently) delivery modes as configured per channel in topology params. The `compete` mode SHALL ensure at-most-once delivery semantics while maintaining correct reclaim and backpressure behavior through independent per-consumer cursor advancement.

#### Scenario: 模式兼容性验证
- **WHEN** 使用现有 broadcast 配置
- **THEN** 行为与修改前完全一致

#### Scenario: 抢占模式集成
- **WHEN** 配置 compete 模式并运行多消费者测试
- **THEN** 系统正确实现 CAS 竞争消费 + 独立游标推进，无回归

该规格定义了新增抢占模式的精确行为，作为实现和验证的契约。
