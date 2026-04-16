# 覆膜智能检测后端架构设计（单仓库 + CMake + 多进程）

## 1. 目标与约束

本文档用于指导“中间服务 + 推理服务”后端实现，满足以下约束：

- 中间服务与推理服务放在同一个代码仓库，统一版本管理。
- 全工程使用 CMake 构建。
- 模块按“进程”划分：一个进程负责一大块职责，独立运行。
- 每类服务都必须有主进程，负责监护和管理子进程。
- 当前阶段仅考虑中间服务与推理服务同机（单节点）部署。
- 同节点（同机）优先使用高性能 IPC（共享内存 / 管道 / Unix Domain Socket）。
- 程序架构需预留跨节点扩展接口（后续可接入 gRPC 通道）。
- 需要统一“通信中间件”接口，屏蔽底层通信细节。
- 推理请求必须最终完成：丢失可重试，重复可去重（工程上采用 At-least-once + 幂等）。
- 前端只负责可靠传输图片与图片元信息，不直接上传 OSS。
- 中间服务负责接收图片、内存缓存、向推理服务分发，并负责上传 OSS。

---

## 2. 单仓库目录建议

```text
mould_system/
├── CMakeLists.txt
├── cmake/
│   ├── third_party.cmake
│   └── platform.cmake
├── common/                          # 公共库
│   ├── proto/                       # protobuf 定义
│   ├── comm/                        # 统一通信中间件（抽象层 + 实现）
│   ├── config/                      # 配置加载
│   ├── logging/                     # 日志
│   ├── metrics/                     # 指标
│   ├── storage/                     # MySQL/Redis/OSS 客户端封装
│   └── reliability/                 # 幂等、重试、状态机、outbox
├── services/
│   ├── broker/                      # 中间服务
│   │   ├── master/                  # 主进程
│   │   ├── ingress_worker/          # 接入子进程
│   │   ├── scheduler_worker/        # 调度子进程
│   │   ├── dispatcher_worker/       # 派发子进程
│   │   └── status_worker/           # 状态查询/回调子进程
│   └── infer/                       # 推理服务
│       ├── master/                  # 主进程
│       ├── consumer_worker/         # 消费子进程
│       ├── executor_worker/         # 推理执行子进程
│       ├── result_worker/           # 结果提交子进程
│       └── heartbeat_worker/        # 心跳与负载上报子进程
├── deploy/
│   ├── configs/
│   ├── scripts/
│   └── systemd/
└── docs/
    └── backend_architecture.md
```

> 注：当前文档先放仓库根目录，后续可移动到 `docs/`。

---

## 3. 进程模型（主进程 + 子进程）

## 3.1 中间服务（Broker Service）

### 主进程职责（`broker_master`）

- 读取配置，初始化运行环境（日志、监控、配置中心连接）。
- 拉起并监护所有子进程（fork/exec 或 supervisor 模式）。
- 健康检查：周期探活、异常重启、重启熔断（防止抖动风暴）。
- 统一生命周期管理：启动顺序、优雅停机、滚动重启。
- 汇总子进程指标并上报（进程存活、请求速率、积压深度、错误率）。

### 子进程职责

1) `ingress_worker`（接入）
- 对外提供 gRPC/HTTP API（提交任务、查状态、取结果）。
- 鉴权、限流、参数校验、生成/校验 `request_id`。
- 接收前端上传的图片二进制与元信息（设备号、拍摄时间、业务标签等）。
- 将图片写入节点内存缓存（可基于共享内存段或内存对象池），生成 `image_ref`。
- 写入任务表（`PENDING`），并写 Outbox 事件（同事务）。
- 异步触发 OSS 上传流程，上传成功后回填 `image_uri`。

2) `scheduler_worker`（调度）
- 读取 Redis 负载信息（推理节点心跳、并发、队列积压）。
- 按策略选择目标节点/队列（模型、优先级、地域、版本）。
- 对任务状态执行 `PENDING -> QUEUED`。

