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

测试标签: `unit`, `shm_segment`, `shm_ring_buffer`, `shm_pubsub`, `channel_topology_config`, `launch_plan_config`, `multiprocess`, `module_base`, `supervisor`, `module_factory_registry`, `sync`, `backpressure`, `control_plane`, `membership`, `proto`, `smoke`

## 代码库架构

### 概述

工业缺陷检测（覆膜检测）后端系统。采用多进程架构，**主进程** (`backend_main`) fork 并监管通过 POSIX 共享内存 IPC 通信的**子模块进程**。

### 关键目录

- `common/comm/` — 通信中间件：共享内存 pub/sub 总线、模块基类、监管器
- `common/config/` — 配置解析器：启动配置 (JSON)、通道拓扑 (txt)
- `common/include/` — 通用工具：日志、测试辅助
- `common/proto/` — IPC 消息的 Protobuf 定义
- `services/backend/` — 主进程入口点和测试模块
- `gate/` — HTTP 网关：`module/`（模块实现）、`gflags/`（模块 gflags）、`handler/`（请求处理器）、`http/`（HTTP 服务器）、`oss/`（OSS 客户端）
- `logging/` — 日志管线：`module/`（日志模块实现）、`gflags/`（模块 gflags）、`src/`（日志基础设施）、`include/`（日志接口）
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

### 配置原则

**业务模块配置必须通过 GFLAGS + launch_plan.json module_params 传递，禁止使用环境变量。**

- `launch_plan.json` 中每个模块的 `module_params` 字段存放业务参数
- `main.cpp` 通过 `ApplyLaunchPlanScalarsToMatchingRegisteredGflags()` 将 `module_params` 键值对注入匹配的 `FLAGS_*`
- 模块代码直接读取 `FLAGS_*`（gflags），不得调用 `std::getenv`
- 基础设施/系统层参数（如 `MOULD_MODULE_CHANNEL_CONFIGS`、`MOULD_SHM_SLOT_COUNT`）仍可使用环境变量，业务配置一律走 gflags

### 错误处理（禁止 try/catch）

**本仓库自研业务代码禁止使用 `try` / `catch` 处理错误**（新增与重构均应遵守）。

- **约定**：用返回值表达成败与原因，并优先采用 **Boost** 惯用错误类型（例如 `boost::system::error_code` 作为返回值或传出参数，或与所用 Boost 组件配套的 `error_code` 约定），由调用方分支处理，不依赖异常控制流。
- **日志**：进入失败分支时必须 **打印日志**（使用本仓库既有 `LOG` / `VLOG` 等宏），携带足够上下文，禁止静默失败。
- **测试**：对错误路径的断言应基于返回值或错误码；不因本规范而要求对第三方库行为使用 `EXPECT_THROW` 等例外断言，但自研接口不应以抛异常为契约。

### 构建规范：GFLAGS 编译规则

**模块的 `DEFINE_*` / `DECLARE_*` 必须编译进该模块所属的静态库或动态库，不得直接列在 `add_executable(backend_main ...)` 的源文件中。**

- 每个业务模块的 gflags 源码（如 `gateway_module_gflags.cpp`）放在模块目录下的 `gflags/` 子目录中
- 在模块的 CMakeLists.txt 中通过 `target_sources` 或 `add_library` 加入该文件
- 测试模块的 gflags 可直接定义在测试 `.cpp` 文件中，无需单独 gflags 源文件

### 测试基础设施

测试使用 Google Test (`GTest::gtest`)。测试定义在组件级别（common/comm/、common/config/），通过 `gtest_discover_tests()` 发现。集成测试数据位于 `tests/resources/backend_integration/`。特定领域（sensor_fusion、risk_eval、path_plan、actuator_coord）的测试 proto 类型位于 `tests/resources/proto/`。
