## Why

多节点 Qt 采集软件需要将图片及附属信息上报到后端推理流水线，当前缺少统一的 HTTP 入口。网关作为后端对外的 HTTP 接入点，使 Qt 客户端在不直接操作 SHM 的前提下将数据送入流水线，同时异步持久化至 OSS 供长期留存和未来丢图恢复。

## What Changes

- 新增 `gate/` 顶层目录，承载 HTTP 接入网关的全部实现（HTTP 接入、编排逻辑、OSS 封装、单元测试、CMakeLists）
- 基于 muduo 提供 HTTP 服务，接收图片二进制及附属元数据（multipart/form-data）
- 实现请求校验 → 组包 → SHM 发布 → 异步 OSS 上传的编排流水线
- 作为 SHM 图片通道的生产者，遵守 ChannelTopology / launch_plan 约束
- 在阿里云 OSS SDK 之上实现工程内封装层，编排层仅依赖封装窄接口
- 使用真实 OSS（Bucket: mould-sys）跑通上传全链路验证
- 纳入 supervisor 管理的子进程模型，通过 launch_plan 配置 CPU、重启策略等
- 根 CMakeLists.txt 通过 `add_subdirectory(gate)` 纳入本模块；muduo 仅链接外部动态库，不编译随仓源码

## Capabilities

### New Capabilities

- `http-ingress-endpoint`: 基于 muduo 的 HTTP 服务端点，监听地址/端口可配置，支持连接数、请求体大小、速率上限
- `request-validation-handler`: 请求解析、字段校验、载荷组包的编排流水线，支持可插拔解析策略
- `shm-frame-publishing`: 将校验通过的图片/元数据发布到 SHM 图片通道，遵守 ChannelTopology 约束
- `oss-blob-persistence`: 基于官方 SDK 封装的异步 OSS 上传，含路径策略、重试退避、失败可观测
- `gateway-module-lifecycle`: 网关模块进程生命周期管理，包含 ModuleBase 集成、supervisor 监管、launch_plan 配置

### Modified Capabilities

<!-- 无需修改现有 spec，均为新增能力 -->

## Impact

- 新增目录：`gate/`（含源码、测试、CMakeLists、文档）
- 根 `CMakeLists.txt`：新增 `add_subdirectory(gate)`
- 依赖：muduo 动态库（CMake IMPORTED 目标，不编译源码）、阿里云 OSS SDK（通过工程内封装层使用）
- 配置：`launch_plan.json` 新增加模块条目；通道拓扑新增图片通道生产者声明
- 通信中间件：无需改动，网关作为普通 SHM 生产者使用现有 pub/sub 运行时
