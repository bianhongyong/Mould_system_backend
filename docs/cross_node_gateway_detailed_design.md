# 跨节点通信方案三详细设计：Gateway + SHM

## 1. 架构总览

### 核心理念

引入**Gateway 子进程**，每节点唯一持有 gRPC 端口，作为"跨节点数据守门人"。模块进程完全保持零修改，继续使用 `ShmBusRuntime` 进行本地 SHM 通信。Gateway 在 SHM 和 gRPC 之间桥接数据。

```
Node A                              Node B
┌──────────────────────┐          ┌──────────────────────┐
│ backend_main         │          │ backend_main         │
│  ├── Gateway:50051   │◀══gRPC══▶│  ├── Gateway:50051   │
│  │   ↕ SHM           │          │  │   ↕ SHM           │
│  ├── FrameSource     │          │  ├── FeatureExtract  │
│  └── SensorFusion    │          │  └── RiskEval        │
└──────────────────────┘          └──────────────────────┘
```

### 数据流

**出站（Node A → Node B）：**

```
FrameSource.publish("frame.raw", payload)
  → SHM RingBuffer_L (写入)
  → Gateway 收到 eventfd 通知
  → Gateway 从 SHM 读取 MessageEnvelope
  → Gateway 查拓扑: 此通道有 Node B 消费者
  → Gateway 序列化 → gRPC 流写入
  → ... 网络传输 ...
  → Node B Gateway 收到 gRPC 消息
  → Node B Gateway 反序列化 → SHM RingBuffer_R (写入)
  → FeatureExtract 收到 eventfd 通知
  → FeatureExtract 从 SHM 读取 → handler
```

**入站（Node B → Node A）：** 对称的逆向流程。

### 设计原则

| 原则 | 说明 |
|------|------|
| **模块零侵入** | 现有 `ModuleBase` / `ShmBusRuntime` 代码完全不修改 |
| **SHM 复用** | Gateway 复用 `ShmBusRuntime` 做本地数据面的订阅和发布 |
| **端口归一** | 每节点仅 Gateway 一个进程监听 gRPC 端口 |
| **拓扑驱动** | 路由决策完全基于 `ChannelTopologyIndex` + 节点映射 |

---

## 2. 组件设计

### 2.1 整体类关系

```
┌────────────────────────────────────────────────────────────┐
│                    GatewayModule                            │
│  (继承 ModuleBase)                                         │
│                                                            │
│  组合关系:                                                 │
│  ├── local_shm_bus_: ShmBusRuntime   (本地 SHM 数据面)     │
│  ├── data_plane_: GrpcCrossNodeDataPlane  (网络数据面)     │
│  └── control_plane_: GrpcBusControlPlane  (控制面)         │
│                                                            │
│  生命周期:                                                 │
│    DoInit() → 初始化控制面 + gRPC Server + 连接 Peer       │
│    SetupSubscriptions() → 订阅有远程消费者的通道           │
│    OnRunIteration() → 维护连接/心跳                        │
└────────────────────────────────────────────────────────────┘
         │                        │
         ▼                        ▼
┌──────────────────┐    ┌──────────────────────────┐
│  ShmBusRuntime   │    │  GrpcCrossNodeDataPlane  │
│  (已有, 不修改)   │    │  (新增)                  │
│                  │    │                          │
│ - 订阅本地 SHM    │    │ - gRPC Server            │
│ - 发布到本地 SHM  │    │ - gRPC Client (双向流)   │
│ - eventfd/epoll  │    │ - Protobuf 序列化         │
└──────────────────┘    └──────────────────────────┘
                                │
                                ▼
                      ┌──────────────────┐
                      │  GrpcBusControl  │
                      │  Plane (新增)     │
                      │                  │
                      │ - 集群拓扑管理    │
                      │ - 连接生命周期    │
                      │ - 通道路由计算    │
                      │ - 心跳/健康检查   │
                      └──────────────────┘
```

### 2.2 GrpcCrossNodeDataPlane — 跨节点网络数据面

负责 gRPC 连接管理和数据传输。

