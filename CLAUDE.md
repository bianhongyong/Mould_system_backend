# CLAUDE.md

本文为 Claude Code (claude.ai/code) 在此仓库中工作时的指导说明。

## 输出语言

所有输出必须使用中文。

## 构建与测试

```bash
# 配置
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 构建所有后端目标
cmake --build build --target backend_all

# 运行所有测试
ctest --test-dir build --output-on-failure

# 按名称运行指定测试
ctest --test-dir build -R <test_name> --output-on-failure

# 按标签分类运行测试
ctest --test-dir build -L shm_pubsub --output-on-failure

# 全新构建（已验证配置）
cmake -S . -B build-clean -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-clean --target backend_all
ctest --test-dir build-clean --output-on-failure
```

测试标签: `unit`, `shm_segment`, `shm_ring_buffer`, `shm_pubsub`, `channel_topology_config`, `launch_plan_config`, `multiprocess`, `module_base`, `supervisor`, `module_factory_registry`, `sync`, `backpressure`, `control_plane`, `membership`, `proto`, `smoke`, `third-party`

## 代码库架构

### 概述

工业缺陷检测（覆膜检测）后端系统。采用多进程架构，**主进程** (`backend_main`) fork 并监管通过 POSIX 共享内存 IPC 通信的**子模块进程**。

### 关键目录

- `common/comm/` — 通信中间件：共享内存 pub/sub 总线、模块基类、监管器
- `common/config/` — 配置解析器：启动配置 (JSON)、通道拓扑 (txt)
- `common/include/` — 通用工具：日志、测试辅助
- `common/proto/` — IPC 消息的 Protobuf 定义
- `services/backend/` — 主进程入口点和模块实现
- `tests/` — 集成测试数据和 proto 定义
- `cmake/` — 构建配置：平台标志、第三方依赖解析、自定义 Find 模块
- `docs/` — 架构文档、构建说明、通道拓扑运行时文档

### 进程模型

```
backend_main (主进程)
  ├── supervisor: fork/exec、健康检查、带退避/熔断的重启
  ├── control plane: 创建共享内存段、管理消费者生命周期
  ├── config: 解析 launch_plan.json → 解析模块配置 → 设置环境变量
  ├── ready-pipe 协议: 等待子进程 READY 信号
  └── 子进程 (模块):
       ├── FrameSourceModule      — 发布帧
       ├── FeatureExtractModule   — 消费帧、提取特征
       ├── RiskEvalModule         — 风险评估
       ├── SensorFusionModule     — 传感器融合
       ├── PathPlanModule         — 路径规划
       ├── ActuatorCoordModule    — 执行器协调
       └── ... (均继承 ModuleBase)
```

模块通过 `REGISTER_MOULD_MODULE_AS("ModuleName", ClassName)` 宏在静态初始化时自动注册到 `ModuleFactoryRegistry`。

### 通信中间件

- **接口** (`interfaces.hpp`): `IPubSubBus`, `IEventLoop`, `TimerScheduler`, `MessageCodec`
- **SHM 总线**: 分为 `ShmBusControlPlane`（创建/管理 POSIX SHM 段）和 `ShmBusRuntime`（运行时挂载）。
- **模块基类** (`ModuleBase`): 模板方法模式 — `DoInit()` → `SetupSubscriptions()` → `OnRunIteration()` 循环。使用 `CallbackQueue` 实现无锁回调投递。
- **通道拓扑**: 每个模块在 `.txt` 文件中声明 I/O 通道。`ChannelTopologyConfig` 解析器验证无重复生产者、无参数冲突，并驱动 SHM 段大小计算。
- **投递模式**: `broadcast`（所有消费者收到每条消息）和 `compete`（仅一个消费者收到）。
- **环形缓冲区**: 单生产者、多消费者环形缓冲区，采用两阶段提交（预留 → 拷贝 → 提交）。每个消费者有独立的游标。
- **Protocol Buffers**: 所有 IPC 消息使用 protobuf，构建时通过 `protoc` 生成。

### 配置流程

1. `launch_plan.json` 指定模块、资源（CPU 亲和性、重启策略、优先级）和通道配置路径
2. `ParseLaunchPlanFile` 解析所有内容并验证已注册的模块
3. `SetupRuntimeEnvironmentFromLaunchPlan` 设置环境变量（`MOULD_MODULE_CHANNEL_CONFIGS`、`MOULD_SHM_SLOT_COUNT` 等）
4. `ShmBusControlPlane::ProvisionChannelTopologyFromModuleConfigs` 创建 SHM 段
5. Supervisor 按启动优先级批次 fork 子进程；每个子进程通过 `ModuleFactoryRegistry` 创建其 `ModuleBase` 子类

### 依赖项 (cmake/third_party.cmake)

必需: protobuf, gRPC, abseil, gflags, glog, Boost (filesystem/system/thread), OpenCV, ONNX Runtime, MySQL 客户端, RabbitMQ C 客户端, OSS SDK, moduo

### 测试基础设施

测试使用 Google Test (`GTest::gtest`)。测试定义在组件级别（common/comm/、common/config/），通过 `gtest_discover_tests()` 发现。`third_party_probe` 目标验证所有第三方依赖正确链接。集成测试数据位于 `tests/resources/backend_integration/`。特定领域（sensor_fusion、risk_eval、path_plan、actuator_coord）的测试 proto 类型位于 `tests/resources/proto/`。
