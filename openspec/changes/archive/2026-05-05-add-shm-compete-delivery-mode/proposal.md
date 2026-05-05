## Why

当前 SHM Bus 实现的消息投递采用广播模式（broadcast），即生产者发布一条消息后，所有在线消费者都会通过 event_fd 被唤醒并必须收到该消息。这在任务抢占、负载均衡、单一处理者场景下会导致不必要的重复计算、资源浪费和潜在的竞态问题。

我们希望新增一种**抢占模式（compete/preempt）**：生产者发布消息后，只有一个消费者能成功消费该消息，其他消费者不会重复处理。该模式能显著优化特定业务场景下的性能和资源使用，同时保持现有广播模式的完全兼容。

## What Changes

- 在通道拓扑配置中新增 `delivery_mode` 参数（`broadcast` 默认值，新增 `compete` 值）
- 在 `SlotMeta` 中新增 `claimed` 标志位（利用已有的 4 字节 padding，不增加结构体大小），作为 compete 模式下消息是否已被抢占的原子标志
- 生产者 `PublishWithStatus()` 对 broadcast 和 compete 一视同仁，通知所有在线消费者的 event_fd（简化发布逻辑，竞争在消费端解决）
- 消费者 `TryDispatchOneRingMessage()` 在 compete 模式下通过原子 CAS 抢占 `claimed` 标志：抢到者处理消息，未抢到者跳过处理；无论抢到与否，**每个消费者都独立推进自己的游标**
- 移除 `SyncPeerConsumerCursors`（不再需要强行同步其他消费者游标）
- `AdvanceConsumerCursor` 变更为拒绝回归（原允许回归是为 `SyncPeerConsumerCursors` 设计的）
- `kRingLayoutVersion` 从 7 升至 8（反映 SlotMeta 布局语义变更）
- 最小化改动：不改变 SHM 总布局大小、不修改 RingLayoutView 核心 reclaim 协议，仅在 runtime 层和 SlotMeta 增加竞争逻辑
- 在相关测试文件中添加详尽的 GTest 单元测试（覆盖竞争语义、总交付次数正确性、reclaim 正确性、背压场景、多进程消费者竞争）

**BREAKING**: kRingLayoutVersion 升级，旧 segment 在新代码下 attach 会因版本不匹配而快速失败（需要重新创建）。broadcast 模式的行为完全向后兼容。

## Capabilities

### New Capabilities

- `shm-compete-delivery-mode`: 引入 SHM Bus 抢占投递模式，支持生产者消息被单一消费者竞争消费，包含配置参数、发布策略、槽级原子抢占标志、原子 CAS 竞争逻辑及完整测试覆盖

### Modified Capabilities

- `linux-shm-pubsub-runtime`: 扩展发布/消费流程以支持 `delivery_mode`，修改 `PublishWithStatus` 和 `TryDispatchOneRingMessage` 逻辑
- `multi-consumer-cursor-reclaim`: 确保抢占模式下多消费者游标同步后，`MinActiveReadSequence` 和 reclaim 逻辑仍正确工作
- `channel-topology-config-parser`: 支持解析 `delivery_mode` 参数（如果需要扩展拓扑验证）

## Impact

- **核心文件**：
  - `common/comm/include/shm_ring_buffer.hpp`（SlotMeta 增加 claimed 字段、RingLayoutView 增加 TryClaimSlot、kRingLayoutVersion 升级）
  - `common/comm/src/shm_ring_buffer.cpp`（实现 TryClaimSlot、TryReclaimSlotIfSafe 重置 claimed、AdvanceConsumerCursor 拒绝回归）
  - `common/comm/include/shm_bus_runtime.hpp`（移除 SyncPeerConsumerCursors 声明、移除 compete_notify_rr_ 成员）
  - `common/comm/src/shm_bus_runtime.cpp`（PublishWithStatus 统一通知所有消费者、TryDispatchOneRingMessage 新增 TryClaimSlot 竞争逻辑、移除 SyncPeerConsumerCursors）
- **配置层**：`common/config/src/channel_topology_config.cpp`（Resolve 参数支持）
- **测试**：`common/comm/split_shm_bus_control_runtime_test.cpp`（调整验证条件）
- **文档**：更新 `docs/channel_topology_runtime.md` 和 OpenSpec specs
- **BREAKING**: kRingLayoutVersion 从 7 升至 8，旧 segment attach 失败；broadcast 模式零改动

该提案为后续详细设计和实现奠定基础。