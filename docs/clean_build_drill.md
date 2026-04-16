# 干净环境构建演练记录

## 环境

- 日期：2026-04-11
- OS：Linux 6.8.0-107-generic
- 生成器：Ninja
- 构建类型：RelWithDebInfo

## 执行命令

```bash
cmake -S . -B build-clean -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-clean --target backend_all
ctest --test-dir build-clean --output-on-failure
```

## 结果

- configure：通过
- build：通过（目标：`backend_all`）
- ctest：通过（`third_party_probe_self_test`，1/1）

关键输出摘要：

- 依赖版本与来源已在 configure 阶段打印（含 protobuf/gRPC/absl/Boost/MySQL/RabbitMQ/OSSSDK）。
- `third_party_probe` 成功链接并运行自检用例。
