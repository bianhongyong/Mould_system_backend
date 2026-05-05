## ADDED Requirements

### Requirement: Image payload SHALL use binary body plus protobuf metadata
图片传输 MUST 使用双层协议：图片本体 SHALL 以编码后二进制写入 payload，图片元数据 SHALL 使用 protobuf 描述，不得将图片本体直接作为 protobuf 主承载。

#### Scenario: Producer sends image as binary with metadata
- **WHEN** 发布端发送图片消息
- **THEN** 消息包含编码后的 `image_bytes` 与可解析的 `ImageMeta` protobuf 元数据

### Requirement: Image metadata SHALL include checksum and core attributes
`ImageMeta` MUST 包含 `checksum_type`、`checksum_value`、`encoding`、`width`、`height`、`timestamp`、`schema_version` 以及业务标记字段。

#### Scenario: Consumer validates checksum before image decode
- **WHEN** 消费端收到图片消息并读取元数据
- **THEN** 消费端先校验 checksum，再决定后续图像解码流程

### Requirement: Image message size MUST be limited to 3MB
单条图片消息总大小 MUST 小于等于 3MB，超限消息 SHALL 被拒绝并产生可观测告警。

#### Scenario: Oversized image is rejected
- **WHEN** 发布端尝试发送超过 3MB 的图片消息
- **THEN** 系统拒绝写入并记录超限错误指标与日志

### Requirement: Image channel slot capacity SHALL be configured independently
图片通道的槽位容量 MUST 支持独立配置，不得强制复用普通消息通道的默认槽位配置。

#### Scenario: Image channel uses dedicated slot profile
- **WHEN** 系统加载通道拓扑配置
- **THEN** 图片通道采用独立槽位参数并与普通通道资源隔离
