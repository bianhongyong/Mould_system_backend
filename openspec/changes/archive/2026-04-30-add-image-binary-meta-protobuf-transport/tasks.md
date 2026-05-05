## 1. 编解码策略层与注册机制

- [x] 1.1 梳理并固化 `IPayloadCodec` 统一接口契约（输入输出、错误返回、类型标识），补齐默认 protobuf 路由行为定义。
- [x] 1.2 实现/整理 `CodecRegistry` 注册与查找路径，确保按 `payload_type` 可插拔扩展并对未知类型返回可观测错误。
- [x] 1.3 在发布与消费调用链接入统一 registry，替换模块内分散的手写编码逻辑。

## 2. 通道治理与命名强约束

- [x] 2.1 新增通道 schema 规则：未显式声明类型时默认 protobuf。
- [x] 2.2 新增启动期校验：通道名与 protobuf 消息名必须严格一致，不允许 alias。
- [x] 2.3 补充治理日志与错误码，明确命名不一致与 schema 缺失的失败原因。

## 3. 图片双层协议与容量控制

- [x] 3.1 定义图片双层消息封装：`meta_len + meta_pb_bytes + image_bytes` 二进制布局并接入编解码流程。
- [x] 3.2 定义并接入 `ImageMeta` protobuf 元数据（含 `encoding` 枚举、`checksum_type`、`checksum_value`、宽高、时间戳、业务 flag、`schema_version`）。
- [x] 3.3 实现单消息 3MB 上限校验与拒绝路径，并输出超限指标/告警日志。
- [x] 3.4 为图片通道增加独立槽位大小配置项，避免复用普通通道默认槽位。

## 4. Protobuf 聚合入口与兼容规则落地

- [x] 4.1 新增总 protobuf 聚合入口文件并纳入构建流程，统一导出通信消息类型。
- [x] 4.2 在开发规范与校验流程中落地硬规则：字段号只增不改不复用、废弃字段 `reserved`、新增字段尽量 `optional`。
- [x] 4.3 建立聚合 proto 版本记录机制（`schema_version` 变更记录 + 兼容说明）。

## 5. 详尽单元测试（GTest）

- [x] 5.1 单元测试 `PayloadCodecRegistry_DispatchByPayloadType_DefaultProtobuf`：构造未显式类型通道与多类型消息，验证默认 protobuf 路由和 registry 按类型分发正确。测试目的：确保策略层行为稳定且默认规则生效。
- [x] 5.2 单元测试 `PayloadCodecRegistry_RejectUnknownPayloadType_WithObservableError`：发送未注册类型消息，断言发布失败、返回明确错误且错误指标递增。测试目的：确保异常类型可控失败并可观测。
- [x] 5.3 单元测试 `ChannelSchema_StrictNameEqualsProtoMessage_NoAlias`：分别覆盖同名与异名通道，断言同名通过、异名启动失败。测试目的：验证“通道名与 protobuf 消息名严格一致”的治理约束。
- [x] 5.4 单元测试 `ImageBinaryMetaTransport_EncodeDecode_RoundTrip`：对 `meta_len + meta_pb_bytes + image_bytes` 做编解码往返，断言元数据字段与图片字节完整一致。测试目的：验证图片双层协议正确性。
- [x] 5.5 单元测试 `ImageBinaryMetaTransport_ChecksumValidation_FailFast`：构造 checksum 错误消息，断言消费端在图像解码前快速失败并上报错误。测试目的：验证数据完整性校验路径。
- [x] 5.6 单元测试 `ImageMessageSizeLimit_RejectOver3MB`：发送恰好 3MB 与超过 3MB 的边界样本，断言前者通过、后者拒绝且有超限告警。测试目的：验证图片上限规则与边界行为。
- [x] 5.7 单元测试 `ImageChannelSlotProfile_UsesDedicatedCapacity`：加载包含图片通道和普通通道的拓扑配置，断言图片通道采用独立槽位参数。测试目的：验证资源隔离策略。
- [x] 5.8 单元测试 `ProtobufEvolution_BackwardCompatibleOptionalField`：旧版消费端解析含新增 optional 字段消息，断言主流程仍可用。测试目的：验证向后兼容解析约束。
- [x] 5.9 单元测试 `ProtobufEvolution_ReservedFieldNumber_NotReusable`：模拟废弃字段后尝试复用字段号，断言校验失败。测试目的：验证字段演进硬规则落地。
