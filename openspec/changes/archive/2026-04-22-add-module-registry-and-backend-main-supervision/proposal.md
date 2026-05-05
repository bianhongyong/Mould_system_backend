## Why

当前后端模块拉起路径缺少统一的“模块名 -> 构造器”注册机制，模块创建仍依赖手工映射，导致新增模块时易漏配、难校验。
同时主进程入口尚未形成完整的启动、监控、异常重启与优雅退出闭环，需要把 launch_plan、Supervisor、ReadyPipeProtocol、RestartPolicy 串成可运行主流程。

## What Changes

- 新增 `ModuleFactoryRegistry` 与编译期宏注册机制，支持 `ModuleBase` 子类在静态初始化阶段自动注册。
- 为注册表提供按名称创建实例、重复注册拒绝、注册名枚举导出（`RegisteredNames()`）等能力。
- 将 `main.cpp` 实现为完整主进程生命周期入口：配置加载、参数校验、批次启动、READY/FAILED 栅栏、waitpid 监控、按策略重启、优雅退出清理。
- 在 launch_plan 解析前引入“配置模块名与注册表名集合一致性校验”，避免配置引用未注册模块。

## Capabilities

### New Capabilities
- `module-factory-registry`: 提供模块构造器注册、创建与注册名枚举能力，并通过宏支持 `ModuleBase` 子类静态注册。

### Modified Capabilities
- `main-process-supervisor-lifecycle`: 扩展主进程生命周期要求，纳入注册表一致性校验、分批启动栅栏、异常监控重启与优雅退出行为。
- `channel-topology-config-parser`: 增加 launch_plan 解析与模块注册名集合联动校验约束，要求能够消费注册表导出的名称集合。

## Impact

- 影响 `services/backend` 主程序入口、模块工厂/模块基类相关代码与启动流程编排逻辑。
- 影响 launch_plan 解析与校验接口签名（需接入 `registered_module_names` 集合）。
- 影响运行期稳定性行为：模块故障重启策略、熔断抑制以及主进程退出资源清理路径。
