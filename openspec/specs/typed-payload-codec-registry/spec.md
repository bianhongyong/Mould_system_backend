# Typed payload codec registry

## Purpose

Define a unified, pluggable payload codec registry keyed by payload type.

## Requirements

### Requirement: Payload codec registry SHALL support pluggable type strategies
系统 MUST 提供统一的 payload 编解码注册表，按 `payload_type` 路由到对应策略实现，以支持 protobuf、图片二进制、原始二进制及后续新增类型的无侵入扩展。

#### Scenario: Registry dispatches by payload type
- **WHEN** 发布端提交某个已注册 `payload_type` 的消息
- **THEN** 系统使用该类型对应的 codec 执行编码并写入二进制 payload

#### Scenario: Registry rejects unknown payload type
- **WHEN** 发布端提交未注册 `payload_type` 的消息
- **THEN** 系统拒绝发布并返回明确错误，且记录错误指标

### Requirement: Consumer decode SHALL use the same codec contract
消费端 MUST 使用与发布端一致的 codec 契约进行解码，确保同一类型消息在跨模块传输时语义一致。

#### Scenario: Consumer decodes payload with matching codec
- **WHEN** 消费端收到已注册类型的消息
- **THEN** 消费端按对应 codec 成功还原上层数据对象

#### Scenario: Consumer fails decode on metadata and payload mismatch
- **WHEN** 消费端发现 payload 元数据声明类型与实际 codec 不匹配
- **THEN** 消费端拒绝解码并记录一致性错误