3) `dispatcher_worker`（可靠派发）
- 扫描 Outbox 未投递事件并投递到消息通道。
- 投递成功后标记 Outbox 已发送，失败重试并记录退避策略。
- 负责消息路由键生成（例如 `model_x.high_priority`）。
- 同节点部署时优先投递 `image_ref`（共享内存引用）给推理服务，减少重复拷贝与网络传输。
- 当前版本固定使用同节点路径；跨节点派发接口仅保留扩展位（后续可回退 `image_uri` 或扩展大对象传输通道）。

4) `status_worker`（状态与回调）
- 提供任务状态查询和结果查询接口。
- 可选推送回调（WebHook/MQ 回执）给上游系统。
- 处理人工补偿操作（手动重试、冻结、取消）。

---

## 3.2 推理服务（Inference Service）

### 主进程职责（`infer_master`）

- 读取模型与资源配置，初始化 GPU/CPU 资源管理器。
- 拉起并监护推理侧子进程。
- 子进程异常隔离与重启（崩溃不影响整机可用）。
- 管理模型版本生命周期（预热、切换、回滚）。
- 汇总并上报节点负载到 Redis（供中间服务调度）。

### 子进程职责

1) `consumer_worker`（消息消费）
- 从消息系统拉取任务（手动 ACK 模式）。
- 做幂等预检查：若 `request_id` 已成功，直接 ACK 丢弃重复消息。
- 将可执行任务转发给 `executor_worker`。

2) `executor_worker`（执行推理）
- 同节点优先通过 `image_ref` 从共享内存读取图片（零拷贝/少拷贝路径）。
- 若共享内存不可用或数据已过期，回退从 OSS 拉取图片（超时、重试、校验）。
- 执行预处理 -> 模型推理 -> 后处理。
- 产生推理结果、置信度、缺陷框等结构化输出。

3) `result_worker`（结果提交）
- 事务写结果表，并更新任务状态 `RUNNING -> SUCCEEDED`。
- 若失败则写错误码并触发重试流程 `-> RETRYING`。
- 成功后通知 `consumer_worker` 进行 ACK。

4) `heartbeat_worker`（心跳负载）
- 周期上报：并发占用、GPU 利用率、平均耗时、失败率、队列长度。
- 维持节点在线状态 TTL，断心跳自动下线。

---

## 4. 统一通信中间件（Communication Middleware）

目标：封装模块间通信方式，向业务模块提供统一的事件循环、发布订阅与定时任务能力。当前落地同机通信，并为后续跨节点扩展预留接口。

## 4.1 模块基类（模板方法模式）

```cpp
class ModuleBase {
public:
  bool Start() final {
    if (!Init()) return false;
    if (!CreateEventLoop()) return false;                    // 基类创建主线程事件循环
    if (!RegisterPublications()) return false;
    if (!RegisterSubscriptions()) return false;
    if (!RegisterTimers()) return false;
    event_loop_->Run([this] { DrainCallbackQueue(); });      // 统一消费回调队列
    return OnStarted();
  }

  bool Stop() final {
    if (event_loop_) event_loop_->Stop();
    return OnStopping();
  }

  bool Subscribe(const std::string& input_channel,
                 MessageCallback cb); // 基类实现：注册输入通道回调
  bool RegisterPublishChannel(const std::string& output_channel); // 基类实现：注册可发布输出通道
  bool Publish(const std::string& output_channel,
               const google::protobuf::Message& msg); // 基类实现：仅允许发布到已注册输出通道
  void EnqueueCallback(CallbackTask task);            // 基类实现：将消息/定时器回调投递到回调队列
  TimerId SetTimer(std::chrono::milliseconds interval,
                   TimerCallback cb,
                   bool repeat = true); // 基类实现：注册定时器

protected:
  bool CreateEventLoop();                                       // 基类实现：构建事件循环实例
  void DrainCallbackQueue();                                    // 基类实现：按序执行回调队列
  virtual bool Init() = 0;                                   // 子类实现初始化
  virtual bool RegisterPublications() = 0;                   // 子类实现发布注册（调用基类 RegisterPublishChannel）
  virtual bool RegisterSubscriptions() = 0;                  // 子类实现订阅注册（调用基类 Subscribe）
  virtual bool RegisterTimers() { return true; }             // 子类按需注册定时器（调用基类 SetTimer）
  virtual bool OnStarted() { return true; }
  virtual bool OnStopping() { return true; }

private:
  std::unique_ptr<IEventLoop> event_loop_;
  CallbackQueue callback_queue_; // 无锁 MPSC 队列，统一承载消息/定时器回调任务
};
```

