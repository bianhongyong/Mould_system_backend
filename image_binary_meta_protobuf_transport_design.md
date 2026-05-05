# Image Binary Meta Protobuf Transport Design

## 背景

本次变更为共享内存通信链路补齐了统一的 payload 编解码策略层，并为图片传输定义了“protobuf 元数据 + 二进制图像本体”的双层协议。

底层 SHM ring 仍然只承载 `ByteBuffer`，不改变共享内存布局和发布/订阅模型；新增能力集中在上层 payload 类型治理、通道 schema 校验、图片封装协议、protobuf 聚合入口和测试验证。

## 设计目标

- 统一 protobuf、图片、原始二进制等 payload 类型的编解码入口。
- 未显式声明通道类型时默认按 protobuf 处理。
- 启动期校验 protobuf 通道名必须严格等于 protobuf 消息名，不支持 alias。
- 图片传输使用 `meta_len + meta_pb_bytes + image_bytes` 布局。
- 图片单消息最大 3MB，超限直接拒绝并记录指标。
- 图片通道支持独立槽位大小配置，避免复用普通消息容量。
- 提供 protobuf 聚合入口与 schema version 记录。

## 关键文件

- `common/comm/include/payload_codec.hpp`
- `common/comm/src/payload_codec.cpp`
- `common/comm/src/shm_bus_runtime.cpp`
- `common/config/include/channel_topology_config.hpp`
- `common/config/src/channel_topology_config.cpp`
- `common/proto/mould/comm/image_meta.proto`
- `common/proto/mould/comm/aggregate.proto`
- `common/proto/SCHEMA_VERSION.md`
- `common/comm/payload_codec_registry_test.cpp`
- `common/config/channel_topology_config_gtest.cpp`

## 核心数据结构

### `PayloadType`

定义 payload 的逻辑类型：

- `kProtobuf`
- `kImage`
- `kBinaryBlob`
- `kUnknown`

它是 codec registry 的路由 key，也是通道治理中 `payload_type` 的运行时表达。

### `PayloadDescriptor`

描述一条 payload 的核心元数据：

- `payload_type`：payload 类型，默认 protobuf。
- `size`：payload 字节数。
- `checksum`：payload 校验值。
- `schema_version`：schema 版本。
- `ttl`：消息生命周期。
- `ref_count`：引用计数。
- `storage_flags`：存储标记。

它用于发布端写入 payload 前生成一致的描述信息，也用于消费端解码时做一致性检查。

### `PayloadPacket`

统一承载 codec 输入输出结果：

- `descriptor`：上层 payload 描述。
- `shm_slot`：需要镜像到 SHM slot/header 的元数据。
- `data`：最终写入共享内存的二进制 payload。

消费端解码时会校验 `descriptor` 与 `shm_slot` 的镜像字段是否一致，避免 header 与 payload 声明漂移。

### `IPayloadCodec`

统一 codec 接口：

```cpp
class IPayloadCodec {
 public:
  virtual ~IPayloadCodec() = default;
  virtual PayloadType Type() const = 0;
  virtual bool Encode(const ByteBuffer& input, PayloadPacket* packet) const = 0;
  virtual bool Decode(const PayloadPacket& packet, ByteBuffer* output) const = 0;
};
```

每个 payload 类型实现一个 codec，调用方不直接关心具体编码细节。

### `CodecRegistry`

`CodecRegistry` 负责注册和查找 codec，并提供带错误状态的统一入口：

- `Register(codec)`：注册 codec。
- `Find(type)`：按 `PayloadType` 查找 codec。
- `Encode(type, input, metrics)`：按类型编码。
- `Decode(packet, metrics)`：按 packet 声明类型解码。
- `CreateDefault()`：注册默认 protobuf、image、binary codec。

未知类型会返回明确 `absl::Status`，并递增 `codec_unknown_payload_type_errors` 指标。

### `ImageMeta`

图片元数据结构，对应 `common/proto/mould/comm/image_meta.proto`：

- `encoding`：JPEG、PNG、NV12、RAW 等编码枚举。
- `checksum_type`
- `checksum_value`
- `width`
- `height`
- `timestamp`
- `lamination_detect_flag`
- `channels`
- `format`
- `schema_version`
- `image_name`

图像本体不放进 protobuf message，而是作为独立二进制追加在 transport payload 末尾。

### `ChannelTopologyEntry`

通道拓扑项新增治理语义：

- `payload_type` 为空时默认 protobuf。
- protobuf 通道可配置 `proto_message`，但必须与 `channel` 完全一致。
- 通道可配置 `slot_payload_bytes`，作为每槽 payload 大小（图像与普通消息统一键名）。

相关 helper：

- `ResolveChannelPayloadType`
- `ValidateChannelSchemaGovernance`
- `ResolveSlotPayloadBytesForChannel`
- `ValidateProtoReservedFieldNumbers`

## 图片双层协议

图片 payload 的二进制布局为：

```text
+----------------+----------------+-------------+
| meta_len u32le | meta_pb_bytes  | image_bytes |
+----------------+----------------+-------------+
```

字段说明：

- `meta_len`：小端 `uint32`，表示 protobuf 元数据长度。
- `meta_pb_bytes`：`ImageMeta` 的 protobuf wire bytes。
- `image_bytes`：JPEG/PNG/NV12/RAW 等编码后的图像字节。

编码流程：

