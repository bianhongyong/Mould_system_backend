# Linux SHM pubsub runtime

## Purpose

`ShmPubSubBus` and factory behavior for single-node Linux shared-memory pub/sub.

## Requirements

### Requirement: `ShmPubSubBus` SHALL provide true multi-process IPC semantics

Linux SHM pub/sub runtime SHALL preserve true cross-process IPC semantics after control-plane and data-plane split: the control plane governs shared-memory channel initialization and lifecycle, while the data plane performs cross-process read/write and dispatch without process-local fallback transport.

#### Scenario: Broker and Infer communicate via split runtime from separate processes

- **WHEN** control plane has initialized channel segments, Broker data plane publishes, and Infer runs in another process
- **THEN** Infer reads committed payload through shared memory and the message path does not use in-process fallback transport

### Requirement: Channel factory SHALL resolve single-node mode to Linux SHM runtime

`ChannelFactory` SHALL resolve single-node mode to a Linux SHM data-plane runtime instance. The data-plane instance SHALL assume control-plane lifecycle preparation is complete and only perform attach/use/detach, never `shm_unlink`; parent process SHALL hold a singleton data-plane base instance that forked child processes reuse through COW for read-only base state.

#### Scenario: Single-node mode selects data-plane runtime

- **WHEN** a business process requests bus instance under single-node deployment mode
- **THEN** factory returns SHM data-plane implementation and that implementation does not trigger global `unlink`

#### Scenario: Forked children reuse parent singleton base state

- **WHEN** parent process creates singleton data-plane base instance and then forks business child processes
- **THEN** child processes reuse the read-only base state directly and finish required local runtime resource initialization per process

### Requirement: SHM 通道 SHALL 支持可配置的投递模式（broadcast / compete）

`ShmBusRuntime` SHALL 读取通道拓扑参数 `delivery_mode`：`broadcast`（默认）向所有在线消费者 `event_fd` 发信号，每个消费者独立处理每条消息；`compete` 同样通知所有在线消费者，但消息槽内置原子 `claimed` 标志，消费者通过 CAS（0→1）抢占该标志，仅抢到者执行 handler，所有消费者均独立推进自己的游标（无需游标同步）。非法 `delivery_mode` 值 SHALL 在 `BuildChannelTopologyIndex` 阶段被拒绝。

#### Scenario: 默认广播投递

- **WHEN** 未配置 `delivery_mode` 或配置为 `broadcast`
- **THEN** 每次发布后所有在线订阅者均被唤醒并各处理该消息一次

#### Scenario: 抢占投递与原子 CAS 竞争

- **WHEN** 通道配置 `delivery_mode=compete` 且存在多个在线订阅者
- **THEN** 每次发布后所有在线消费者均被唤醒，通过 CAS 竞争槽级 `claimed` 标志，恰好一个消费者抢到并执行业务 handler；所有消费者均独立推进游标，保证 at-most-once 语义，且 `MinActiveReadSequence` 正确反映全局最慢消费者进度，槽回收不受阻塞