约束：
- 项目中每个“进程级模块”都必须继承 `ModuleBase`（如 `ingress_worker`、`dispatcher_worker`、`consumer_worker` 等）。
- 业务模块禁止直接操作底层 IPC 细节，只通过中间件暴露的发布/订阅接口通信。
- `ModuleBase` 必须在主线程持有事件循环，统一驱动消息回调与定时任务回调，避免业务模块自行创建分散事件循环。
- `ModuleBase` 必须维护统一回调队列；订阅消息与定时器触发都先入队，再由事件循环按序出队执行。

## 4.2 通信接口与职责分层

- `IEventLoop`：事件循环抽象接口，提供 `Run/Poll/Stop` 与定时器调度能力。
- `CallbackQueue`：回调任务队列，承载订阅回调与定时器回调，确保主线程串行执行。
- `IPubSubBus`：发布订阅抽象接口，提供 `Init/Subscribe/Publish`。
- `ShmPubSubBus`：当前主实现，基于共享内存完成高吞吐发布订阅。
- `GrpcPubSubBus`：跨节点预留实现（当前不启用）。
- `MessageCodec`：统一 protobuf 编解码，负责版本兼容和基础校验。
- `ChannelFactory`：按配置返回具体总线实现；当前固定返回 `ShmPubSubBus`，保留扩展位。
- `TimerScheduler`：基于事件循环提供一次性/周期性定时器能力，供 `ModuleBase::SetTimer` 复用。

## 4.3 发布订阅机制（共享内存输入/输出通道 + protobuf 协议）

- 每个模块在自身配置文件中声明输入通道（Input Channels）与输出通道（Output Channels）。
- 启动阶段必须先完成发布注册：模块通过 `RegisterPublishChannel` 声明允许写入的输出通道集合。
- 发布语义：模块只向自身配置的输出通道写入数据（`Publish -> output channel`）。
- 订阅语义：模块只从自身配置的输入通道读取数据（`Subscribe -> input channel`）。
- 通道名称按业务划分：如 `task.dispatch`、`infer.result`、`task.status`。
- 消息体统一使用 protobuf，消息头至少包含：`request_id`、`trace_id`、`topic`、`timestamp`、`schema_version`。
- `Publish`：先校验目标通道已注册，再将 protobuf 序列化后写入共享内存输出通道队列，并推进写游标。
- `Subscribe`：注册输入通道回调，由主线程事件循环拉取、反序列化并分发到模块处理函数。
- 可靠性要求：发布失败可重试，消费端必须幂等（同 `request_id` 重复消息可安全处理）。
- 配置示例（按模块拆分）：

```yaml
module: dispatcher_worker
channels:
  input:
    - task.dispatch
  output:
    - infer.request
    - task.status
```

## 4.4 共享内存机制设计（核心）

- **内存模型**：建议“共享内存段 + 通道环形队列”结构，队列槽位保存消息元数据和 payload 偏移。
- **配置驱动分配**：读取每个模块配置文件中的输入/输出通道、队列深度、单消息上限，并据此分配共享内存空间。
- **读写策略**：单通道可采用单写多读模型；跨进程使用原子序号推进读写游标，避免全局锁竞争。
- **容量治理**：按通道配置队列深度和单消息上限；超限时触发丢弃策略或回退磁盘/OSS 引用。
- **生命周期**：消息设置 TTL，过期自动回收；进程重启后可按游标恢复未消费数据窗口（按可用性策略配置）。
- **监控指标**：队列水位、发布失败数、反序列化失败数、消费延迟、过期丢弃数必须暴露到监控系统。

