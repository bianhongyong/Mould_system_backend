# 跨节点传输方案设计

## 背景

当前通信中间件仅支持单机多进程（SHM IPC），需要扩展为跨机器传输。技术选型为 gRPC。

## 核心矛盾

当前架构是**多进程模型**，每个模块是一个独立进程（通过 `fork()` 创建），由 Supervisor 管理。

```
backend_main (supervisor)
  ├── FrameSource       ← 独立进程
  ├── FeatureExtract    ← 独立进程
  ├── RiskEval          ← 独立进程
  └── ...
```

跨节点传输面临的问题：**多个进程共享一个网络端口**是无法绕开的架构约束。

---

## 方案对比

### 方案一：纯 gRPC（每进程独立端口）

```
Node A                              Node B
FrameSource:50051 ──gRPC──▶ FeatureExtract:50051
                    └───▶ RiskEval:50052
                          PathPlan:50053
```

**工作方式：** 每个模块进程启动自己的 gRPC Server，消费者主动连接生产者，通过双向流订阅感兴趣的通道。模块直接调用 `GrpcDataPlane::Publish()` 序列化并通过 gRPC 发送。

| 项目 | 说明 |
|------|------|
| 实现复杂度 | **低**。只需实现 `GrpcDataPlane`（实现 `IPubSubBus`），新增 proto 定义 |
| 代码量 | 估算 ~600 行（不含 gRPC 模板代码） |
| 数据路径 | `publish → serialize → gRPC → deserialize → handler` |
| 延迟 | 一次网络 RTT |
| 模块修改 | 零修改（通过 `IPubSubBus` 接口） |

**瓶颈：**

```
Node B 暴露的端口数 = 模块数
控制面需要管理 (模块数 × 目标机器数) 条连接
Node A 需要维护到每个远程模块的独立 gRPC 连接
```

- 每节点 N 个模块 → 每节点 N 个端口 → 全局 M 个节点 → M×N 个端口，端口爆炸
- 每个模块的配置中要写明远程每个模块的地址和端口，拓扑信息高度耦合
- 控制面需要管理 N×M 条连接的生命周期

---

### 方案二：纯 gRPC + SO_REUSEPORT（共享端口）

```
Node B:50051（SO_REUSEPORT）
  ┌─────────────────────────────┐
  │ FeatureExtract (进程)         │  内核随机分发连接
  │ RiskEval (进程)               │
  │ PathPlan (进程)               │
  └─────────────────────────────┘
```

**问题：** 内核在 accept 时随机将连接分配给某个进程，无法控制 "这条消息给 FeatureExtract"。SO_REUSEPORT 做的是**无状态负载均衡**，不是**通道路由**。此方案**不适用**。

---

### 方案三：gRPC + SHM + Gateway 代理进程（推荐）

引入一个独立的 **Gateway** 子进程，每节点唯一持有 gRPC 端口，作为"跨节点数据守门人"：

```
Node A                              Node B
┌──────────────────────┐          ┌──────────────────────┐
│ FrameSource          │          │ Gateway:50051         │
│  publish()           │          │  ┌────────────────┐  │
│   ↓                  │          │  │ gRPC Server     │  │
│  写入本地 SHM        │          │  │  deserialize    │  │
│   ↓                  │          │  │  写入本地 SHM   │  │
│  ┌──────────┐       │          │  └──────┬─────────┘  │
│  │ Gateway  │◀──SHM─┼──────────┼─▶SHM    │ SHM写入     │
│  │ :50051   │       │          │    ↓    ↓             │
│  └──────────┘       │          │  ┌────────────────┐  │
│                     │          │  │ FeatureExtract │  │
│                     │          │  │ 从 SHM 读取     │  │
│                     │          │  │ RiskEval       │  │
│                     │          │  │ 从 SHM 读取     │  │
│                     │          │  └────────────────┘  │
└──────────────────────┘          └──────────────────────┘
```