```cpp
// grpc_cross_node_data_plane.hpp
namespace mould::comm {

/// 跨节点 gRPC 数据面。
/// 封装了 gRPC Server 和到各 Peer Gateway 的 Client 双向流。
class GrpcCrossNodeDataPlane {
 public:
  using InboundMessageHandler = std::function<void(const MessageEnvelope&)>;

  GrpcCrossNodeDataPlane();
  ~GrpcCrossNodeDataPlane();

  // ========= 服务端 =========

  /// 启动 gRPC Server, 监听 host:port
  /// 必须在所有连接 Peer 之前调用
  bool StartServer(const std::string& host, std::uint16_t port);

  /// 注册入站消息回调: 收到远程消息后调用此回调
  /// 由 GatewayModule 注入: 将消息写入本地 SHM
  void SetInboundHandler(InboundMessageHandler handler);

  // ========= 客户端 =========

  /// 连接到 Peer Gateway, 建立双向流
  /// node_id: 远程节点标识, host_port: "ip:port"
  bool ConnectToPeer(const std::string& node_id, const std::string& host_port);

  /// 向指定的远程节点发送数据消息
  bool SendToPeer(const std::string& node_id, const DataMessage& msg);

  /// 向所有订阅了指定通道的远程节点广播
  /// channels_in_node: 各节点订阅的通道列表, 由控制面计算
  bool BroadcastToPeers(
      const std::string& channel,
      const DataMessage& msg,
      const std::unordered_map<std::string, std::unordered_set<std::string>>& channels_per_node);

  /// 向远程节点发送订阅命令
  bool SendSubscribe(const std::string& node_id, const SubscribeRequest& req);

  /// 断开并清理
  void Shutdown();

 private:
  /// 单个 Peer 的连接状态
  struct PeerConnection {
    std::string node_id;
    std::string host_port;
    std::unique_ptr<CrossNodeGateway::Stub> stub;
    std::unique_ptr<grpc::ClientReaderWriter<GatewayMessage, GatewayMessage>> stream;
    std::mutex write_mutex;                // 串行化 gRPC 流写入
    std::atomic<bool> connected{false};
    std::thread read_thread;                // 读取远程消息的线程
  };

  std::unique_ptr<grpc::Server> server_;
  std::unordered_map<std::string, std::shared_ptr<PeerConnection>> peers_;
  std::mutex peers_mutex_;

  InboundMessageHandler inbound_handler_;

  // 后台读线程: 从 Peer 流中读取消息
  void PeerReadLoop(std::shared_ptr<PeerConnection> peer);
};

}  // namespace mould::comm
```

### 2.3 GrpcBusControlPlane — 跨节点控制面

负责集群拓扑解析、通道路由计算、连接生命周期管理。

```cpp
// grpc_bus_control_plane.hpp
namespace mould::comm {

/// 跨节点控制面。
/// 负责集群拓扑管理、通道→节点路由计算、连接生命周期的维护。
class GrpcBusControlPlane {
 public:
  GrpcBusControlPlane();

  /// 从 ParsedLaunchPlan 初始化集群配置
  /// 包括: 本地节点 ID, 所有节点列表, 模块→节点映射
  bool InitFromLaunchPlan(const mould::config::ParsedLaunchPlan& plan);

  /// 初始化集群配置(直接传入)
  void InitCluster(mould::config::ClusterConfig cluster);

  // ========= 路由查询 =========

  /// 某通道是否有远程消费者
  bool HasRemoteConsumers(const std::string& channel) const;

  /// 获取某通道的所有远程消费者节点列表
  std::vector<std::string> GetRemoteConsumerNodes(const std::string& channel) const;

  /// 某通道的生产者是否在本地
  bool IsLocalProducer(const std::string& channel) const;

  /// 获取通道→订阅节点映射(用于 BroadcastToPeers)
  std::unordered_map<std::string, std::unordered_set<std::string>>
      BuildChannelToRemoteNodesMap() const;

  /// 本地节点 ID
  const std::string& LocalNodeId() const;

  // ========= 连接管理 =========

  /// 泵送连接维护: 应周期调用(在 GatewayModule::OnRunIteration 中)
  void PumpConnections(GrpcCrossNodeDataPlane* data_plane);

  /// 更新对端节点的订阅列表
  void UpdatePeerSubscriptions(
      GrpcCrossNodeDataPlane* data_plane,
      const std::string& peer_node_id,
      const std::unordered_set<std::string>& channels_to_subscribe);

  // ========= 拓扑 =========

  /// 全局通道拓扑索引
  const mould::config::ChannelTopologyIndex& GlobalTopology() const;

 private:
  // 节点配置
  mould::config::ClusterConfig cluster_config_;

  // 通道 → 消费者所在节点列表 (由 InitFromLaunchPlan 计算)
  std::unordered_map<std::string, std::vector<std::string>> channel_remote_consumers_;

  // 通道 → 生产者所在节点列表
  std::unordered_map<std::string, std::vector<std::string>> channel_producer_nodes_;

  // 对端节点 → 本端已向它订阅的通道列表
  std::unordered_map<std::string, std::unordered_set<std::string>> subscribed_channels_to_peer_;

  // 本端向对端发布的通道列表（即对端订阅了本端的哪些通道）
  std::unordered_map<std::string, std::unordered_set<std::string>> peer_subscribed_channels_;

  // 构建路由表
  void BuildRoutingTable(const mould::config::ParsedLaunchPlan& plan);
};

}  // namespace mould::comm
```