### 4.4.1 并发设计（避免冲突）

- **通道级隔离**：每个通道独立维护环形队列与游标，避免不同业务通道之间互相争锁。
- **写入互斥策略**：同一输出通道约束单写者；若必须多写者，使用 CAS 抢占写槽位并在失败后快速重试退避。
- **读取并发策略**：消费者维护独立读游标，按消费组/订阅者分片推进，避免读读冲突与慢消费者阻塞快消费者。
- **提交两阶段**：写入分“预留槽位 -> 拷贝 payload -> 原子提交可见”，消费者仅消费已提交槽位，避免半写数据被读取。
- **顺序保证**：通道内以递增序号作为唯一顺序基准；回调队列按序号入队，确保同通道消息处理顺序一致。
- **冲突可观测**：暴露 CAS 失败次数、重试次数、队列覆盖次数、慢消费者积压长度，用于提前发现并发热点。

## 4.5 定时器能力（由事件循环统一调度）

- 中间件提供统一 `SetTimer` 接口，支持单次触发与周期触发。
- 定时器回调与消息回调都在主线程事件循环内执行，保证模块内时序一致。
- 典型用途：心跳上报、超时扫描、轻量状态轮询、延迟重试触发。
- 定时器任务应短小且不可阻塞；重任务需投递到专用工作线程池。

## 4.6 当前范围与扩展预留

- 当前版本只落地 `ShmPubSubBus`，满足单节点中间服务与推理服务通信。
- `IPubSubBus`、`ChannelFactory`、消息头 `schema_version` 作为后续跨节点扩展保留点。
- 未来启用跨节点时，模块层代码不变，仅替换总线实现与部署配置。

---

## 5. 可靠性设计（请求最终完成）

## 5.1 核心机制

- 全局唯一 `request_id`（设备侧或中间服务生成）。
- MySQL 唯一键约束（`request_id`）保证幂等。
- Outbox 保证“写库成功 -> 必可投递”。
- 消费端手动 ACK：结果落库成功后再 ACK。
- 超时扫描器：扫描长时间 `QUEUED/RUNNING` 任务并补偿重试。
- 死信队列：超过阈值后隔离，避免毒消息阻塞主链路。

## 5.2 状态机

```text
PENDING -> QUEUED -> RUNNING -> SUCCEEDED
   |         |         |
   |         |         -> RETRYING -> QUEUED
   |         -> RETRYING -> QUEUED
   -> FAILED_FINAL
```

说明：
- 可重试错误（网络抖动、OSS 暂时不可用、共享内存读取失败、推理超时）进入 `RETRYING`。
- 不可重试错误（参数非法、图片损坏不可解析）进入 `FAILED_FINAL`。

## 5.3 重试策略

- 指数退避：`1s, 2s, 4s, 8s ...`（可加抖动）。
- 最大重试次数按业务优先级配置。
- 支持人工触发补偿重试。

---

## 6. 数据与消息契约（最小集合）

## 6.1 任务表（示意）

- `request_id`（唯一）
- `status`
- `model_id` / `model_version`
- `image_ref`（同节点共享内存引用，可为空）
- `image_uri`（OSS 地址，上传成功后回填）
- `image_meta_json`（图片元信息：设备、批次、时间戳等）
- `retry_count`
- `error_code` / `error_message`
- `created_at` / `updated_at`

## 6.2 结果表（示意）

- `request_id`
- `defect_type`
- `score`
- `bbox_json`
- `raw_output_uri`
- `finished_at`

## 6.3 消息体（示意字段）

