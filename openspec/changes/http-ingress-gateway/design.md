## Context

当前后端通过 POSIX 共享内存 pub/sub 总线进行进程间通信，模块以 `ModuleBase` 子类形式运行于 supervisor 监管下。Qt 采集节点需要将图片及元数据上报到推理流水线，但缺少 HTTP 入口。

网关是全新增模块，位于 `gate/` 目录，基于 muduo 提供 HTTP 服务、编排 SHM 发布与 OSS 持久化。muduo 仅作为外部动态库链接（BUILD-01），不编译随仓源码。

## Goals / Non-Goals

**Goals:**
- 提供基于 muduo 的 HTTP 接入服务，接收 multipart/form-data 格式的图片+元数据
- 编排校验→组包→SHM发布→异步OSS上传的流水线
- 作为 SHM 图片通道生产者，无缝融入现有 ChannelTopology/launch_plan 体系
- 在 OSS SDK 之上构建工程内封装层，编排层仅依赖窄接口
- 使用真实 OSS（mould-sys bucket）跑通上传全链路
- 支持 Fake/NoOp 后端用于 CI 和单元测试

**Non-Goals:**
- 不实现 OSS 补拉/回放恢复流程
- 不实现 gRPC 或其他接入协议
- 不实现接入鉴权
- 不实现独立持久化 Worker（使用进程内队列）
- 不修改现有 SHM pub/sub 运行时或 ModuleBase 基类

## Decisions

### D1: 分层架构 — 接入层 / 编排层 / 适配层

三层分离：接入层仅处理 HTTP/muduo 连接生命周期；编排层实现固定阶段流水线（解析→校验→组包→发SHM→登记持久化）；适配层封装 muduo、SHM、OSS 具体实现。

**理由**：符合需求规格第 6 节的架构原则，各层可独立测试和替换。接入层可替换为 gRPC 时不影响编排逻辑；OSS 封装层可替换 SDK 大版本时仅修改内部实现。

**备选方案**：将 HTTP 处理直接嵌入 ModuleBase::OnRunIteration 循环。被否决——muduo 自带事件循环，与 ModuleBase 的事件循环模型冲突，且违反分层原则。

### D2: 线程模型 — muduo I/O 线程 + 编排工作线程

muduo 事件循环运行在自己的 I/O 线程池中，HTTP 请求到达后，将解析后的请求体投递到编排工作线程。编排工作线程执行校验、组包、SHM Publish、发起异步 OSS 上传。HTTP 响应在编排工作线程完成后通过 muduo 的回调机制返回。

```
muduo I/O 线程池                    编排工作线程
     │                                  │
     ├── HTTP 请求到达                   │
     ├── 解析 multipart                  │
     ├── 投递到编排队列 ──────────────→  │
     │                              ├── 校验字段
     │                              ├── 组包 protobuf
     │                              ├── SHM Publish
     │                              ├── 发起 OSS 上传(异步)
     │   ← 返回 HTTP 200 ─────────  └── 回调 muduo context
```

**理由**：SHM Publish 允许在多线程环境下执行（FR-3.3-03）；将编排逻辑与 muduo I/O 解耦，避免阻塞事件循环。编排工作线程使用单一 `CallbackQueue` 或 `muduo::BlockingQueue`，统一跨线程投递机制（ARCH-02）。

**备选方案**：在 muduo I/O 线程直接执行编排和 SHM Publish。被否决——可能阻塞事件循环影响并发能力，且违反分层原则。

### D3: ModuleBase 集成 — 网关作为独立 ModuleBase 子类

网关模块继承 `ModuleBase`，在 `DoInit()` 中启动 muduo HTTP 服务器，在 `SetupSubscriptions()` 中无需订阅（网关只生产不消费），`OnRunIteration()` 作为轻量心跳循环。

muduo 事件循环与 ModuleBase 主循环通过 `muduo::EventLoopThread` 解耦：muduo 在自己的线程中运行，ModuleBase 主线程仅做健康检查和指标上报。

**理由**：复用现有 supervisor 监管、launch_plan 配置、CPU 亲和性设置等基础设施。网关不是传统「消费→处理→发布」模块，但 ModuleBase 的模板方法足够灵活容纳此模式。

### D4: OSS 封装层 — IBlobStorageUploader 接口 + OssClientFacade 实现