### 2.4 GatewayModule — Gateway 进程入口

继承 `ModuleBase`，组织数据面和控制面的协作。

```cpp
// gateway_module.hpp
namespace mould::comm {

class GatewayModule : public ModuleBase {
 public:
  GatewayModule(
      std::string module_name,
      std::shared_ptr<IPubSubBus> shm_bus,           // ShmBusRuntime
      std::unique_ptr<GrpcCrossNodeDataPlane> data_plane,
      std::unique_ptr<GrpcBusControlPlane> control_plane);

 protected:
  bool DoInit() override;
  bool SetupSubscriptions() override;
  void OnRunIteration() override;

 private:
  // 数据面
  std::shared_ptr<IPubSubBus> shm_bus_;               // 本地 SHM 总线
  std::unique_ptr<GrpcCrossNodeDataPlane> data_plane_; // 网络 gRPC 数据面

  // 控制面
  std::unique_ptr<GrpcBusControlPlane> control_plane_;// 控制面

  // 入站消息处理: gRPC 收到远程消息 → 写入本地 SHM
  void OnInboundRemoteMessage(const MessageEnvelope& envelope);

  // 出站消息处理: SHM 收到本地消息 → 转发到远程节点
  void OnLocalShmMessage(const MessageEnvelope& envelope);

  // gRPC 心跳定时器 ID
  TimerScheduler::TimerId heartbeat_timer_id_ = 0;
};

}  // namespace mould::comm
```

**DoInit 流程：**

```cpp
bool GatewayModule::DoInit() {
  // 1. 获取本机绑定地址 (从配置)
  auto& local_node = control_plane_->LocalNodeConfig();
  
  // 2. 启动 gRPC Server
  if (!data_plane_->StartServer(local_node.host, local_node.grpc_port)) {
    LOG(ERROR) << "Failed to start gRPC server";
    return false;
  }

  // 3. 注册远程消息入站回调
  data_plane_->SetInboundHandler(
      [this](const MessageEnvelope& env) { OnInboundRemoteMessage(env); });

  // 4. 连接到所有 Peer Gateway
  for (auto& peer : control_plane_->PeerNodes()) {
    if (!data_plane_->ConnectToPeer(peer.node_id, peer.HostPort())) {
      LOG(WARNING) << "Failed to connect to peer: " << peer.node_id;
      // 连接失败不阻塞, PumpConnections 会重试
    }
  }

  // 5. 注册心跳定时器
  heartbeat_timer_id_ = timer_scheduler_->RegisterPeriodic(
      std::chrono::seconds(5),
      [this]() {
        // 心跳通过 gRPC 流发送 (数据面内部处理)
      });

  return true;
}
```

**SetupSubscriptions 流程：**

```cpp
bool GatewayModule::SetupSubscriptions() {
  // 遍历全局拓扑, 对所有"本地生产 + 远程消费"的通道注册订阅
  for (const auto& [channel, entry] : control_plane_->GlobalTopology()) {
    if (control_plane_->IsLocalProducer(channel) &&
        control_plane_->HasRemoteConsumers(channel)) {
      SubscribeOneChannel(channel,
          [this](const MessageEnvelope& env) { OnLocalShmMessage(env); });
    }
  }

  // 同步订阅信息给 Peer
  for (auto& peer : control_plane_->PeerNodes()) {
    // 告诉对端: 本端要消费哪些由对端生产的通道
    auto channels_to_sub = control_plane_->GetChannelsToSubscribeFrom(peer.node_id);
    data_plane_->SendSubscribe(peer.node_id,
        SubscribeRequest{channels_to_sub});
  }

  return true;
}
```

