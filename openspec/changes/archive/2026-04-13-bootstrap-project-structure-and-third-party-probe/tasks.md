## 1. 工程骨架与 CMake 主体

- [x] 1.1 创建后端单仓库目录骨架：`common`、`services/broker`、`services/infer`、`cmake`、`tests`
- [x] 1.2 编写根 `CMakeLists.txt`，纳入各子目录并声明聚合目标 `backend_all`
- [x] 1.3 编写 `cmake/platform.cmake`，统一 C++ 标准、构建类型、告警级别与可选优化参数
- [x] 1.4 为 `services/broker` 与 `services/infer` 增加聚合构建目标（如 `broker_all`、`infer_all`）

## 2. 第三方依赖导入体系

- [x] 2.1 编写 `cmake/third_party.cmake`，统一导入 OSS SDK、MySQL、RabbitMQ、Boost、absl、gRPC、protobuf
- [x] 2.2 为缺少 CMake 包配置的依赖补充 `pkg-config` 导入逻辑
- [x] 2.3 为仍不可导入的依赖补充 `cmake/modules/Find<Lib>.cmake` 兜底模块
- [x] 2.4 在 configure 阶段输出关键依赖版本与路径，保证诊断信息可追踪

## 3. 依赖探针与自动化验证

- [x] 3.1 新增最小可执行目标 `third_party_probe`，覆盖头文件包含与符号链接验证
- [x] 3.2 将 `third_party_probe` 接入默认构建流程，确保依赖问题可尽早暴露
- [x] 3.3 配置 `ctest` 执行探针验证用例，形成可复用的本地/CI 检查入口
- [x] 3.4 在 CI 中加入 `cmake configure -> build -> ctest` 基础闸门并验证失败可阻断

## 4. 文档与交付验收

- [x] 4.1 补充构建说明文档（依赖安装、配置命令、常见失败排查）
- [x] 4.2 补充依赖导入验收清单（每个三方库的通过标准与失败策略）
- [x] 4.3 完成一次干净环境构建演练并记录结果
- [x] 4.4 验收标准确认：工程骨架可构建、探针可运行、CI 闸门生效
