## Context

当前工程已建立统一第三方依赖导入机制，通过 `cmake/third_party.cmake` 将依赖聚合到 `thirdparty::deps`，并由 `tests/third_party_probe.cpp` 执行最小可编译/可链接验证。moduo 尚未纳入该流程，导致网络相关能力在不同环境中的可用性无法被统一校验。

引入 moduo 的目标不是在本次变更中实现业务功能，而是将其并入现有依赖治理基线，使本地和 CI 在业务开发前就能发现缺依赖、版本不兼容或链接失败问题。

## Goals / Non-Goals

**Goals:**
- 将 moduo 纳入统一第三方依赖发现链路（CMake config / Find 模块 / pkg-config 兜底中的可行路径）。
- 通过 `thirdparty::deps` 暴露 moduo，保持子工程消费方式不变。
- 扩展 `third_party_probe`，把 moduo 编译与链接检查纳入自动化验证。
- 在 configure 日志中输出 moduo 的依赖来源与诊断信息。

**Non-Goals:**
- 不在本变更中引入基于 moduo 的业务服务实现。
- 不重构现有依赖管理架构（仍沿用 `thirdparty::deps` + `common_build_flags`）。
- 不在本变更中替换现有网络库或调整服务接口。

## Decisions

1. 沿用现有依赖治理模式扩展 moduo，而非新增独立依赖模块。  
   - Rationale: 现有流程已覆盖“多发现路径 + 统一聚合 + 报告输出”，扩展成本最低且与当前能力一致。  
   - Alternative considered: 单独创建 `moduo.cmake` 并由目标按需链接；被否决，原因是会破坏统一依赖入口并增加维护面。

2. 保持单一契约目标 `thirdparty::deps`，不向业务目标直接暴露 moduo 探测细节。  
   - Rationale: 目标层保持稳定接口，避免每个可执行目标重复编写 moduo 链接规则。  
   - Alternative considered: 在 `common_build_flags` 外新增 `network_build_flags`；被否决，当前阶段收益不足且会增加接入复杂度。

3. 通过 `third_party_probe` 增量验证 moduo 可用性。  
   - Rationale: 探针已是 CI 门禁的一部分，扩展该目标可以最小改动获得最大反馈价值。  
   - Alternative considered: 新增独立 `moduo_probe`；被否决，短期会分散门禁入口，后续仍需汇总到同一验证链路。

4. 依赖缺失保持 configure 阶段快速失败。  
   - Rationale: 失败越早越容易定位，不应把缺依赖问题延迟到编译或链接阶段。  
   - Alternative considered: 将 moduo 标记为可选依赖；被否决，因为 proposal 明确其为必需依赖集合成员。

## Risks / Trade-offs

- [Risk] 不同发行版上 moduo 包名或导出目标名不一致，可能导致发现链路不稳定  
  → Mitigation: 提供多路径发现策略并在日志中明确“source/version/include”，便于定点修复。

- [Risk] 探针中对 moduo 的符号引用过深，可能引入非必要 ABI 绑定  
  → Mitigation: 只选择稳定且最小的头文件/符号做编译链接验证，避免业务语义耦合。

- [Risk] 将 moduo 设为强依赖后，部分开发环境会立即 configure 失败  
  → Mitigation: 在文档与 CI 环境中同步声明安装前置项，并提供明确报错指引。

- [Trade-off] 更严格的依赖门禁会提升初次环境准备成本  
  → Mitigation: 换取更早发现问题与更稳定的跨环境构建一致性。
