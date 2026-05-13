# Third-party dependency build integration

## Purpose

通过 CMake 统一导入并校验必需第三方依赖；不再提供独立的最小可执行探针目标。

## Requirements

### Requirement: 统一的第三方依赖导入流程

构建系统 MUST 定义统一依赖导入流程，覆盖 OSS SDK、MySQL 客户端、RabbitMQ 客户端、Boost、Abseil、gRPC、Protobuf、moduo。

#### Scenario: 在配置阶段检查必需依赖

- **WHEN** 用户对后端工程执行 CMake configure
- **THEN** 配置步骤 SHALL 校验必需依赖可用性，并在缺失库时快速失败且输出可执行的诊断信息

### Requirement: 非 CMake 包的兜底导入策略

对于未提供可用 CMake 包配置文件的依赖，构建系统 MUST 提供基于 `pkg-config` 或自定义 `Find<Lib>.cmake` 模块的兜底发现机制。

#### Scenario: 缺少 Config.cmake 的依赖仍可被发现

- **WHEN** 某必需依赖缺少原生 CMake config 包
- **THEN** 工程 SHALL 尝试兜底发现，并且要么成功导入，要么输出确定性的失败原因
