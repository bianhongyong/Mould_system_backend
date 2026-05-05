## ADDED Requirements

### Requirement: Channel transport type SHALL default to protobuf
通道在未显式声明传输类型时 MUST 按 protobuf 处理，发布端与消费端均 SHALL 遵循该默认约定。

#### Scenario: Unconfigured channel uses protobuf by default
- **WHEN** 通道配置未设置 `payload_type`
- **THEN** 系统按 protobuf 类型执行编解码和校验

### Requirement: Channel name MUST strictly equal protobuf message name
通道名与对应 protobuf 消息名 MUST 严格一致，不允许别名映射或宽松匹配。

#### Scenario: Strict name match succeeds
- **WHEN** 通道名与 protobuf 消息名完全一致
- **THEN** 系统允许该通道完成 schema 注册与消息收发

#### Scenario: Name mismatch is rejected
- **WHEN** 通道名与 protobuf 消息名不一致
- **THEN** 系统启动校验失败并拒绝该通道生效
