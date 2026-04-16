## Context

`common/comm/shm_pubsub_bus` 原先为进程内队列与线程派发，无法表达 Broker 与 Infer 分进程部署时的真实 IPC。本 change 在保持上层发布/订阅调用形态稳定的前提下，将单机数据面迁移到 Linux POSIX 共享内存；语义与边界以 `tasks.md` 六个模块为准，不再在本文中扩展未列入任务书的承诺。

**约束（与任务书一致）：**

- 仅 Linux 单机多进程。
- 每模块通过 `.txt` 声明输入/输出通道；启动前汇总全模块配置，再决定共享内存预分配。
- 多消费者独立读游标；回收与 quorum 与在线集合及 generation 一致。
- 至少一次语义下提供重复可判定键 `channel + sequence`，消费侧需能走幂等去重路径。
- 消费者重新上线不追离线期历史（任务书明确允许的简化）；与此一致，不宜再写“绝对不丢消息”之类与模块 5 矛盾的表述。

## Goals / Non-Goals

**Goals（与 `tasks.md` 模块 1–6 对齐）：**

1. POSIX SHM 数据面：段管理、通道映射初始化、版本/magic、替换虚拟 SHM，并通过模块 1 测试。
2. 通道配置：`.txt` 解析与校验、全局拓扑汇总、按拓扑预分配 SHM，并通过模块 2 测试。
3. Ring：固定布局、两阶段提交、布局版本不兼容拒绝附着，并通过模块 3 测试。
4. 多消费者：游标、最小读回收、慢消费者隔离与 lag、去重键接入路径，并通过模块 4 测试。
5. 守护进程协同：数据平面 add/remove、generation 切换、`ONLINE/OFFLINE` 语义、删除触发回收与健康指标，并通过模块 5 测试。
6. 同步与背压占位：`eventfd + epoll`、映射区、fork 前创建与继承、reactor 反序列化与 `ModuleBase` 职责划分；发布等待与阻塞观测日志；完整背压策略不在本 change，并通过模块 6 测试。

**Non-Goals：**

- 跨节点网络总线（`GrpcPubSubBus` 等仍为扩展位，不在本 change）。
- 完整背压治理（超时、阈值、失败分级）— 任务书模块 6 明确延后。
- 不承诺具体吞吐/延迟 SLO；以参数化与可观测为主。

## Decisions

**D1 — 数据面载体：** POSIX `shm_open` + `ftruncate` + `mmap`；每 channel 独立命名段，便于多进程 attach 与调试。备选 `memfd_create`：匿名段跨进程命名与运维成本更高，本 change 不采用。

**D2 — 并发与可见性：** slot 两阶段提交（reserve → write → commit），消费者只读已提交槽位，配合内存序固定可见性边界。

**D3 — 回收：** 基于最小读序号的安全回收；在线消费者集合与 generation 决定 quorum，避免单点 ref_count 在掉线场景下卡死或误回收。

**D4 — 重复投递：** 已提交消息具备可与通道组合的 `sequence`，去重键为 `channel + sequence`；消费端幂等为业务责任，数据面提供可判定路径与契约。

**D5 — 通知：** `eventfd + epoll`；ring 内维护 `(channel, consumer) → eventfd` 映射；守护进程在 `fork` 前创建 fd 以便子进程继承；发布后向在线消费者广播唤醒；`ShmPubSubBus` 内 reactor 负责等待、读 SHM、反序列化并调用订阅 handler，`ModuleBase` 保持主线程回调、仅走发布接口。

**D6 — 拓扑与预分配：** 启动期完成全模块配置解析与拓扑索引后，再按消费者数量与通道参数创建/尺寸化共享内存，避免运行期盲目扩容带来的竞态与兼容风险。

**D7 — 守护进程与数据平面边界：** 共享内存段与 ring 的创建/附着逻辑落在数据面实现（模块 1）；模块 5 交付的是守护进程可调用的消费者在线控制与观测面（add/remove、generation、指标），不在本文假设一个完整的独立“守护进程实现”为本 change 必交工件—以任务书描述为准。

**D8 — 成员与游标规则：** 状态简化为 `ONLINE/OFFLINE`；新增消费者从 ring 最新 slot 起读；删除触发最小游标重算与回收；重新上线视为新消费者、不补历史。

## Risks / Trade-offs

- 段泄漏或命名冲突：启动探测、magic/版本校验、退出清理与运维巡检。
- 慢消费者占满容量：lag 与占用指标、守护进程侧活跃集合治理参数（与模块 4、5 任务一致）。
- 成员变更与回收中间态：generation 单代快照计算 quorum。
- 通知 fan-out 成本：先观测每次发布的唤醒范围与耗时，再议聚合优化（非本 change 必做）。
- 配置冲突或非法拓扑：启动期校验并 fail（模块 2）。
- 重复投递：契约层强调 `channel + sequence` 与消费端幂等（模块 4）。

## Migration Plan

按 `tasks.md` 模块顺序推进；每模块单元测试通过后再进入下一模块：

1. POSIX SHM 段与通道映射，替换虚拟 SHM。  
2. `.txt` 解析、拓扑汇总、驱动预分配。  
3. Ring 布局与提交语义、版本拒绝。  
4. 多消费者游标、回收、lag、去重路径。  
5. 守护进程可调接口、generation、在线语义、指标。  
6. `eventfd + epoll`、继承与 reactor、阻塞观测日志。

## Open Questions

- 背压从“仅观测”升级为可配置超时/失败语义时，错误如何暴露给业务层（后续 change）。
- 是否需要在布局中预留 metadata 扩展槽位以避免后续破坏性改版（后续 change）。
- channel 热重建（不重启进程）是否纳入路线图（当前非目标）。
