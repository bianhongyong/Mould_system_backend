# Tasks

## 1. 更新配置解析支持 delivery_mode 参数

- [x] 1.1 在 common/config/src/channel_topology_config.cpp 中扩展 Resolve 函数，支持从 params 读取 `delivery_mode`（broadcast/compete），默认 broadcast
- [x] 1.2 更新 channel_topology_config.hpp 添加相关 helper 函数（如 ResolveDeliveryModeForChannel）
- [x] 1.3 在 Parse/Validate 逻辑中添加对新参数的合法性校验

## 2. ShmBusRuntime 架构扩展

- [x] 2.1 在 common/config/include/channel_topology_config.hpp 中新增 `enum class ShmBusDeliveryMode`（与 comm 共用，避免重复枚举）
- [x] 2.2 扩展 ChannelRuntime 或新增 per-channel map 缓存 delivery mode（从 topology 加载）
- [x] 2.3 在 SubscriberEntry 或 ChannelRuntime 中记录 mode 信息
- [x] 2.4 更新 Subscribe() 方法以支持新模式下的 consumer slot 分配（与 broadcast 相同槽位模型，无需改动 Subscribe）

## 3. 生产者发布路径修改（最小改动核心）

- [x] 3.1 修改 ShmBusRuntime::PublishWithStatus()：broadcast 和 compete 统一通知所有在线消费者的 event_fd（不再区分模式）
- [x] 3.2 移除 compete 模式下的轮询选择逻辑（不再需要 round-robin ticket）
- [x] 3.3 移除 compete_notify_rr_ 成员变量
- [x] 3.4 添加日志支持

## 4. 消费者处理与游标推进（解决游标同步竞态）

- [x] 4.1 在 SlotMeta 中增加 `std::atomic<std::uint32_t> claimed` 字段（利用 4 字节 padding，结构体大小不变）
- [x] 4.2 新增 `RingLayoutView::TryClaimSlot(slot_index)` 方法，通过原子 CAS（0→1）尝试抢占槽
- [x] 4.3 修改 TryDispatchOneRingMessage()：compete 模式下先 TryClaimSlot，成功者执行 handler，失败者跳过；无论抢到与否均调用 CompleteConsumerSlot 独立推进游标
- [x] 4.4 移除 SyncPeerConsumerCursors 静态方法及其所有调用
- [x] 4.5 AdvanceConsumerCursor 改为拒绝回归（不再需要为 SyncPeerConsumerCursors 允许回归）
- [x] 4.6 TryReclaimSlotIfSafe 和 ResetSlot 中重置 claimed=0
- [x] 4.7 kRingLayoutVersion 从 7 升至 8

## 5. 测试、文档与验证

- [x] 5.1 更新 docs/channel_topology_runtime.md，添加 delivery_mode 参数说明和 compete 使用示例
- [x] 5.2 更新 openspec/specs/linux-shm-pubsub-runtime/spec.md 等主规格（delta）
- [x] 5.3 在 CMakeLists.txt 和测试目标中确保新测试被包含
- [x] 5.4 运行现有测试验证无回归（broadcast 行为不变）——**88 个测试全部通过**

## 6. 详尽单元测试

本变更所有相关改动**必须**添加 GTest 单元测试，测试代码统一添加到对应改动部分的 test.cpp 文件中（multi_consumer_cursor_reclaim_test.cpp 和 split_shm_bus_control_runtime_test.cpp）。测试使用 GTEST 框架，覆盖正常、边界、错误和性能场景。以下为具体测试清单（每个测试包括名称、大概内容、测试目的）：

**在 multi_consumer_cursor_reclaim_test.cpp 中新增：**

- [x] **TEST(CompeteDeliveryMode, CursorSynchronizationAfterConsume)** — 配置 compete 模式通道，注册2个消费者，发布10条消息，验证所有消费者 cursor 均推进到相同值；检查 MinActiveReadSequence() 和 ReclaimCommittedSlots() 正确工作。
- [x] **TEST(CompeteDeliveryMode, IndependentCursorAdvanceNoRegression)** — 模拟竞争场景，验证 AdvanceConsumerCursor 拒绝回归（current > next_read_sequence 时返回 false）。
- [x] **TEST(CompeteDeliveryMode, BackpressureAndReclaimCorrectness)** — 模拟慢消费者，验证 compete 模式下即使部分消费者滞后，reclaim 仍能正常进行。
- [x] **TEST(CompeteDeliveryMode, ErrorHandlingAndFallback)** — 测试 handler 异常时游标仍推进；测试 TryClaimSlot 对无效 slot_index 返回 false。

**在 split_shm_bus_control_runtime_test.cpp 中新增集成测试：**

- [x] **TEST(ShmBusRuntime, CompeteModeEndToEnd)** — 端到端验证 compete 模式：delivered count == publish count，所有 cursor 一致。
- [x] **TEST(ShmBusRuntime, BroadcastBackwardCompatibility)** — 验证 broadcast 行为不变，零 breaking change。
- [x] **TEST(ShmBusRuntime, TopologyParamParsing)** — 测试 ResolveDeliveryModeForChannel 对各种参数的处理（有效/无效/默认）。
- [x] **TEST(ShmBusRuntime, MultiConsumerCompeteWithFork)** — fork 多消费者进程，验证跨进程 CAS 竞争和消息不重复。
