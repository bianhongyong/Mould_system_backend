## Why

当前 `ShmPubSubBus` 仍是进程内虚拟共享内存模型，无法满足 Broker/Infer 多进程间的真实 IPC。本 change 将数据面升级为 Linux POSIX 共享内存，并在不改上层业务发布/订阅调用形态的前提下，使跨进程传输路径可验证、可测试。

## What Changes

本 change 的范围与 `tasks.md` 中六个模块一一对应：

1. **POSIX 共享内存数据面基础**：在 `common/comm` 增加段管理（命名、`shm_open`、`ftruncate`、`mmap`、unmap/close）；定义通道映射初始化（create/attach、版本与 magic 校验、参数化容量）；将单机默认数据面切到 Linux SHM，并移除旧的进程内虚拟 SHM 路径。
2. **模块通道配置解析与汇总**：解析各模块 `.txt` 通道声明（输入/输出、可选参数）并做语法校验；汇总为 channel → producers/consumers/count/params 的拓扑索引；按拓扑预分配共享内存段（按消费者数与参数）。
3. **Ring Buffer 布局与提交语义**：固定布局 `RingHeader`、`ConsumerTable`、`SlotMeta`、`PayloadRegion`；slot 两阶段提交（reserve/write/commit）与可见性内存序；布局版本不兼容则拒绝附着。
4. **多消费者游标与回收**：每消费者独立读游标注册、推进与持久状态；基于最小读序号的安全回收与容量返还；慢消费者隔离与 lag；消费侧接入基于 `channel + sequence` 的幂等去重检查路径。
5. **守护进程消费者在线治理与可观测性**：数据平面向守护进程暴露消费者 add/remove 控制接口（契约与参数校验）；online set 的 generation 原子切换（影响回收 quorum，不要求全局停写）；成员状态 `ONLINE/OFFLINE`（新增从 ring 最新 slot 起读，重新上线视为新消费者、不追离线期消息；删除后重算最小游标并触发回收）；暴露占用、阻塞时长、lag、回收次数、成员变更与上下线事件等联合健康指标。
6. **跨进程同步与背压**：`eventfd + epoll` 通知，ring 内 `(channel, consumer) -> eventfd` 映射区；守护进程在 `fork` 前创建并继承 eventfd，发布后唤醒在线消费者，消费者按 `read_sequence` 从 SHM 消费；`ShmPubSubBus` 内 reactor 线程反序列化并透传到订阅 handler，`ModuleBase` 仅发布、主线程执行业务回调；本阶段不实现完整背压治理，仅发布等待与阻塞观测日志（起止/时长/occupancy/lag）。

## Capabilities

### New Capabilities

- `linux-shm-data-plane`：对应模块 1 — POSIX SHM 段与通道映射初始化、生命周期与单机默认路径。
- `channel-topology-config-parser`：对应模块 2 — 模块 `.txt` 解析、拓扑汇总与预分配驱动。
- `ring-buffer-slot-contract`：对应模块 3 — 固定布局、两阶段提交、版本拒绝附着。
- `multi-consumer-cursor-reclaim`：对应模块 4 — 多消费者游标、最小读回收、慢消费者隔离、`channel + sequence` 去重路径。
- `daemon-consumer-governance-observability`：对应模块 5 — 守护进程可调控制接口、generation 切换、在线语义与联合健康指标。
- `process-shared-sync-backpressure`：对应模块 6 — `eventfd + epoll`、fork 前继承、reactor 与发布侧观测日志（背压策略留待后续）。

### Modified Capabilities

- None.

## Impact

- **受影响代码（与六模块实现落点一致，非穷举）**：`common/comm`（SHM、总线、ring、通知、消费者控制）、模块通道配置与汇总相关路径、`tests/` 下各模块对应单测。
- **行为与契约**：`IPubSubBus` 实现从进程内模拟变为真实跨进程 IPC；ring、游标、回收、generation 与通知映射等行为以 `tasks.md` 与 delta spec 为准。
- **运行环境**：Linux/POSIX 共享内存、`eventfd`、`epoll`；Broker/Infer 与守护进程的进程模型及 `/dev/shm` 等部署约束。
