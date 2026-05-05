# Protobuf aggregate versioning

## Purpose

Define protobuf aggregate entry governance and schema evolution compatibility rules.

## Requirements

### Requirement: System SHALL provide a protobuf aggregate entry file
系统 MUST 提供一个总 protobuf 聚合入口文件，用于统一汇总项目内对外通信所需的 protobuf 类型。

#### Scenario: New message type is discoverable from aggregate proto
- **WHEN** 新增通信消息类型并完成注册
- **THEN** 该类型可通过聚合入口文件被统一引用

### Requirement: Protobuf evolution MUST follow strict compatibility rules
protobuf 演进 MUST 遵循以下规则：字段号只增不改不复用；废弃字段 MUST 使用 `reserved`；新增字段 SHOULD 为 `optional` 或具有默认语义；每次聚合 proto 变更 MUST 记录 `schema_version`。

#### Scenario: Deprecated field keeps reserved number
- **WHEN** 某字段被废弃
- **THEN** 字段号被保留为 `reserved` 且后续不可复用

#### Scenario: Aggregate proto change records schema version
- **WHEN** 聚合 proto 发生变更
- **THEN** 变更记录包含新的 `schema_version` 与兼容性说明

### Requirement: Consumers SHALL parse with backward compatibility
消费端 MUST 采用向后兼容解析策略，读不到新字段时仍能保持核心业务流程可用。

#### Scenario: Older consumer reads message with new optional field
- **WHEN** 旧版本消费端读取包含新增可选字段的消息
- **THEN** 消费端忽略未知字段并继续正常处理
