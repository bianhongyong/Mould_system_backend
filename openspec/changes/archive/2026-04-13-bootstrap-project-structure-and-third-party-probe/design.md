## Context

该变更是后续中间服务与推理服务实现的前置工程化步骤，目标是先建立可扩展的单仓库 CMake 结构，并验证关键第三方库在当前环境下可被稳定导入与链接。当前风险主要来自依赖来源不统一、不同库导入方式差异大、以及“配置成功但链接/运行失败”的晚发现问题。

约束条件：

- 同一代码仓需要承载 `common`、`broker`、`infer` 三类代码并共享依赖。
- 构建系统必须支持后续多进程可执行目标持续扩展。
- 需要对 OSS SDK、MySQL、RabbitMQ、Boost、absl、gRPC、protobuf 做可验证导入。

## Goals / Non-Goals

**Goals:**

- 形成统一的 CMake 分层架构（根构建入口 + 平台配置 + 第三方依赖导入 + 子模块）。
- 明确模块级依赖边界，避免子模块重复查找三方库。
- 提供一个最小可执行探针 `third_party_probe`，用于配置期、编译期、运行期三层验证。
- 为 CI 提供可执行的基础构建目标（`backend_all`）与验证入口（`ctest`）。

**Non-Goals:**

- 不实现中间服务、推理服务的业务逻辑。
- 不在本变更中完成生产级部署脚本和完整运维体系。
- 不处理模型推理框架（如 TensorRT/ONNX Runtime）的深度优化配置。

## Decisions

1. 采用“单入口 + 分层包含”的 CMake 组织

- 决策：
  - 根 `CMakeLists.txt` 仅负责全局选项、模块编排和聚合目标。
  - `cmake/platform.cmake` 负责编译选项、构建类型、LTO/PCH/警告策略。
  - `cmake/third_party.cmake` 统一处理三方依赖导入与降级方案。
- 原因：
  - 便于跨模块复用配置，降低重复配置导致的不一致。
  - 将“平台差异”和“依赖差异”与业务目录解耦。
- 备选方案：
  - 每个子模块自行 `find_package`。该方案初期简单，但会导致版本漂移与维护复杂度上升，未采用。

1. 采用“统一依赖入口 + 接口库传播”的依赖管理方式

- 决策：
  - 在 `common` 层定义接口库（INTERFACE/STATIC），通过 `target_link_libraries` 传播 include、compile definitions、link flags。
  - 子模块不直接耦合三方库具体导入细节。
- 原因：
  - 保持依赖关系清晰，便于后续替换客户端实现（如 RabbitMQ 切换到 Kafka 客户端）。
- 备选方案：
  - 直接在可执行目标上链接所有依赖。该方案耦合高、难以治理，未采用。

1. 对第三方导入采用“优先 Config.cmake，兜底 pkg-config/Find 模块”

- 决策：
  - 优先 `find_package(<Lib> CONFIG REQUIRED)`。
  - 若库未提供 Config 包，则使用 `pkg_check_modules`。
  - 仍不可用时，提供 `cmake/modules/Find<Lib>.cmake`。
- 原因：
  - 兼容多发行版与多安装来源（系统包、源码安装、vcpkg/conan）。
- 备选方案：
  - 强制单一包管理器。该方案对现场环境要求高，灵活性不足，未采用。

1. 引入 `third_party_probe` 作为依赖健康闸门

- 决策：
  - 新增最小程序同时覆盖头文件包含、符号引用与最小运行检测。
  - 将其纳入默认构建与 `ctest`。
- 原因：
  - 防止仅通过 configure 但在链接/运行阶段失败。
- 备选方案：
  - 仅在文档中要求手工验证。该方案不可自动化、容易遗漏，未采用。

1. 构建性能基线

- 决策：
  - 推荐 `Ninja + ccache + PCH` 组合。
  - 默认 `RelWithDebInfo` 用于开发调试，发布使用 `Release`。
- 原因：
  - 在保证问题可定位的前提下降低增量编译耗时。

## Risks / Trade-offs

- [Risk] 部分第三方库在目标机器无标准 CMake 包配置  
→ Mitigation：提供 `pkg-config` 与自定义 `Find<Lib>.cmake` 双兜底，并在 `third_party_probe` 中做最终验证。
- [Risk] 不同环境中库版本不一致导致 ABI/符号冲突  
→ Mitigation：在 configure 阶段输出关键库版本并在 CI 固化版本矩阵。
- [Risk] 依赖过早耦合在可执行目标，后续重构成本高  
→ Mitigation：依赖全部下沉到 `common` 接口库并统一传播。
- [Risk] 构建优化参数（如 LTO）在某些工具链不稳定  
→ Mitigation：将优化参数配置为可开关，默认保守启用策略。

## Migration Plan

1. 创建工程目录与基础 `CMakeLists.txt`（根 + common + services 子模块）。
2. 落地 `platform.cmake` 与 `third_party.cmake`，先完成配置期导入验证。
3. 实现 `third_party_probe`，验证编译与链接。
4. 将 `third_party_probe` 接入 `ctest`，作为 CI 基础闸门。
5. 在通过依赖闸门后，再进入业务实现阶段（通信中间件与服务主链路）。

回滚策略：

- 若某库导入失败，保持工程骨架与无该库耦合的目标可构建；
- 对失败依赖按模块开关降级，不阻塞其他基础模块推进。

## Open Questions

- OSS SDK 在目标部署环境的安装来源是否统一（系统包或源码）？
- RabbitMQ 客户端是否固定为 `rabbitmq-c`，是否需要预留替换层？
- 是否要求离线环境构建（涉及第三方依赖镜像与缓存策略）？

