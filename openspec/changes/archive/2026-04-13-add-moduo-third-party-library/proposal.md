## Why

当前后端依赖清单尚未纳入 moduo，导致新项目或新环境中无法通过统一的 CMake 依赖发现与探针机制验证该库可用性。现在引入 moduo，可将网络基础库纳入现有第三方依赖治理流程，降低后续服务接入成本与环境不一致风险。

## What Changes

- 在第三方依赖导入模块中新增 moduo 的发现、导入与失败诊断逻辑。
- 将 moduo 接入统一依赖聚合目标（`thirdparty::deps`），使业务与测试目标通过既有构建契约消费它。
- 扩展 `third_party_probe` 的编译/链接验证覆盖范围，确保 moduo 在本地与 CI 中可被真实验证。
- 更新依赖报告输出内容，体现 moduo 的来源、版本（可得时）与头文件路径信息。

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `third-party-dependency-probe`: 扩展必需依赖集合与探针验证范围，将 moduo 纳入统一导入流程、兜底发现策略与自动化验证门禁。

## Impact

- 影响构建配置：`cmake/third_party.cmake`、可能新增或调整 `cmake/modules/Find*.cmake`。
- 影响测试探针：`tests/third_party_probe.cpp` 及其构建链接行为。
- 影响依赖环境：开发机与 CI 需具备可发现的 moduo 开发包（CMake config、Find 模块或 pkg-config 任一可用）。
