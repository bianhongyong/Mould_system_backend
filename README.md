# Mould System Backend

工业缺陷检测（覆膜检测）后端系统。

## 架构概述

采用多进程架构，主进程 `backend_main` fork 并监管多个业务子模块进程，子进程之间通过 POSIX 共享内存 IPC 通信。

```
backend_main (主进程)
  ├── supervisor: fork/exec、健康检查、带退避/熔断的重启
  ├── control plane: 创建共享内存段、管理消费者生命周期
  ├── config: 解析 launch_plan.json → 解析模块配置 → 设置环境变量
  ├── ready-pipe 协议: 等待子进程 READY 信号
  └── 子进程 (模块):
       ├── FrameSourceModule      — 发布帧（测试模块）
       ├── FeatureExtractModule   — 消费帧、提取特征（测试模块）
       ├── RiskEvalModule         — 风险评估（测试模块）
       ├── SensorFusionModule     — 传感器融合（测试模块）
       ├── PathPlanModule         — 路径规划（测试模块）
       ├── ActuatorCoordModule    — 执行器协调（测试模块）
       └── ... (均继承 ModuleBase)
```

## 核心特性

- **共享内存 pub/sub 总线**: 高性能进程间通信，支持 broadcast（广播）和 compete（抢占消费）两种投递模式
- **模块化架构**: 业务逻辑以独立进程运行，通过 `REGISTER_MOULD_MODULE_AS` 宏注册，由 `ModuleFactoryRegistry` 自动发现
- **主进程监管**: Supervisor 负责子进程 fork/exec、健康检查、带退避与熔断的自动重启
- **通道拓扑配置**: 每个模块以 `.txt` 文件声明 I/O 通道，系统自动聚合验证（无重复生产者、无参数冲突）并驱动共享内存段大小计算
- **无锁环形缓冲区**: 单生产者、多消费者，两阶段提交（预留 → 拷贝 → 提交），每个消费者拥有独立游标
- **启动计划配置**: `launch_plan.json` 指定模块列表、资源分配（CPU 亲和性、优先级、重启策略）和通道配置路径

## 目录结构

| 目录 | 说明 |
|------|------|
| `common/comm/` | 通信中间件：共享内存 pub/sub 总线、模块基类、监管器 |
| `common/config/` | 配置解析器：启动配置 (JSON)、通道拓扑 (txt) |
| `common/include/` | 通用工具：日志、测试辅助 |
| `common/proto/` | IPC 消息的 Protobuf 定义 |
| `services/backend/` | 主进程入口点和模块实现 |
| `tests/` | 集成测试数据和 proto 定义 |
| `cmake/` | 构建配置：平台标志、第三方依赖解析 |
| `docs/` | 架构文档、构建说明 |

## 配置流程

1. `launch_plan.json` 指定模块、资源和通道配置路径
2. `ParseLaunchPlanFile` 解析并验证已注册模块
3. `SetupRuntimeEnvironmentFromLaunchPlan` 设置环境变量
4. `ShmBusControlPlane::ProvisionChannelTopologyFromModuleConfigs` 创建 SHM 段
5. Supervisor 按启动优先级批次 fork 子进程

## 构建与测试

依赖: protobuf, gRPC, abseil, gflags, glog, Boost, OpenCV, ONNX Runtime, MySQL 客户端, RabbitMQ C 客户端, OSS SDK, moduo

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
```

## 文档

- [通道拓扑运行时配置](docs/channel_topology_runtime.md) — 共享内存通道拓扑的配置说明与校验规则
