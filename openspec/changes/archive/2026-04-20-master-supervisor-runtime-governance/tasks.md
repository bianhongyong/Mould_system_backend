## 1. Schema 与注册预检

- [x] 1.1 引入强类型资源 Schema 模型，包含必填字段（`startup_priority`、`cpu_set`、`restart_backoff_ms`、`restart_max_retries`、`restart_window_ms`、`restart_fuse_ms`、`ready_timeout_ms`）。
- [x] 1.2 实现 `ResourceSchemaValidator`，提供严格解析/校验错误，并支持遗留 `cpu_id` 到 `cpu_set` 的兼容映射。
- [x] 1.3 增加启动预检，强制启动计划中的模块名与工厂注册保持一致。
- [x] 1.4 增加 GTest 用例，覆盖必填字段缺失、`cpu_set` 非法、注册不匹配三类拒绝场景。

## 2. Supervisor 核心进程模型

- [x] 2.1 实现 `Supervisor` 进程生命周期，满足仅 fork 启动与每子进程单模块不变量。
- [x] 2.2 增加按 `startup_priority` 的启动分组，并在同优先级首次启动时进行随机排序。
- [x] 2.3 实现同优先级重启轮次排序（仅当待重启集合大小 > 1 时随机化）。
- [x] 2.4 增加 GTest 覆盖，验证优先级批次顺序与随机化规则。

## 3. READY Pipe 协议与状态机

- [x] 3.1 实现 `ReadyPipeProtocol` 消息契约与父进程管理的 pipe 生命周期。
- [x] 3.2 增加父进程侧模块状态机迁移（`FORKED`、`INITING`、`READY`、`RUNNING`、`FAILED`），并记录失败原因。
- [x] 3.3 强制执行基于 READY 的启动屏障，使下一优先级批次等待 READY 或进入显式失败策略路径。
- [x] 3.4 基于 `ready_timeout_ms` 增加超时处理，并补充 READY 成功/失败迁移行为的 GTest。

## 4. 重启策略治理

- [x] 4.1 在每次子进程退出时执行 `RestartPolicy` 评估（仅异常路径）。
- [x] 4.2 基于 `restart_backoff_ms` 实现指数退避调度，并基于 `restart_window_ms` 实现窗口计数。
- [x] 4.3 强制执行 `restart_max_retries` 与 `restart_fuse_ms` 熔断行为，同时保持 master 存活。
- [x] 4.4 增加 GTest 用例，覆盖重试上限、退避增长、熔断抑制与 Supervisor 非快速失败行为。

## 5. SHM 控制面生命周期与成员一致性

- [x] 5.1 在子进程启动前，由 master 完成（或确认）SHM 控制面初始化与拓扑冻结。
- [x] 5.2 确保子模块仅附着/使用现有 SHM 控制/数据结构，不得修改拓扑定义。
- [x] 5.3 在子进程退出时实现基于 PID 的 consumer 槽位下线，同时保留 channel 对象以支持重拉起复用。
- [x] 5.4 将 SHM channel unlink 限制在 master 最终关闭路径，并为生命周期不变量补充 GTest 覆盖。

## 6. 集成、可观测与验收

- [x] 6.1 增加集成测试，覆盖多优先级模块计划下端到端启动屏障流程。
- [x] 6.2 增加集成测试，覆盖崩溃/重启循环，并验证重启策略与 SHM 成员关系正确性。
- [x] 6.3 增加日志/指标埋点，覆盖 READY 迁移、重启计数、熔断状态与启动顺序轨迹。
- [x] 6.4 更新文档与验收清单，明确严格 Schema 要求与运行期重启语义。

## 7. 详尽单元测试（GTest）

- [x] 7.1 `ResourceSchemaValidator_RejectsMissingRequiredFields`
  - 大概内容：构造缺少 `startup_priority`、`cpu_set`、`restart_max_retries` 等必填字段的配置样本，逐项验证解析失败路径与错误信息。
  - 测试目的：确保 Schema 必填约束严格执行，避免运行时进入不完整策略。
- [x] 7.2 `ResourceSchemaValidator_RejectsInvalidCpuSetAndMapsLegacyCpuId`
  - 大概内容：覆盖非法 `cpu_set`（空、越界、格式错误）与合法 `cpu_id` 兼容映射场景，校验失败与映射结果。
  - 测试目的：保证 CPU 亲和配置安全可用，并平滑兼容历史配置。
- [x] 7.3 `Supervisor_StartupPriorityBatchReadyBarrier`
  - 大概内容：构造多优先级模块计划，验证只有当前批次全部 READY 或进入明确失败策略后，才允许推进下一批次。
  - 测试目的：确保 READY 启动屏障有效，避免依赖链被提前拉起。
- [x] 7.4 `Supervisor_SamePriorityRandomizationRules`
  - 大概内容：验证同优先级首次启动随机化、以及重启轮次仅在待重启集合大于 1 时随机化；固定随机种子进行可复现断言。
  - 测试目的：保证公平性策略与可复现性同时成立。
- [x] 7.5 `ReadyPipeProtocol_TransitionsSuccessTimeoutFailure`
  - 大概内容：模拟 READY 正常上报、超时未上报、协议异常三类路径，断言状态机从 `FORKED`/`INITING` 到 `READY`/`FAILED` 的迁移及失败原因。
  - 测试目的：保证 READY 协议健壮性与状态机迁移确定性。
- [x] 7.6 `RestartPolicy_BackoffRetryWindowFuseBehavior`
  - 大概内容：模拟连续异常退出，验证指数退避增长、窗口计数重置逻辑、重试上限与熔断开启后的抑制重拉起行为。
  - 测试目的：确保重启治理按策略执行，防止无限重启或错误熔断。
- [x] 7.7 `Supervisor_FuseOpenKeepsMasterAlive`
  - 大概内容：触发子模块熔断后，验证 master 进程持续存活且可继续对外暴露状态与指标。
  - 测试目的：满足“非快速失败”治理目标，保留可观测与后续恢复能力。
- [x] 7.8 `ShmControlPlane_OfflineConsumerSlotsByPidOnly`
  - 大概内容：模拟子进程退出，验证仅下线对应 PID 的 consumer 槽位，不影响其他在线消费者。
  - 测试目的：保证 SHM 成员关系一致性与隔离性。
- [x] 7.9 `ShmChannelLifecycle_UnlinkOnlyOnFinalMasterShutdown`
  - 大概内容：验证子进程异常与重启过程中 channel 对象持续可复用，仅在 master 最终关闭时发生 unlink。
  - 测试目的：确保 channel 生命周期稳定，避免数据面抖动与重复建链成本。