- `request_id`
- `trace_id`
- `image_ref`（优先，同节点）
- `image_uri`（兜底，当前用于共享内存失效回退；后续可用于跨节点回源）
- `image_meta_json`
- `model_id`
- `priority`
- `deadline_ms`
- `retry_count`

---

## 7. 部署拓扑

## 7.1 单机部署（中间 + 推理同机）

- 节点内大量通信走 IPC（共享内存 + UDS）。
- 图片接入后直接缓存到共享内存，推理优先读取共享内存引用。
- OSS 上传由中间服务异步执行，不阻塞同机推理主路径。
- 对外业务 API 可继续使用 gRPC/HTTP（与内部部署拓扑解耦）。
- 适合 PoC 和小规模产线。

## 7.2 跨机扩展预留（后续阶段）

- 当前版本不落地跨机部署，仅保留接口与配置扩展点。
- `IChannel` + `ChannelFactory` 保持拓扑无关，后续可切换到 `GrpcChannel`。
- 消息体继续保留 `image_uri` 字段，便于后续跨机场景直接回源 OSS。
- 调度层预留“目标节点选择”与“跨节点路由键”策略接口。

---

## 8. CMake 构建与编译架构（性能与依赖）

## 8.1 构建分层架构

```text
Root CMakeLists.txt
├── cmake/platform.cmake         # 编译器、平台、LTO、IPO、警告级别
├── cmake/third_party.cmake      # 第三方依赖导入与版本检查
├── common/CMakeLists.txt        # 公共库（comm/storage/reliability/...）
├── services/broker/CMakeLists.txt
│   ├── broker_master
│   ├── ingress_worker
│   ├── scheduler_worker
│   ├── dispatcher_worker
│   └── status_worker
└── services/infer/CMakeLists.txt
    ├── infer_master
    ├── consumer_worker
    ├── executor_worker
    ├── result_worker
    └── heartbeat_worker
```

原则：
- 统一工具链：全项目锁定编译器版本、C++ 标准、构建类型。
- 统一依赖入口：所有三方库在 `cmake/third_party.cmake` 导入，禁止子模块重复 `find_package`。
- 统一目标属性：通过 `INTERFACE` 库传播 include、compile definition、link flags。

## 8.2 编译性能策略（构建速度 + 运行性能）

- 构建速度：
  - 启用 Ninja 生成器（推荐）。
  - 启用 `ccache`（如可用）。
  - 使用预编译头（PCH）降低重复头文件编译成本。
  - 拆分目标：按进程产物拆 target，避免无关模块全量重编。
- 运行性能：
  - `Release`：`-O3 -DNDEBUG`，可选 `-flto`。
  - `RelWithDebInfo`：`-O2 -g`（线上排障建议）。
  - 链接器优化：`gold` 或 `lld`（视环境可用性）。
- 可靠构建：
  - 统一 `-Wall -Wextra -Werror`（可按模块放宽）。
  - CI 强制执行 `cmake --build` + `ctest`。

## 8.3 模块编译依赖关系（建议）

- `common_proto`：由 `.proto` 生成，当前提供 `protobuf::libprotobuf` 相关代码，并预留 gRPC 代码生成扩展。
- `common_comm`：当前依赖 `common_proto`、`absl::*`、`Boost::*`，并预留 `gRPC::grpc++` 接入位。
- `common_storage`：依赖 `mysqlclient`、`redis` 客户端、`oss sdk`。
- `common_reliability`：依赖 `common_storage`、`rabbitmq-c`（或其他 MQ 客户端）。
- `broker_*` 子进程：依赖 `common_comm/common_storage/common_reliability`。
- `infer_*` 子进程：依赖 `common_comm/common_storage/common_reliability` + 推理运行时库。

## 8.4 第三方库导入清单与 CMake 检查点

需确认可导入库：
- OSS SDK
- MySQL Client
- RabbitMQ C Client
- Boost
- Abseil（absl）
- gRPC（可选，跨节点扩展时启用）
- Protobuf

