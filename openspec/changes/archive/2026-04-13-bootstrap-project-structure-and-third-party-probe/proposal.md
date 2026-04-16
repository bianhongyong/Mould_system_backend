## Why

当前后端需要同时推进中间服务与推理服务，但在缺少统一工程结构和可靠依赖验证的情况下，开发容易被构建问题反复阻塞。先完成 CMake 工程骨架和第三方库最小探针，可将风险前置并为后续功能开发建立稳定基线。

## What Changes

- 建立单仓库 C++ 后端工程结构，覆盖 `common`、`services/broker`、`services/infer`、`cmake` 等基础目录与构建入口。
- 建立分层 CMake 构建体系：根 `CMakeLists.txt`、`cmake/platform.cmake`、`cmake/third_party.cmake`、子模块 `CMakeLists.txt`。
- 增加第三方库导入与链接验证能力，覆盖 OSS SDK、MySQL、RabbitMQ、Boost、absl、gRPC、protobuf。
- 增加最小测试程序 `third_party_probe`，用于在配置期、编译期、运行期确认依赖可用性。
- 增加基础聚合构建目标（如 `backend_all`、`broker_all`、`infer_all`）及 CI 可执行的最小验证流程。

## Capabilities

### New Capabilities
- `backend-build-foundation`: 定义后端单仓库 CMake 分层构建能力，包括模块划分、目标组织与编译选项基线。
- `third-party-dependency-probe`: 定义第三方依赖的统一导入与最小可执行验证能力，确保库可配置、可编译、可链接、可运行。

### Modified Capabilities
- (none)

## Impact

- Affected code:
  - 根构建入口与构建脚本：`CMakeLists.txt`、`cmake/`
  - 公共库与服务目录结构：`common/`、`services/broker/`、`services/infer/`
  - 依赖验证程序与测试：`third_party_probe` 及 `ctest` 配置
- APIs/Protocols:
  - 无业务接口变更；仅新增构建与依赖验证能力。
- Dependencies:
  - 明确并验证 OSS SDK、MySQL、RabbitMQ、Boost、absl、gRPC、protobuf 的导入策略。
- Systems:
  - 影响本地开发环境与 CI 构建流水线，降低后续实现阶段的集成风险。
