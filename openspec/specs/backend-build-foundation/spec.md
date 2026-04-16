# Backend build foundation

## Purpose

TBD

## Requirements

### Requirement: 统一的 CMake 工程目录布局

后端仓库 MUST 提供统一的 CMake 工程布局，并使用唯一的根 `CMakeLists.txt` 编排 `common`、`services/broker`、`services/infer` 子目录。

#### Scenario: 根工程可成功配置

- **WHEN** 用户在仓库根目录使用受支持的生成器执行 CMake configure
- **THEN** 构建系统 SHALL 自动发现并纳入所有声明的子目录，无需手工修改路径

### Requirement: 分层的构建配置模块

构建系统 MUST 将平台/工具链配置与第三方依赖配置拆分为独立 CMake 模块，且各子工程 SHALL 从根配置统一消费这些模块。

#### Scenario: 平台与依赖模块只在统一入口加载

- **WHEN** CMake configure 执行根 `CMakeLists.txt`
- **THEN** 工程 SHALL 在集中入口加载平台设置与第三方依赖配置，子工程 SHALL 不重复实现全局依赖发现逻辑

### Requirement: 服务聚合构建目标

构建系统 SHALL 提供中间服务二进制聚合目标、推理服务二进制聚合目标，以及适用于 CI 的仓库级聚合目标。

#### Scenario: 聚合目标可触发完整构建

- **WHEN** 用户在已配置的构建目录中编译聚合目标
- **THEN** CMake MUST 对所选聚合目标集合中的全部进程二进制执行调度与构建

### Requirement: 可确定的编译器与构建类型基线

构建系统 MUST 定义可确定的编译器标准与构建类型基线，并为调试与发布配置显式编译选项。

#### Scenario: 构建配置选项被正确应用

- **WHEN** 用户使用指定构建类型配置工程
- **THEN** 生成目标 SHALL 继承基线定义的编译宏、优化参数与调试参数
