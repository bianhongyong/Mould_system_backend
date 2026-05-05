## Why

当前共享内存总线的 `payload` 固定为二进制，但上层业务数据类型持续增加（protobuf、图片、原始二进制等），缺少统一且可扩展的编解码抽象，导致模块内重复编码逻辑和协议治理不一致。现在需要统一通信约定，特别是图片场景采用“二进制图像本体 + protobuf 元数据”的双层模型，以兼顾性能与可维护性。

## What Changes

- 新增统一的 payload 编解码策略层与注册机制，支持按数据类型/通道进行编码与解码路由。
- 明确通道默认传输类型为 protobuf，并定义强约束的通道命名治理规则。
- 建立总 protobuf 聚合入口文件，统一汇总项目内通用消息类型。
- 定义图片传输协议：图片本体以编码后二进制传输，图片元数据使用 protobuf 承载。
- 固化图片消息上限为 3MB，并要求图片槽位采用独立容量配置。
- 固化 protobuf 演进兼容规则（字段号、reserved、optional、版本记录、向后兼容解析）。

## Capabilities

### New Capabilities
- `typed-payload-codec-registry`: 定义可插拔的 payload 编解码策略接口与注册机制，统一多类型数据在共享内存二进制载荷中的编解码过程。
- `protobuf-channel-governance`: 定义默认 protobuf 通道规则与通道名和 protobuf 消息名严格一致的治理约束。
- `image-binary-meta-transport`: 定义图片“二进制本体 + protobuf 元数据”双层传输协议及 3MB 上限与槽位隔离策略。
- `protobuf-aggregate-versioning`: 定义总 protobuf 聚合入口文件与 protobuf 版本兼容演进规则。

### Modified Capabilities
- `payload-slot-metadata-routing`: 扩展现有 payload/slot 元数据规范，使其与新的图片元数据协议、校验信息和通道治理规则保持一致。

## Impact

- 影响 `common/comm` 的 payload 编解码抽象、通道类型路由与图片元数据封装路径。
- 影响模块侧发布/订阅约定（默认 protobuf、命名严格一致、图片消息大小限制）。
- 影响 protobuf 文件组织方式，需要新增聚合入口与版本记录流程。
- 影响测试体系，需要新增编解码正确性、兼容性、超限与校验失败等测试。
