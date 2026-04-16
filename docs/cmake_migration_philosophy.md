# CMake 编写哲学（可迁移版）

本文档提炼当前项目的 CMake 组织思想，目标是让你在其他项目中快速复用同一套构建方法，而不是只复制语法片段。

## 1. 顶层只做“编排”，不做实现细节

根 `CMakeLists.txt` 的职责应当是：

- 定义项目元信息（`cmake_minimum_required`、`project`）。
- 加载统一策略模块（如 `cmake/platform.cmake`、`cmake/third_party.cmake`）。
- 按模块注册子目录（`add_subdirectory(...)`）。
- 定义少量聚合目标（如 `backend_all`）用于“一键构建/检查”。

核心原则：**顶层文件保持短小，避免把编译选项和第三方探测逻辑堆在根目录。**

## 2. 平台策略集中化：一次定义，全局生效

`cmake/platform.cmake` 体现的思想是“把编译行为当作可配置策略”：

- 统一 C++ 标准（C++20）、关闭编译器扩展（`CMAKE_CXX_EXTENSIONS OFF`）。
- 提供可切换选项（`MS_ENABLE_WARNINGS_AS_ERRORS`、`MS_ENABLE_LTO`、`MS_ENABLE_NATIVE_OPT`）。
- 按编译器分支设置告警级别（GNU/Clang 与 MSVC 分开）。
- 按构建类型设置优化等级（Debug / Release / RelWithDebInfo / MinSizeRel）。
- 配置阶段打印关键状态（编译器、标准、构建类型、开关值），用于可观测性。

核心原则：**把“构建策略”从业务目标中抽离，形成项目级基础设施。**

## 3. 依赖治理分层：优先现代 CMake，逐级降级兜底

`cmake/third_party.cmake` 的依赖发现路径非常值得迁移：

1. 优先 `find_package(... CONFIG)`（现代 CMake target 方式）。
2. 次选 `find_package(... MODULE)`（Find 模块）。
3. 最后 `pkg-config` 兜底。
4. 三层都失败则 `FATAL_ERROR`（快速失败，避免隐性缺依赖）。

同时做了两件关键事：

- 统一收敛为一个 `INTERFACE` 目标：`thirdparty::deps`。
- 记录依赖来源/版本/头文件路径并在 configure 阶段输出。

核心原则：**依赖解析可以复杂，但对业务目标暴露的接口必须简单稳定。**

## 4. 用 INTERFACE 目标做“构建契约”

当前项目使用两层 `INTERFACE`：

- `thirdparty::deps`：聚合所有三方库与头文件路径。
- `common_build_flags`：聚合编译特性（`cxx_std_20`）并链接 `thirdparty::deps`。

业务/测试目标只需链接 `common_build_flags`，无需重复写第三方依赖细节。

核心原则：**让目标之间通过“契约”连接，而不是通过复制粘贴连接。**

## 5. 测试先接入基础探针，再扩展业务测试

`tests/CMakeLists.txt` 先放了 `third_party_probe`：

- 它验证“依赖可链接、可运行”这一底座能力。
- 通过 `add_test` 接入 `ctest`，形成可自动化执行的门禁。

核心原则：**先守住构建与依赖健康，再叠加复杂测试。**

## 6. 聚合目标是迁移时的“对外入口”

当前聚合目标：

- `broker_all`
- `infer_all`
- `third_party_probe`
- 根目标 `backend_all` 统一依赖上述目标

这种设计让外部系统（CI、本地脚本、IDE）只面向固定入口，而不是理解全部内部目标。

核心原则：**对外一个入口，对内自由演进。**

## 7. 迁移到其他项目时的推荐落地步骤

1. 复制目录骨架：`cmake/`、`cmake/modules/`、各模块子目录 `CMakeLists.txt`。
2. 先迁移 `platform.cmake`（标准、告警、优化、LTO 开关）。
3. 再迁移 `third_party.cmake`（按你的依赖清单替换包名与 Find 模块）。
4. 保留并改名 `thirdparty::deps` 与 `common_build_flags` 两层契约目标。
5. 新项目先做一个 `*_probe` 可执行并接入 `ctest`。
6. 增加一个根聚合目标 `<project>_all` 作为 CI 默认构建入口。

## 8. 可直接复用的最小模板（骨架）

```cmake
# CMakeLists.txt (root)
cmake_minimum_required(VERSION 3.22)
project(my_project LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

include(cmake/platform.cmake)
include(cmake/third_party.cmake)

enable_testing()

add_subdirectory(common)
add_subdirectory(services/foo)
add_subdirectory(tests)

add_custom_target(
  project_all
  DEPENDS
    foo_all
    third_party_probe
)
```

```cmake
# common/CMakeLists.txt
add_library(common_build_flags INTERFACE)
target_compile_features(common_build_flags INTERFACE cxx_std_20)
target_link_libraries(common_build_flags INTERFACE thirdparty::deps)
```

## 9. 迁移检查清单（建议）

- `cmake -S . -B build` 能完整输出平台信息与依赖来源信息。
- `cmake --build build -j` 成功，且聚合目标可一次构建。
- `ctest --test-dir build --output-on-failure` 至少包含一个探针测试且通过。
- 打开/关闭 `-DMS_ENABLE_LTO=ON`、`-DMS_ENABLE_WARNINGS_AS_ERRORS=OFF` 时行为符合预期。
- 缺失关键依赖时能在 configure 阶段快速失败并给出明确报错。

---

如果你愿意，我可以下一步再给你补一份“跨项目复制时的变量命名规范对照表”（哪些保留 `MS_` 前缀，哪些改成新项目前缀），避免后续维护时语义混乱。