**OnLocalShmMessage — 出站转发：**

```cpp
void GatewayModule::OnLocalShmMessage(const MessageEnvelope& envelope) {
  auto remote_nodes = control_plane_->GetRemoteConsumerNodes(envelope.channel);
  if (remote_nodes.empty()) return;

  DataMessage msg;
  msg.set_channel(envelope.channel);
  msg.set_publisher_module(envelope.publisher_module);
  msg.set_sequence(envelope.sequence);
  msg.set_payload(envelope.payload.data(), envelope.payload.size());

  auto channels_per_node = control_plane_->BuildChannelToRemoteNodesMap();
  data_plane_->BroadcastToPeers(envelope.channel, msg, channels_per_node);
}
```

**OnInboundRemoteMessage — 入站写入 SHM：**

```cpp
void GatewayModule::OnInboundRemoteMessage(const MessageEnvelope& envelope) {
  // 写入本地 SHM, 本节点消费者进程会收到通知
  shm_bus_->Publish("gateway", envelope.channel, envelope.payload);

  // 注意: publisher_module 保留原始模块名
  // 消费者可以通过 MessageEnvelope::publisher_module 知道消息来源
}
```

### 2.5 ClusterTopologyConfig — 集群配置

```cpp
// cluster_topology_config.hpp
namespace mould::config {

struct ClusterNodeConfig {
  std::string node_id;
  std::string host;
  std::uint16_t grpc_port = 50051;

  std::string HostPort() const {
    return host + ":" + std::to_string(grpc_port);
  }
};

struct ClusterConfig {
  std::string local_node_id;
  std::vector<ClusterNodeConfig> nodes;

  // 便利方法
  const ClusterNodeConfig* FindNode(const std::string& node_id) const;
};

/// 扩展 ParsedLaunchPlan, 增加集群信息
/// 在 ParseLaunchPlanFile 中解析 "cluster" 字段
struct ExtendedParsedLaunchPlan {
  ParsedLaunchPlan base;
  std::optional<ClusterConfig> cluster;
  // module → node_id 映射 (从 launch plan 中 modules[].node 提取)
  std::unordered_map<std::string, std::string> module_node_map;
};

bool ParseLaunchPlanFileExtended(
    const std::string& path,
    ExtendedParsedLaunchPlan* out_plan,
    std::string* out_error);

}  // namespace mould::config
```

---

## 3. gRPC Proto 定义

```protobuf
// cross_node_gateway.proto
syntax = "proto3";
package mould.comm.gateway;

// Gateway 间双向流服务
service CrossNodeGateway {
  // 每个 Peer 一对连接, 复用处理所有通道的数据和控制
  rpc GatewayStream(stream GatewayMessage) returns (stream GatewayMessage);
}

// 统一消息类型: 控制命令和数据复用同一条流
message GatewayMessage {
  oneof kind {
    // 控制面
    SubscribeRequest subscribe = 1;
    UnsubscribeRequest unsubscribe = 2;
    Heartbeat heartbeat = 3;

    // 数据面
    DataMessage data = 10;
  }

  uint64 sequence = 255;  // 消息序列号, 用于去重/排序
}

// 订阅请求: 通知对端, 本端要消费哪些通道
message SubscribeRequest {
  string node_id = 1;            // 本端节点 ID
  repeated string channels = 2;  // 要订阅的通道列表
}

// 退订请求
message UnsubscribeRequest {
  string node_id = 1;
  repeated string channels = 2;
}

// 心跳
message Heartbeat {
  int64 timestamp_ns = 1;
}

// 数据消息: 与 MessageEnvelope 字段对齐
message DataMessage {
  string channel = 1;             // 通道名, 如 "frame.raw"
  string publisher_module = 2;    // 发布模块名, 如 "FrameSource"
  uint64 sequence = 3;            // 序列号
  bytes payload = 4;              // 实际数据负载
  uint64 delivery_id = 5;         // 投递 ID, 用于去重
  int64 timestamp_ns = 6;         // 时间戳
}
```

### 流的复用策略

两个 Gateway 之间只有**一个双向流**，所有通道的数据和所有控制命令都复用到这个流上。这种设计避免了连接爆炸：

