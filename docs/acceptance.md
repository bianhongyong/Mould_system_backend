# 验收标准确认

本变更验收标准如下：

1. 工程骨架可构建：根 `CMakeLists.txt` + 分层模块 + 子目录组织齐备。
2. 探针可运行：`third_party_probe` 接入构建与 `ctest`。
3. CI 闸门生效：`cmake configure -> build -> ctest` 在 CI 工作流中定义为必经步骤。

验收说明：

- 骨架与构建入口：已完成。
- 探针与本地检查入口：已完成，`third_party_probe_self_test` 通过。
- CI 基础闸门：已完成（`.github/workflows/backend-dependency-gate.yml`）。

结论：验收标准满足，工程骨架可构建、探针可运行、CI 闸门已定义并可执行。
