## ADDED Requirements

### Requirement: ModuleBase 集成

网关 SHALL 继承 `ModuleBase`，在 `DoInit()` 中启动 muduo HTTP 服务器，在 `OnRunIteration()` 中执行轻量心跳循环。

#### Scenario: 模块初始化

- **WHEN** supervisor 启动网关子进程
- **THEN** 网关调用 `DoInit()` → muduo 服务器启动 → ready-pipe 通知父进程 → 进入事件循环

#### Scenario: 模块退出

- **WHEN** 网关收到 SIGTERM 信号
- **THEN** `DoCleanup()` SHALL 优雅关闭 muduo 服务器（停止接受新连接、等待处理中请求完成），然后退出事件循环

### Requirement: supervisor 监管

网关 SHALL 作为 supervisor 管理下的子进程纳入 `launch_plan`，CPU 亲和性、重启策略、优先级等配置与其他模块一致。

#### Scenario: crash 后重启

- **WHEN** 网关进程异常崩溃
- **THEN** supervisor SHALL 按配置的重启策略（退避/熔断）重新启动网关进程

#### Scenario: 启动优先级

- **WHEN** launch_plan 中网关的启动优先级低于 SHM control plane
- **THEN** supervisor SHALL 先启动 control plane 及相关模块，SHM 段就绪后再启动网关

### Requirement: 通道拓扑集成

网关所占图片通道 SHALL 在通道拓扑配置中声明，与全拓扑无生产者冲突。

#### Scenario: 拓扑校验通过

- **WHEN** 通道拓扑配置中声明 `image_channel_http` 通道，生产者角色分配给网关
- **THEN** `ChannelTopologyConfig` 解析器 SHALL 校验通过，计算 SHM 段大小并分配槽位

#### Scenario: 拓扑校验失败

- **WHEN** `image_channel_http` 通道在拓扑中已存在其他生产者
- **THEN** `ChannelTopologyConfig` SHALL 拒绝解析并报告冲突

### Requirement: 构建系统集成

仓库根 `CMakeLists.txt` SHALL 通过 `add_subdirectory(gate)` 纳入本模块；muduo SHALL 以外部动态库方式链接。

#### Scenario: 根构建包含 gate

- **WHEN** 执行 `cmake --build build --target backend_all`
- **THEN** gate 模块及其测试目标 SHALL 被构建

#### Scenario: muduo 动态库链接

- **WHEN** 检查 gate 模块的链接依赖
- **THEN** muduo SHALL 以 `-lmuduo_*` 形式链接为共享库，SHALL NOT 以源码编译目标形式存在

### Requirement: 可观测性

网关 SHALL 记录访问与结果维度的日志，SHOULD 暴露成功率、延迟、SHM 拒绝次数、OSS 失败与重试次数等指标。

#### Scenario: 访问日志

- **WHEN** 每个 HTTP 请求完成（无论成功或失败）
- **THEN** 网关 SHALL 记录一行结构化日志，包含 `request_id`、`node_id`、HTTP 状态码、延迟毫秒、SHM 发布结果、OSS 上传结果分类

#### Scenario: 指标暴露

- **WHEN** 网关运行中
- **THEN** 可通过日志或简单 HTTP 端点获取累计成功率、平均延迟、SHM 拒绝计数、OSS 失败计数
