# 第三方依赖导入验收清单

| 依赖 | 通过标准 | 失败策略 |
| --- | --- | --- |
| OSS SDK | configure 可发现；`third_party_probe` 可通过头文件编译并链接 OSS 符号 | 阻断构建；输出安装路径与包名提示 |
| MySQL Client | configure 发现 `mysql/mysql.h` 与 client 库；探针输出版本 | 阻断构建；提示安装 `libmysqlclient-dev` 或等效包 |
| RabbitMQ C Client | configure 发现 `amqp.h` 与 `rabbitmq` 库；探针输出版本 | 阻断构建；提示安装 `librabbitmq-dev` 或等效包 |
| Boost | configure 发现 `filesystem/system/thread` 组件 | 阻断构建；提示缺失组件名称 |
| absl | configure 发现 `absl::base` 或 `pkg-config` 条目 | 阻断构建；提示安装 `libabsl-dev` |
| gRPC | configure 发现 `gRPC::grpc++` 或 `pkg-config` 条目 | 阻断构建；提示安装 `libgrpc++-dev` |
| protobuf | configure 发现 `protobuf`；探针成功链接 | 阻断构建；提示安装 `libprotobuf-dev` 和 `protoc` |