```
Node A Gateway ──── GatewayStream (单一 bidi 流) ──── Node B Gateway
                   ├── Subscribe("frame.raw")
                   ├── Data {channel: "frame.raw", ...}
                   ├── Data {channel: "sensor.data", ...}
                   ├── Heartbeat
                   ├── Unsubscribe("old.channel")
                   └── Data {channel: "result.data", ...}
```

---

## 4. Compete 和 Broadcast 的跨节点实现

### Broadcast

最简映射。Gateway 收到本地 SHM 消息后，向所有有消费者的远程节点发送：

```
Node A: FrameSource 发布 "frame.raw"
  → Gateway A 从 SHM 读出
  → 查拓扑: 消费者在 Node B, Node C
  → GatewayStream(A→B): Data{channel:"frame.raw"}
  → GatewayStream(A→C): Data{channel:"frame.raw"}
  → B Gateway 写入本地 SHM → B 上消费者竞争
  → C Gateway 写入本地 SHM → C 上消费者竞争
```

### Compete — 两级竞争

```
第一级: 节点级竞争 (Gateway 做, 无锁决策)
  Node A Gateway 从拓扑得知:
    通道 "task.assign" 是 compete 模式
    远程消费者分布在 Node B, Node C
  → 用一致性哈希选出一个目标节点 (本轮选 B)
  → 只发 GatewayStream(A→B), 不发 C

第二级: 进程级竞争 (已有 SHM CAS)
  Node B Gateway 收到后写入本地 SHM
  B 上多个消费者进程通过 TryClaimSlot() CAS 竞争
  最终只有胜出的进程执行 handler
```

节点选择策略：

```cpp
// GrpcBusControlPlane 中
std::string SelectCompeteTargetNode(
    const std::string& channel,
    std::uint64_t sequence) const
{
  auto nodes = GetRemoteConsumerNodes(channel);
  if (nodes.empty()) return "";
  if (nodes.size() == 1) return nodes[0];

  // 一致性哈希: 基于 channel + sequence 选择节点
  // 保证同一消息不会被发到多个节点
  std::hash<std::string> hasher;
  auto hash = hasher(channel + std::to_string(sequence));
  return nodes[hash % nodes.size()];
}
```

---

## 5. 配置模型扩展

**launch_plan.json** 增加 `cluster` 字段和模块的 `node` 字段：

```json
{
  "cluster": {
    "local_node_id": "node-a",
    "nodes": [
      { "node_id": "node-a", "host": "192.168.1.10", "grpc_port": 50051 },
      { "node_id": "node-b", "host": "192.168.1.11", "grpc_port": 50051 }
    ]
  },
  "modules": {
    "FrameSource": {
      "node": "node-a",
      "module_name": "FrameSource",
      "resource": {
        "startup_priority": 100,
        "cpu_set": "0-1"
      },
      "module_params": {},
      "io_channels_config_path": "channels/frame_source.json"
    },
    "FeatureExtract": {
      "node": "node-b",
      "module_name": "FeatureExtract",
      "resource": {
        "startup_priority": 90,
        "cpu_set": "0-3"
      },
      "module_params": {},
      "io_channels_config_path": "channels/feature_extract.json"
    },
    "RiskEval": {
      "node": "node-b",
      "module_name": "RiskEval",
      "resource": {
        "startup_priority": 80,
        "cpu_set": "4-7"
      },
      "module_params": {},
      "io_channels_config_path": "channels/risk_eval.json"
    }
  }
}
```

**通道拓扑文件不变**，仍描述逻辑生产者/消费者。节点映射在 `launch_plan.json` 层面完成。

---

## 6. 模块生命周期

### 启动流程

```
backend_main (Node A)
  │
  ├── 1. ParseLaunchPlanFileExtended → 解析集群配置 + 模块配置
  │      构建 global_topology_ + module_node_map
  │
  ├── 2. ShmBusControlPlane.ProvisionChannelTopology
  │      创建所有 SHM 段 (与纯本地模式一致)
  │      注意: 须为 Gateway 预留额外消费者槽位
  │      → 各通道 consumer_capacity += 1 (预留 Gateway 槽位)
  │
  ├── 3. SetupRuntimeEnvironmentFromLaunchPlan
  │      设置环境变量
  │
  ├── 4. Fork GatewayModule 子进程
  │      │
  │      ├── ShmBusRuntime (attach 所有 SHM 段)
  │      ├── GrpcCrossNodeDataPlane.StartServer
  │      ├── for each peer: data_plane_.ConnectToPeer
  │      ├── SetupSubscriptions → 订阅需要转发的通道
  │      ├── ReadyPipe Write → 通知 supervisor 就绪
  │      └── Run() → 进入主循环:
  │            OnRunIteration():
  │              control_plane_.PumpConnections(data_plane_)
  │              (重连断开的 Peer, 重新同步订阅)
  │
  ├── 5. Fork FrameSource 子进程
  │      (与纯本地模式完全一致, 不感知 Gateway 的存在)
  │
  ├── 6. Fork SensorFusion 子进程
  │      (同上)
  │
  └── 7. Supervisor 监控所有子进程
         Gateway 崩溃 → 重启, 重启后重连 Peer
         模块崩溃   → 按原有 restart policy 处理
```

