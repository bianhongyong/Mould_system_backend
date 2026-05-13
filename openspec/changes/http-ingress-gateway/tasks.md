## 1. 构建系统搭建

- [x] 1.1 创建 `gate/CMakeLists.txt`，定义模块构建目标，通过 `add_subdirectory` 组织 oss/http/pipeline/module 子目录
- [x] 1.2 创建 `gate/oss/CMakeLists.txt`，定义 OSS 封装层 OBJECT 库，链接阿里云 OSS SDK
- [x] 1.3 创建 `gate/tests/CMakeLists.txt`，定义测试目标并链接 GTest
- [x] 1.4 修改仓库根 `CMakeLists.txt`，添加 `add_subdirectory(gate)`，确认 muduo 以外部动态库（IMPORTED 目标或 `find_package`）链接
- [x] 1.5 验证构建：`cmake --build build --target backend_all` 包含 gate 目标且 muduo 以 `.so` 链接

## 2. OSS 封装层

- [x] 2.1 定义 `IBlobStorageUploader` 窄接口（`gate/oss/iblob_storage_uploader.hpp`），暴露 `UploadAsync(image_data, metadata, callback)` 方法
- [x] 2.2 实现 `OssClientFacade`（`gate/oss/oss_client_facade.hpp/.cpp`），封装阿里云 OSS SDK：凭证加载、Client 生命周期、PutObject、超时、错误码映射
- [x] 2.3 实现重试与退避：基于配置的重试次数（默认 3）和指数退避（1s, 2s, 4s）
- [x] 2.4 实现 `NoOpUploader`（`gate/oss/noop_uploader.hpp`），立即回调成功，用于 CI 和无网测试
- [x] 2.5 实现 `LocalFileUploader`（`gate/oss/local_file_uploader.hpp`），写入本地文件系统指定目录，文件名 `{request_id}.jpg`
- [x] 2.6 实现 `IUploadPathPolicy` 接口和默认实现（`gate/oss/upload_path_policy.hpp`），支持 `images/{node_id}/{date}/{image_id}.jpg` 等模板规则

## 3. HTTP 接入层

- [x] 3.1 定义 `RequestView` 结构体（`gate/http/request_view.hpp`），作为与 muduo 无关的通用请求视图
- [x] 3.2 实现 `MuduoHttpServer`（`gate/http/muduo_http_server.hpp/.cpp`）：启动 muduo HTTP 服务、设置回调将原始请求转为 RequestView 并投递到编排队列
- [x] 3.3 实现连接/请求上限控制：`max_connections`、`max_body_size`、`max_rate_per_conn`，超限时返回 413/503/429
- [x] 3.4 实现优雅关闭：`DoCleanup()` 停止接受新连接、等待处理中请求完成（带超时）

## 4. 编排层

- [x] 4.1 定义 `IRequestBodyParser` 接口（`gate/handler/body_parser.hpp`），实现 `MultipartFormDataParser` 解析 multipart/form-data
- [x] 4.2 实现请求校验器：必填字段（`node_id`、`capture_time`）、类型、长度、图片 magic bytes（JPEG/PNG/BMP）校验
- [x] 4.3 定义 `IFramePayloadBuilder` 接口，实现 `ImageFramePayloadBuilder`：组装 protobuf 载荷，注入 `request_id`、`node_id`、`capture_time`、`image_data`
- [x] 4.4 实现 `IngestHandler`（`gate/handler/ingest_pipeline.hpp/.cpp`）：固定五阶段流水线（解析→校验→组包→发SHM→登记OSS），每阶段成功后才进入下一阶段
- [x] 4.5 实现 `IRateLimiter` 接口和 TokenBucket 实现
- [x] 4.6 实现 `IMetricsRecorder` 接口和默认日志实现（成功/失败计数、延迟分位）
- [x] 4.7 实现请求标识生成（UUID），贯穿日志、SHM 载荷、OSS 对象

## 5. SHM 发布集成

- [x] 5.1 实现 `IShmFramePublisher` 接口（`gate/handler/shm_frame_publisher.hpp`），封装 `ShmBusRuntime` 的 Publish 调用
- [x] 5.2 实现 Publish 成功/失败的结果封装，失败时区分背压/槽位满/其他错误
- [x] 5.3 验证多线程 Publish 安全性：在并发测试中确认无数据竞争

## 6. ModuleBase 集成

- [x] 6.1 实现 `HttpGatewayModule`（`gate/module/http_gateway_module.hpp/.cpp`）：继承 ModuleBase，DoInit 启动 muduo 服务器和编排队列，OnRunIteration 轻量心跳
- [x] 6.2 使用 `REGISTER_MOULD_MODULE_AS("HttpGatewayModule", HttpGatewayModule)` 注册到 ModuleFactoryRegistry
- [x] 6.3 实现模块配置解析（监听地址/端口、QPS 上限、超时、重试策略等）；OSS 凭证在 `OssClientFacade` 源码中直接写死
- [x] 6.4 实现线程模型：muduo I/O 线程池 + 单一编排工作线程（通过 `muduo::BlockingQueue` 或类似机制解耦）

## 7. 配置与拓扑集成

- [x] 7.1 创建网关模块专用 JSON 配置模板（`gate/config/gateway_config.example.json`）：监听地址、OSS bucket/key 规则、各上限值
- [x] 7.2 在 `launch_plan.json` 示例中新增加 Gateway 模块条目（CPU 亲和性、重启策略、通道配置路径）
- [x] 7.3 创建网关的通道拓扑声明文件（`.txt`），声明图片通道及生产者角色
- [x] 7.4 在 `ShmBusControlPlane::ProvisionChannelTopologyFromModuleConfigs` 中确认网关通道声明被正确纳入

## 8. 可观测性

- [x] 8.1 实现结构化访问日志：每请求一行，含 `request_id`、`node_id`、HTTP 状态码、延迟 ms、SHM 结果、OSS 结果分类
- [x] 8.2 实现 OSS 失败详细日志：ERROR 级别含 `request_id`、OSS 错误码、失败原因；重试中 WARNING 级别含重试进度
- [x] 8.3 暴露内部指标（通过日志或简单 HTTP `/health` 端点）：总请求数、成功率、平均/p99 延迟、SHM 拒绝次数、OSS 失败/重试次数

## 9. 测试

- [x] 9.1 编写 OssClientFacade 单元测试（使用 NoOp/LocalFile，不依赖真实 OSS）
- [x] 9.2 编写 MultipartFormDataParser 单元测试（合法 multipart、缺失字段、格式错误）
- [x] 9.3 编写 IngestHandler 单元测试（Fake SHM Publisher + Fake OSS Uploader，覆盖正常流程和各阶段失败分支）
- [x] 9.4 编写 HttpGatewayModule 集成测试（启动 muduo 服务器，HTTP 客户端发送请求，验证 200/错误码）
- [x] 9.5 编写并发 Publish 测试（多线程同时 Publish 到 SHM ring buffer）
- [x] 9.6 使用真实阿里云 OSS（Bucket: mould-sys）跑通上传全链路验证并记录结果

## 10. 文档

- [x] 10.1 编写 HTTP 接口规格文档（URL、字段、Content-Type、状态码、请求/响应示例）
- [x] 10.2 编写部署说明（配置项、launch_plan 集成、muduo 动态库路径、OSS 凭证硬编码位置）
