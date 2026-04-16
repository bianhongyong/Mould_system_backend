# Third-party dependency probe

## Purpose

TBD

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

### Requirement: 最小依赖探针可执行程序

工程 MUST 包含最小可执行目标（`third_party_probe`），并对后端服务依赖集合（含 moduo）进行真实编译与链接验证。

#### Scenario: 探针目标可链接声明依赖

- **WHEN** 用户构建 `third_party_probe` 目标
- **THEN** 仅当当前环境中声明依赖（含 moduo）的头文件与链接符号均有效时，构建 SHALL 成功

### Requirement: 探针集成到自动化验证

依赖探针 MUST 集成到自动化验证流程，使 CI 能在业务模块实现前发现依赖回归问题。

#### Scenario: CI 执行探针验证

- **WHEN** CI 流水线运行工程校验
- **THEN** 流水线 SHALL 执行探针相关构建与测试步骤，并在探针检查未通过时失败