**Gateway 的角色：**

- **出站方向：** 监听本地 SHM 段，发现有新数据 → 查拓扑判断是否有远程消费者 → gRPC 推送给目标节点的 Gateway
- **入站方向：** gRPC 收到远程消息 → 反序列化 → 写入本地 SHM → 同节点的模块进程按原有方式消费
- **控制面：** 节点发现、拓扑同步、健康检查

| 项目 | 说明 |
|------|------|
| 实现复杂度 | **中高**。需实现 Gateway + SHM 接入 + gRPC |
| 端口数 | **每节点 1 个**（仅 Gateway 持有） |
| 连接数 | 全局 M 个节点，共 M×(M-1)/2 条 gRPC 连接 |
| 模块修改 | **零修改**（模块完全不感知跨节点） |
| 数据路径 | `publish → SHM 写 → SHM 读 → gRPC serialize → gRPC → deserialize → SHM 写 → SHM 读 → handler` |
| 延迟 | 两次 SHM memcpy + 一次网络 RTT + 一次序列化 |

**控制面拓扑：**

```
backend_main (supervisor)
  ├── Gateway:50051              ← 唯一持有网络端口的进程
  ├── FrameSource                ← 纯本地，无网络
  ├── FeatureExtract             ← 纯本地，无网络
  ├── RiskEval                   ← 纯本地，无网络
  └── ...
```

---

## 同节点多消费者场景对比

这是区分两种方案的关键场景：

```
Node B 上 FeatureExtract 和 RiskEval 都订阅了 "frame.raw"

方案一（纯 gRPC 每进程独立端口）:
  Node A ──gRPC──▶ FeatureExtract   ← 一份数据，一次网络传输
         └──gRPC──▶ RiskEval        ← 同一份数据，第二次网络传输

方案三（Gateway + SHM）:
  Node A ──gRPC──▶ Gateway          ← 一份数据，一次网络传输
                    │
                    ├── SHM ──▶ FeatureExtract
                    └── SHM ──▶ RiskEval   ← 本地内存拷贝，无网络
```

| 场景 | 纯 gRPC | gRPC + SHM |
|------|---------|------------|
| 1 对 1 | 1 次网络传输 | 1 次网络传输 + 2 次 memcpy |
| 1 对 2（同节点） | **2** 次网络传输 | 1 次网络传输 + **3** 次 memcpy |
| 1 对 N（同节点） | **N** 次网络传输 | 1 次网络传输 + **(N+1)** 次 memcpy |

纯 gRPC 在同节点多消费者场景下浪费网络带宽，但 memcpy 的代价和序列化的代价可以部分抵消。

---

## 部署模式权衡

**什么情况下纯 gRPC 够用：**

- 每个节点上每类模块只有**1 个实例**
- 模块间的通信是严格的 1:1 模式
- 网络带宽充足，延迟要求不苛刻

**什么情况下 Gateway + SHM 是必要的：**

- 同一节点上多个模块消费同一通道
- 节点数量多（N > 3 时连接数膨胀明显）
- 模块进程的 crash 隔离性必须保持（不能改成多线程）

---

## 最终建议

当前项目属于**工业缺陷检测**场景，模块较多（FrameSource、FeatureExtract、RiskEval、SensorFusion、PathPlan、ActuatorCoord...），典型的拓扑是多个消费者订阅同一通道，**Gateway + SHM 方案更适配合现有架构**。

纯 gRPC 方案虽然实现更简单，但在多端口管理、连接数膨胀方面与现有多进程架构之间有根本性的矛盾，不做架构大改的情况下不可行。

建议优先级：
1. **确定 Gateway + SHM 方案**
2. 设计 gRPC proto 和 Gateway 组件接口
3. 扩展 `launch_plan.json` 配置模型，增加集群拓扑
4. 实现 Gateway 进程（控制面 + 数据面）
5. 现有模块零修改接入
