# Channel topology config parser

## Purpose

Load per-module `.txt` channel definitions and aggregate topology before shared memory allocation.

## Requirements

### Requirement: 模块通道定义 SHALL 从 `.txt` 配置文件加载
每个模块的 `input_channel` 与 `output_channel` SHALL 定义在**该模块独立的 I/O 配置文件**中（与 `launch_plan.txt` 分离）。`launch_plan.txt` 中对应模块子字典 SHALL 包含指向该 I/O 文件的**路径字段**。在共享内存分配前，调用方 SHALL 通过**配置解析器**、以**总配置文件路径**为入口，加载总配置并按各模块路径加载全部 I/O 文件，解析并校验**所有**模块的通道定义，并获得解析器聚合后的全局拓扑数据结构。模块资源字段解析 MUST 同步执行强 schema 校验，且模块名 MUST 与注册工厂名一一对应，否则配置阶段失败。调用接口 MUST 允许直接接收 `registered_module_names`（来自 `ModuleFactoryRegistry::RegisteredNames()`）参与一致性校验。

#### Scenario: 由总配置路径与模块路径加载单模块通道
- **WHEN** 总配置中模块 `MouldDefectDetector` 子字典包含有效的 I/O 配置文件路径且该文件存在
- **THEN** 解析器加载该文件并输出该模块的 `input_channel` 与 `output_channel`（作为全局结果的一部分）

#### Scenario: 由总配置聚合全部模块通道
- **WHEN** 总配置中存在多个模块子字典且各自 I/O 路径均有效
- **THEN** 解析器 MUST 加载每个模块 I/O 文件、解析其中 `input_channel` 与 `output_channel`，并聚合为全局通道拓扑供共享内存分配准备使用

#### Scenario: 模块名与注册名不一致时拒绝启动
- **WHEN** `launch_plan` 中模块名在注册表中不存在或重复映射
- **THEN** 配置解析阶段 MUST 失败并返回明确的模块注册映射错误

### Requirement: Runtime MUST aggregate all module channel configs before SHM allocation
The middleware MUST merge channel definitions from all modules into a global channel topology to compute producer/consumer counts and allocation parameters per channel. The aggregated topology MUST be frozen by master before child startup and treated as read-only control-plane data during child runtime.

#### Scenario: Shared memory sizing uses aggregated consumers
- **WHEN** runtime prepares shared memory for channel `X`
- **THEN** it uses the aggregated topology for `X` (including consumer count and configured parameters) instead of single-module local assumptions

#### Scenario: Topology is immutable after startup barrier
- **WHEN** child processes are already running
- **THEN** runtime MUST NOT mutate channel topology definitions in shared control-plane structures

### Requirement: Conflicting channel definitions MUST fail fast
If modules define conflicting channel roles or incompatible channel parameters, runtime MUST reject initialization with explicit validation errors.

#### Scenario: Startup blocked on channel parameter conflict
- **WHEN** two module configs define channel `X` with incompatible capacity/ttl constraints
- **THEN** runtime aborts startup and reports conflict details for correction