1. 根据 `ImageMeta` 生成 protobuf wire bytes。
2. 若 `checksum_type == simple32` 且未提供 `checksum_value`，自动计算图像 bytes 校验值。
3. 计算总大小：`4 + meta_pb_bytes.size() + image_bytes.size()`。
4. 超过 3MB 则拒绝编码并递增 `image_message_oversize_errors`。
5. 拼接为最终 `ByteBuffer`。

解码流程：

1. 校验总大小不超过 3MB。
2. 读取 `meta_len`。
3. 拆分 `meta_pb_bytes` 与 `image_bytes`。
4. 解析 `ImageMeta`。
5. 在图像解码前先校验 checksum。
6. checksum 不匹配则快速失败并递增 `image_checksum_errors`。

## 设计模式

### 策略模式

`IPayloadCodec` 是策略接口，`ProtobufPayloadCodec`、`ImagePayloadCodec`、`BinaryBlobCodec` 是具体策略。

优点：

- 每种 payload 类型独立实现。
- 新增类型时只需要新增 codec。
- 避免在发布/消费链路中不断增加 `switch` 或分散编码逻辑。

### 注册表模式

`CodecRegistry` 维护 `PayloadType -> IPayloadCodec` 的映射。

优点：

- 发布和消费链路只依赖 registry。
- 未知类型集中处理，错误可观测。
- 默认 codec 集合可通过 `CreateDefault()` 固化。

### 适配层设计

SHM ring 仍然只接收 `ByteBuffer`。`ShmBusRuntime::PublishWithStatus` 在写入 ring 前通过 `CodecRegistry` 将业务 payload 转为规范化 `PayloadPacket::data`。

这样底层传输无需理解 protobuf 或图片协议，协议治理集中在 comm 上层。

## 发布链路

`ShmBusRuntime::PublishWithStatus` 的关键流程：

1. 根据 channel 查找 topology entry。
2. 解析通道 payload type，未声明时为 protobuf。
3. 使用 `CodecRegistry::CreateDefault()` 获取默认 registry。
4. 调用 `registry.Encode(payload_type, payload, &metrics_)`。
5. 编码失败时返回明确状态并递增失败指标。
6. 编码成功后将 `PayloadPacket::data` 写入 ring。
7. 通知在线 consumer。

## 通道治理

### 默认 protobuf

当通道未设置 `payload_type` 时：

```text
payload_type = protobuf
```

这保证旧配置或普通业务消息默认走 protobuf 路由。

### 严格同名

protobuf 通道如果声明：

```text
channel = CameraFrame
proto_message = CameraFrame
```

则通过校验。

如果：

```text
channel = camera.frame
proto_message = CameraFrame
```

则启动期校验失败。系统不支持 alias，也不做宽松匹配。

### 图片槽位隔离

普通通道使用 `slot_payload_bytes` 推导每槽 payload budget。

图片通道可使用：

```text
payload_type=image
slot_payload_bytes=3145728
```

`ResolveSlotPayloadBytesForChannel` 直接读取统一的 `slot_payload_bytes` 配置。

## Protobuf 组织与演进

新增文件：

- `common/proto/mould/comm/image_meta.proto`
- `common/proto/mould/comm/aggregate.proto`
- `common/proto/SCHEMA_VERSION.md`

演进规则：

- 字段号只增不改不复用。
- 废弃字段必须 `reserved`。
- 新增字段应尽量使用 `optional` 或具备默认语义。
- 聚合 proto 变化必须更新 `schema_version` 记录。
- 消费端解析必须忽略未知字段，保持向后兼容。

当前代码提供 `ValidateProtoReservedFieldNumbers` 做 reserved 字段号复用校验的基础能力。

## 可观测性

`ReliabilityMetrics` 新增指标：

- `codec_unknown_payload_type_errors`
- `codec_mismatch_errors`
- `image_message_oversize_errors`
- `image_checksum_errors`
- `channel_schema_errors`

这些指标用于区分未知类型、元数据不一致、图片超限、checksum 错误和通道治理失败。

## 测试覆盖

新增和扩展的测试覆盖：

- `PayloadCodecRegistry.DispatchByPayloadType_DefaultProtobuf`
- `PayloadCodecRegistry.RejectUnknownPayloadType_WithObservableError`
- `PayloadCodecRegistry.DecodeRejectsMetadataMismatch`
- `ChannelSchema.StrictNameEqualsProtoMessage_NoAlias`
- `ChannelSchema.UnconfiguredChannelDefaultsToProtobuf`
- `ImageBinaryMetaTransport.EncodeDecode_RoundTrip`
- `ImageBinaryMetaTransport.ChecksumValidation_FailFast`
- `ImageMessageSizeLimit.RejectOver3MB`
- `ImageChannelSlotProfile.UsesDedicatedCapacity`
- `ProtobufEvolution.BackwardCompatibleOptionalField`
- `ProtobufEvolution.ReservedFieldNumber_NotReusable`

验证命令：

```bash
cmake -S . -B build
cmake --build build --target payload_codec_registry_test channel_topology_config_gtest
ctest --test-dir build -R 'PayloadCodecRegistry|ImageBinaryMetaTransport|ImageMessageSizeLimit|ProtobufEvolution|ChannelSchema|ImageChannelSlotProfile' --output-on-failure
```

## 后续建议

- 将 `ImageMeta` protobuf 由当前手写 wire helper 进一步接入正式 protoc 生成代码。
- 将 `ValidateProtoReservedFieldNumbers` 扩展为完整 proto lint 工具。
- 在消费链路中补齐 typed decode 的业务对象分发入口。
- 将 image checksum 算法从 `simple32` 扩展为 CRC32、SHA256 等可配置策略。