在 `gate/oss/` 下创建封装层：
- `IBlobStorageUploader`：窄接口，暴露 `UploadAsync(image_data, metadata, callback)` 方法
- `OssClientFacade`：基于阿里云 OSS SDK 的实现，封装凭证加载、Client 生命周期、PutObject、超时、错误码映射、重试退避
- `NoOpUploader`：空实现，用于 CI/单元测试
- `LocalFileUploader`：写到本地文件系统，用于无网开发环境

编排层和业务代码仅 `#include` 封装接口，不直接使用 OSS SDK 头文件（FR-3.4-05）。

**理由**：外观模式隔离第三方 SDK 变更风险；窄接口便于 Fake/Mock 替换；符合 FR-3.4-04 和 FR-3.4-05 要求。

### D5: 配置方式 — JSON 配置文件 + 环境变量覆盖

网关模块配置分为两部分：
- `launch_plan.json`：进程级配置（CPU 亲和性、重启策略、通道配置路径）
- 模块专用配置（JSON）：监听地址/端口、QPS 上限、超时、重试策略等非敏感项
- OSS 凭证（AccessKey ID/Secret、Bucket、Endpoint）在 `OssClientFacade` 源码中直接写死，不经过配置文件或环境变量

凭证写死简化了本期实现，后续需要时再外置化。

### D6: 流水线模式 — 固定阶段 + 可插拔策略

编排流水线为固定阶段序列：
```
RequestView → [Parse] → [Validate] → [BuildPayload] → [PublishSHM] → [EnqueueOSS]
```

每个阶段通过策略接口实现可替换性：
- `IRequestBodyParser`：支持 multipart 及未来格式
- `IFramePayloadBuilder`：构建 protobuf 载荷
- `IRateLimiter`：限流策略（token bucket / 滑动窗口）
- `IUploadPathPolicy`：OSS object key 生成规则

阶段本身不可增删（保证核心契约一致），但每个阶段可注入不同策略实现。

### D7: 构建系统 — gate/ 子目录 + muduo 动态库

```
gate/
├── CMakeLists.txt          # 本模块构建入口
├── oss/                    # OSS 封装层（独立 OBJECT 库）
│   ├── CMakeLists.txt
│   ├── iblob_storage_uploader.hpp
│   ├── oss_client_facade.hpp
│   ├── oss_client_facade.cpp
│   └── noop_uploader.hpp
├── http/                   # HTTP 接入层
│   ├── muduo_http_server.hpp
│   ├── muduo_http_server.cpp
│   └── request_view.hpp
├── handler/               # 编排层
│   ├── ingest_handler.hpp
│   ├── ingest_handler.cpp
│   ├── body_parser.hpp
│   └── frame_payload_builder.hpp
├── module/                 # ModuleBase 集成
│   ├── http_gateway_module.hpp
│   └── http_gateway_module.cpp
└── tests/                  # 本模块单元测试
    └── CMakeLists.txt
```

根 `CMakeLists.txt` 通过 `add_subdirectory(gate)` 纳入；muduo 通过 `FindModuo.cmake` 或 IMPORTED 目标链接，不编译源码（BUILD-01）。

## Risks / Trade-offs

- **[muduo 事件循环与 SHM 的线程安全]**: SHM ring buffer 文档声明允许多线程 Publish，但须在实现中验证并发场景下的游标推进和提交语义。
- **[OSS 上传异步化与 200 解耦]**: OSS 失败不影响 HTTP 200，但需要可靠的重试队列和可观测性来保证数据最终持久化（FR-3.4-03）。进程内队列在崩溃时会丢失未完成的上传任务——可接受（NFR-5.2-02 为 SHOULD）。
- **[muduo 依赖可用性]**: muduo 作为外部动态库，编译/链接环境需要预装。CI 环境需确保 muduo .so 在库搜索路径中。
- **[OSS 凭证写死]**: OSS 凭证（AccessKey ID/Secret）直接硬编码在 `OssClientFacade` 源码中，存在泄露风险。仓库仅限内部使用，暂不处理外置化。
- **[单生产者约束]**: 网关作为图片通道的唯一 HTTP 生产者，拓扑配置需确保无其他模块声明同一通道的生产者角色，否则 ChannelTopology 校验将拒绝启动。

## Open Questions

- QPS 目标值需与业务方确认后填入模块配置默认值
- OSS 对象 key 命名规则（如 `{date}/{node_id}/{image_id}.jpg`）需在接口规格中冻结
- muduo 版本要求（需确认工程环境中的 muduo 库版本）
