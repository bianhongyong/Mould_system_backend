# 验收标准确认

本变更验收标准如下：

1. 工程骨架可构建：根 `CMakeLists.txt` + 分层模块 + 子目录组织齐备。
2. 探针可运行：`third_party_probe` 接入构建与 `ctest`。
3. CI 闸门生效：`cmake configure -> build -> ctest` 在 CI 工作流中定义为必经步骤。

验收说明：

- 骨架与构建入口：已完成。
- 探针与本地检查入口：已完成，`third_party_probe_self_test` 通过。
- CI 基础闸门：已完成（`.github/workflows/backend-dependency-gate.yml`）。

结论：验收标准满足，工程骨架可构建、探针可运行、CI 闸门已定义并可执行。

## 主进程监管治理验收（master-supervisor-runtime-governance）

本轮补充验收标准如下：

1. 启动编排符合优先级批次推进规则，并存在 READY 屏障。
2. READY Pipe 协议可覆盖成功、超时、失败迁移。
3. 异常退出触发重启治理：指数退避、窗口计数、重试上限与熔断。
4. 熔断打开时 master 保持存活，具备持续监管与可观测能力。
5. SHM 生命周期满足：按 PID 下线 consumer、仅最终 master 退出时 unlink。

验收清单：

- [x] `SupervisorSpec.Supervisor_StartupPriorityBatchReadyBarrier`
- [x] `ReadyPipeProtocolSpec.ReadyPipeProtocol_TransitionsSuccessTimeoutFailure`
- [x] `RestartPolicySpec.RestartPolicy_BackoffRetryWindowFuseBehavior`
- [x] `SupervisorSpec.Supervisor_FuseOpenKeepsMasterAlive`
- [x] `SplitShmBusControlRuntimeSpec.ShmControlPlane_OfflineConsumerSlotsByPidOnly`
- [x] `SplitShmBusControlRuntimeSpec.ShmChannelLifecycle_UnlinkOnlyOnFinalMasterShutdown`
- [x] `SplitShmBusControlRuntimeSpec.SupervisorIntegration_CrashRestartLoopAndShmMembership`

运行期语义（对齐实现）：

- 启动路径以 `startup_priority` 分批；批内首次启动随机化，同优先级重启集合大小大于 1 时随机化。
- 只有当前批次实例进入 `READY` 或 `FAILED` 显式策略路径后，才允许释放下一批次。
- 每次异常退出均先执行重启策略评估；熔断期间禁止重拉起，但 master 进程不快速失败。
