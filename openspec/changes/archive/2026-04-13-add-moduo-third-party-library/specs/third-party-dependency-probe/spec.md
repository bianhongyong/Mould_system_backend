## MODIFIED Requirements

### Requirement: 统一的第三方依赖导入流程

构建系统 MUST 定义统一依赖导入流程，覆盖 OSS SDK、MySQL 客户端、RabbitMQ 客户端、Boost、Abseil、gRPC、Protobuf、moduo。

#### Scenario: 在配置阶段检查必需依赖

- **WHEN** 用户对后端工程执行 CMake configure
- **THEN** 配置步骤 SHALL 校验必需依赖可用性，并在缺失库时快速失败且输出可执行的诊断信息

### Requirement: 最小依赖探针可执行程序

工程 MUST 包含最小可执行目标（`third_party_probe`），并对后端服务依赖集合（含 moduo）进行真实编译与链接验证。

#### Scenario: 探针目标可链接声明依赖

- **WHEN** 用户构建 `third_party_probe` 目标
- **THEN** 仅当当前环境中声明依赖（含 moduo）的头文件与链接符号均有效时，构建 SHALL 成功