建议在 `cmake/third_party.cmake` 统一处理（示意）：

```cmake
find_package(Protobuf REQUIRED)
# 可选：跨节点扩展启用时打开
# find_package(gRPC REQUIRED)
find_package(absl REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem system thread)
find_package(unofficial-rabbitmq-c REQUIRED)  # 或 pkg-config: rabbitmq
find_package(MySQL REQUIRED)                   # 或 FindMySQL.cmake / pkg-config
find_package(alibabacloud-oss-cpp-sdk REQUIRED) # 视安装方式调整包名
```

若某些库没有官方 `Config.cmake`，采用两种兜底方案：
- 使用 `pkg_check_modules(...)`（pkg-config）。
- 自定义 `Find<Lib>.cmake` 放在 `cmake/modules/`。

## 8.5 第三方库“是否能正常导入”的确认方法

为避免“配置成功但链接失败”，建议三层验证：

1) **配置期验证**（CMake configure）
- 在 configure 阶段打印版本与路径：
  - `message(STATUS "Protobuf: ${Protobuf_VERSION}")`
  - `message(STATUS "gRPC: ${gRPC_VERSION}")`（仅启用 gRPC 时）

2) **编译期验证**（最小可执行目标）
- 增加 `third_party_probe` 可执行程序，最小化包含各库头文件并调用一个符号。
- 将其接入默认构建，确保链接真实生效。

3) **运行期验证**（可选）
- 在 CI 运行 `third_party_probe --self-test`，确认动态库可加载。

通过标准：
- `cmake -S . -B build` 无缺失依赖报错。
- `cmake --build build -j` 成功产出所有目标。
- `ctest --test-dir build` 通过，至少包含 `third_party_probe`。

## 8.6 CMake 最小工程建议目标

- `third_party_probe`：三方库导入验证目标（必须）。
- `proto_codegen`：显式 protobuf/grpc 代码生成目标。
- `broker_all`：聚合构建中间服务全部可执行文件。
- `infer_all`：聚合构建推理服务全部可执行文件。
- `backend_all`：总聚合目标，供 CI 一键构建。

---

## 9. 可观测与运维要求

- 日志：结构化日志（JSON），全链路携带 `trace_id`、`request_id`。
- 指标：QPS、延迟分位、重试率、超时率、死信数量、节点负载。
- 链路追踪：接入 OpenTelemetry（可选）。
- 告警：任务积压、重试飙升、节点离线、模型推理失败率升高。

---

## 10. 推荐实施顺序

1. **先搭建 CMake 编译工程**：建立根 `CMakeLists.txt`、`third_party.cmake`、`platform.cmake`、`third_party_probe`。
2. **确认第三方库可导入**：逐项验证 OSS SDK、MySQL、RabbitMQ、Boost、absl、protobuf（gRPC 作为可选预留依赖）可配置/可编译/可链接。
3. 实现统一通信中间件抽象层与 IPC 通道，并预留 `GrpcChannel` 接口实现位。
4. 打通中间服务主链路：接入图片与元信息 -> 内存缓存 -> 任务落库 -> outbox -> 派发。
5. 增加中间服务 OSS 异步上传链路：上传、回填 `image_uri`、失败重试与告警。
6. 打通推理服务主链路：消费 -> 同节点读共享内存（失败回退 OSS）-> 推理 -> 结果落库 -> ACK。
7. 增加重试扫描器、死信队列、人工补偿接口。
8. 增加主进程监护、灰度发布、完整监控告警。

---

## 11. 结论

本设计以“主进程监护 + 子进程解耦 + 统一通信中间件 + 状态机补偿”为核心，当前聚焦同机单节点落地。前端仅承担可靠上送职责，由中间服务统一处理图片缓存与 OSS 上传；同节点优先共享内存直达推理链路，并在接口层预留跨节点扩展能力。整体通过“持久化 + 幂等 + 重试”实现请求最终完成，适合工业现场长期运行，并可随设备规模线性扩展。