### 崩溃恢复

| 场景 | 行为 |
|------|------|
| **Gateway 崩溃** | Supervisor 检测到退出 → 按 restart policy 重启 → 重启后重新 attach SHM → 重新连接 Peer → 重新同步订阅 |
| **模块进程崩溃** | 与纯本地模式完全一致。Gateway 已在 SHM 上订阅，自动收到后续消息 |
| **网络分区** | Gateway 检测到 gRPC 连接断开 → PumpConnections 退避重连 → 重连后重新同步订阅 → 消息可能丢失（按 at-most-once） |
| **远程 Gateway 崩溃** | 本端检测到流断开 → 标记为离线 → 暂停向该节点发送 → PumpConnections 重试连接 |

---

## 7. 文件清单

### 新增文件

| 文件 | 内容 | 估算行数 |
|------|------|----------|
| `common/proto/cross_node_gateway.proto` | gRPC 服务定义 | ~60 |
| `common/comm/include/grpc_cross_node_data_plane.hpp` | gRPC 数据面头文件 | ~100 |
| `common/comm/include/grpc_bus_control_plane.hpp` | 跨节点控制面头文件 | ~100 |
| `common/config/include/cluster_topology_config.hpp` | 集群拓扑配置头文件 | ~60 |
| `services/backend/gateway/gateway_module.hpp` | Gateway 模块头文件 | ~80 |
| 对应 .cpp 实现文件 x5 | | ~600 |

### 修改文件

| 文件 | 变更 |
|------|------|
| `common/config/include/launch_plan_config.hpp` | `ParsedLaunchPlan` 增加 `cluster` 字段、`module_node_map` |
| `common/config/src/launch_plan_config.cpp` | `ParseLaunchPlanFile` 扩展解析 `cluster` 和 `node` 字段 |
| `common/comm/include/channel_factory.hpp` | `BusKind` 增加 `kGateway` 枚举值 |
| `common/comm/src/channel_factory.cpp` | `ChannelFactory::Create` 增加 `kGateway` 分支 |
| `CMakeLists.txt` | 新增 gRPC proto 编译、Gateway 构建目标 |
| `services/backend/CMakeLists.txt` | 新增 Gateway 模块注册 |

### 无修改文件

| 文件 | 原因 |
|------|------|
| `module_base.hpp` | GatewayModule 继承自它, 不需要修改 |
| `shm_bus_runtime.hpp` | Gateway 作为客户端使用, 不需要修改 |
| `interfaces.hpp` | `IPubSubBus` 抽象不变 |
| `channel_topology_config.hpp` | 拓扑数据结构不变 |
| 所有业务模块代码 | 零修改 |

---

## 8. 与纯 gRPC 方案的关键差异总结

| 维度 | Gateway + SHM (方案三) | 纯 gRPC 每进程独立端口 (方案一) |
|------|----------------------|-------------------------------|
| 端口数/节点 | **1** (仅 Gateway) | N (模块数) |
| 连接数/节点 | M-1 (到各 Peer) | N × (M-1) |
| 模块进程修改 | **零修改** | 零修改 |
| 同节点多消费者 | 一次网络传输, SHM 多播 | N 次网络传输 |
| SHM 依赖 | **需要** (作为本地分发层) | **不需要** |
| 实现复杂度 | 中 (~860 行新增) | 低 (~600 行新增) |
| 数据路径长度 | SHM → gRPC → SHM (2 memcpy + 1 RTT) | 直接 gRPC (1 RTT) |
| 进程模型适配度 | **完全匹配** (fork + SHM) | 端口管理和连接管理复杂 |
