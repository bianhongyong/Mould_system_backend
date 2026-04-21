## 为什么

当前后端尚未形成一套完整且可强制执行的主进程生命周期规范，用于模块监管、启动门控、READY 信号与重启治理。需要立即冻结这些契约，确保后续实现在故障场景下仍具备确定性行为，并满足严格配置校验要求。

## 变更内容

- 定义仅基于 fork 的监管模型：每个子进程严格承载一个继承自 `ModuleBase` 的模块实例。
- 引入严格的启动计划资源 Schema 校验，重启相关字段必须存在，`cpu_set` 非法时立即硬失败。
- 定义按 `startup_priority` 的启动编排、同优先级首次启动与重启轮次的随机化规则，以及基于 READY 的批次门控。
- 规定子进程通过 pipe 向父进程上报 READY，并定义父进程侧模块生命周期状态。
- 定义首版完整重启治理：指数退避、重试上限、时间窗口计数与熔断行为。
- 明确共享内存控制面归属：由 master 初始化并冻结拓扑；子进程仅附着/使用；子进程退出时按 PID 下线 consumer 槽位。
- 在子进程故障期间保持 channel 对象存活，仅在 master 最终退出时 unlink SHM channel。

## 能力

### 新增能力
- `main-process-supervisor-lifecycle`：用于模块启动、监控与重拉起的监管生命周期与纯 fork 进程模型。
- `resource-schema-validator`：对启动计划资源字段进行强 Schema 解析与校验，并支持 `cpu_id` 到 `cpu_set` 的兼容映射。
- `ready-pipe-protocol`：用于启动屏障的父子进程 READY 上报协议与状态机迁移。
- `restart-policy-governance`：对异常退出进行确定性治理，包含指数退避、最大重试、窗口跟踪与熔断行为。

### 修改能力
- `channel-topology-config-parser`：资源 Schema 约束从自由键值升级为必填且可校验字段。
- `linux-shm-data-plane`：加强控制面生命周期治理，支持按 PID 下线 consumer 与跨重拉起复用 channel。

## 影响

- 受影响代码区域包括：启动计划解析、模块注册表/工厂映射、监管生命周期编排、IPC READY 信号、SHM 控制面与成员状态维护。
- 新核心实现组件将围绕 `Supervisor`、`ResourceSchemaValidator`、`ReadyPipeProtocol` 与 `RestartPolicy` 展开。
- 测试框架继续使用 GTest，并新增对校验失败、启动屏障行为、重启治理与 SHM 成员一致性的覆盖。
