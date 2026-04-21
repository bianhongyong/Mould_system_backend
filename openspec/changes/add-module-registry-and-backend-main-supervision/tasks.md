## 1. 模块工厂注册表骨架

- [x] 1.1 新增 `ModuleFactoryRegistry` 头/源文件，定义 `Register`、`Create`、`RegisteredNames` 与单例访问入口。
- [x] 1.2 定义统一构造器类型与错误返回约定（未注册、重复注册、构造失败）并补齐基础日志。
- [x] 1.3 在现有构建脚本中接入注册表编译目标，确保 backend 主程序和相关测试可链接。

## 2. 宏注册机制与编译期约束

- [x] 2.1 实现 `REGISTER_MOULD_MODULE(ModuleClass)` 与 `REGISTER_MOULD_MODULE_AS(alias, ModuleClass)` 宏，通过静态对象触发注册。
- [x] 2.2 在模板注册入口添加 `static_assert`，强制 `ModuleClass` 继承 `ModuleBase` 且满足预期构造签名。
- [x] 2.3 实现重复模块名拒绝策略（禁止覆盖）并提供明确错误信息。

## 3. launch_plan 与注册表一致性校验接入

- [x] 3.1 为 `ParseLaunchPlanFile(...)` 扩展 `registered_module_names` 输入参数，并在解析阶段执行模块名一致性校验。
- [x] 3.2 在解析失败路径补充错误上下文（缺失模块名、重复映射来源）以便定位配置问题。
- [x] 3.3 调整调用链与接口适配代码，确保主进程可直接传入 `ModuleFactoryRegistry::RegisteredNames()`。

## 4. backend main 启动与监控主流程

- [x] 4.1 在 `main.cpp` 完成初始化阶段（参数、日志、信号）并加载 launch_plan。
- [x] 4.2 按 `startup_priority` 分批启动模块进程，批内随机并使用 `ReadyPipeProtocol` 实现 READY/FAILED 栅栏。
- [x] 4.3 实现 `waitpid` 监控循环：子进程异常退出后调用 `RestartPolicy` 做延迟重启或熔断抑制，主进程保持存活。
- [x] 4.4 实现优雅退出流程：分层终止子进程、`waitpid` 回收防僵尸、控制面资源最终清理。

## 5. 构建整体测试环境与样例业务链路

- [x] 5.1 新增至少 3 个继承 `mouldBase/ModuleBase` 的样例业务模块（如 `FrameSourceModule`、`FeatureExtractModule`、`ResultSinkModule`），并通过注册宏接入工厂；目的：构建可运行的多进程业务样本。
- [x] 5.2 为样例模块分别创建输入/输出通道配置文件（`input_channel` / `output_channel`），形成“源模块 -> 中间处理模块 -> 汇聚模块”的链式通信拓扑；目的：验证进程间消息传递与通道聚合正确性。
- [x] 5.3 创建对应 `launch_plan` 文件，覆盖模块路径、优先级、重启策略与 I/O 配置引用；目的：提供 main 进程可直接加载的整体测试编排输入。

## 6. GTest 单元测试与验收覆盖

- [x] 6.1 新增 `ModuleFactoryRegistry_CreateRegisteredModule_ReturnsInstance`：验证已注册模块可按名称创建实例；目的：覆盖“模块名 -> 构造器 -> 实例”主路径。
- [x] 6.2 新增 `ModuleFactoryRegistry_CreateUnknownModule_ReturnsNotFound`：验证未注册模块创建失败且返回明确错误；目的：覆盖错误语义与可观测性要求。
- [x] 6.3 新增 `ModuleFactoryRegistry_DuplicateRegistration_ReturnsAlreadyExists`：验证同名重复注册被拒绝且不覆盖原映射；目的：覆盖一致性与确定性约束。
- [x] 6.4 新增 `LaunchPlanParser_ValidateAgainstRegisteredNames_FailsOnMismatch`：验证 `ParseLaunchPlanFile(...registered_module_names...)` 能识别未注册模块并失败；目的：覆盖配置与注册表联动校验。
- [x] 6.5 新增 `MainSupervisor_StartupByPriority_RespectsReadyBarrier`：构造多优先级模块假实现，验证下批启动受 READY/FAILED 栅栏控制；目的：覆盖主流程批次启动约束。
- [x] 6.6 新增 `MainSupervisor_RestartPolicyOnCrash_PerformsBackoffOrFuse`：模拟子进程异常退出，验证重启策略与熔断分支行为；目的：覆盖监控循环稳定性。
- [x] 6.7 新增 `MainSupervisor_Shutdown_ReapsChildrenWithoutZombie`：模拟退出信号并断言全部子进程被回收；目的：覆盖优雅退出与资源回收规范。

## 7. 整体测试与集成验证（放在单元测试后）

- [x] 7.1 增加用于集成验证的测试资源目录与说明（样例配置、期望启动顺序、期望交互结果），并在构建/测试目标中纳入；目的：保证本地与 CI 可复现实验场景。
- [x] 7.2 增加一个端到端冒烟脚本或测试入口，启动主进程并观测样例链路完成一次完整交互（含 READY 握手）；目的：在单元测试通过后验证系统级测试环境可用。

