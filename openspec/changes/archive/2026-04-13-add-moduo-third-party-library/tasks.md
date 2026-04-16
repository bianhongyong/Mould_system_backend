# Tasks

## 1. 依赖发现与导入

- [x] 1.1 在 `cmake/third_party.cmake` 增加 moduo 的依赖发现逻辑，按 CONFIG/MODULE/pkg-config 顺序兜底并在缺失时快速失败。
- [x] 1.2 将 moduo 纳入 `MS_THIRD_PARTY_LINK_TARGETS` 或 `MS_THIRD_PARTY_LIBRARIES` 与 `MS_THIRD_PARTY_INCLUDE_DIRS` 聚合集合。
- [x] 1.3 为 moduo 补充依赖报告记录（source/version/include），保证 configure 日志可诊断。

## 2. 构建契约与探针验证

- [x] 2.1 确认 `thirdparty::deps` 输出包含 moduo，`common_build_flags` 与现有目标无需额外改动即可消费。
- [x] 2.2 更新 `tests/third_party_probe.cpp`，加入 moduo 头文件或最小符号引用，形成真实编译/链接验证。
- [x] 2.3 运行 `cmake configure -> build -> ctest` 验证探针通过，并在缺失 moduo 时可复现确定性失败信息。
