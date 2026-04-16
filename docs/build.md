# 后端构建说明

## 依赖安装

建议在 Ubuntu 上安装如下依赖（按实际发行版调整包名）：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential ninja-build cmake pkg-config \
  libprotobuf-dev protobuf-compiler \
  libgrpc++-dev libabsl-dev \
  libboost-filesystem-dev libboost-system-dev libboost-thread-dev \
  libmysqlclient-dev librabbitmq-dev
```

> OSS C++ SDK 需按企业环境安装方式额外安装，并确保头文件 `oss/OssClient.h` 与库可被系统路径或 `CMAKE_PREFIX_PATH` 发现。

## 配置与构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target backend_all
ctest --test-dir build --output-on-failure
```

## 常见失败排查

- `Required dependency not found: <name>`：
  - 补齐系统依赖包；或设置 `CMAKE_PREFIX_PATH` 指向第三方安装路径。
  - 检查 `pkg-config --modversion <pkg>` 是否可返回版本。
- `fatal error: ... No such file or directory`：
  - 头文件未在 include path；优先检查库开发包是否安装。
- 链接失败 `undefined reference`：
  - 对应库仅找到头文件未找到链接库，检查动态库/静态库路径。
- `third_party_probe` 运行失败：
  - 检查运行时动态库路径（`LD_LIBRARY_PATH`）和依赖版本匹配。
