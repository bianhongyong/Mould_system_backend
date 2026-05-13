# HTTP Ingress Gateway — 部署说明

## 构建依赖

编译网关模块需以下依赖（已包含在顶层 `cmake/third_party.cmake` 中）：


| 依赖                                 | 说明             |
| ---------------------------------- | -------------- |
| muduo                              | 网络库（动态库 `.so`） |
| protobuf                           | 消息序列化          |
| glog                               | 日志库            |
| gflags                             | 命令行参数解析        |
| Boost (filesystem, system, thread) | 文件系统与线程工具      |
| OpenCV                             | 图片处理（验证格式、尺寸）  |
| OSS SDK                            | 阿里云 OSS 上传     |
| RabbitMQ C 客户端                     | 预留消息队列支持       |


> **重要：** muduo 以动态库形式提供，`LD_LIBRARY_PATH` 必须包含 muduo 库路径。

## CMake 配置与构建

```bash
# 从项目根目录执行
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 构建所有后端目标（包含 gateway）
cmake --build build --target backend_all

# 仅构建 gateway 模块
cmake --build build --target gate_lib
```

构建产物：

- `build/gate/libgate_lib.a` — 网关静态库
- `build/services/backend/backend_main` — 后端主进程（含网关模块）

## launch_plan.json 配置示例

```json
{
  "minloglevel": 1,
  "communication": {
    "shm_slot_count": 1024
  },
  "modules": {
    "FrameSourceModule": {
      "module_name": "FrameSourceModule",
      "resource": {
        "startup_priority": 10,
        "cpu_set": "0",
        "restart_backoff_ms": 20,
        "restart_max_retries": 3,
        "restart_window_ms": 1000,
        "restart_fuse_ms": 3000,
        "ready_timeout_ms": 500
      },
      "module_params": {},
      "io_channels_config_path": "tests/resources/backend_integration/frame_source_io.json"
    },
    "HttpGatewayModule": {
      "module_name": "HttpGatewayModule",
      "resource": {
        "startup_priority": 5,
        "cpu_set": "1",
        "restart_backoff_ms": 20,
        "restart_max_retries": 3,
        "restart_window_ms": 1000,
        "restart_fuse_ms": 3000,
        "ready_timeout_ms": 500
      },
      "module_params": {},
      "io_channels_config_path": "gate/config/http_gateway_io.json"
    }
  }
}
```

### 配置项说明


| 配置项                                           | 说明                                      |
| --------------------------------------------- | --------------------------------------- |
| `HttpGatewayModule.resource.startup_priority` | 启动优先级，5 表示在 FrameSourceModule (10) 之后启动 |
| `HttpGatewayModule.resource.cpu_set`          | CPU 亲和性，"1" 绑定到 CPU 核心 1                |
| `HttpGatewayModule.io_channels_config_path`   | 通道拓扑声明文件路径                              |


### 网关模块参数（module_params）

可通过 `module_params` 覆盖网关配置：

```json
"module_params": {
  "listen_addr": "0.0.0.0",
  "port": "8080",
  "max_connections": "100",
  "max_body_size_bytes": "52428800",
  "max_rate_per_conn": "10"
}
```

未指定的参数使用 `gate/config/gateway_config.example.json` 中的默认值。

## 通道拓扑声明

`gate/config/http_gateway_io.json`：

```json
{
  "input_channel": {},
  "output_channel": {
    "ImageFrameIngress": {
      "slot_payload_bytes": "4194304"
    }
  }
}
```

- `frame.http` 为输出通道名称，槽大小 4MB，可容纳大图片。
- 该通道会被 `ShmBusControlPlane` 自动发现并分配共享内存段。

## 环境变量配置


| 环境变量                           | 说明                 | 默认值                                       |
| ------------------------------ | ------------------ | ----------------------------------------- |
| `MOULD_MODULE_CHANNEL_CONFIGS` | 模块通道配置路径列表         | 由 launch_plan 生成                          |
| `MOULD_SHM_SLOT_COUNT`         | 共享内存槽位数            | `communication.shm_slot_count`            |
| `LD_LIBRARY_PATH`              | 动态库搜索路径（需包含 muduo） | 视部署环境而定                                   |
| `MOULD_GATEWAY_CONFIG`         | 网关模块 JSON 配置文件路径   | `gate/config/gateway_config.example.json` |


## OSS 凭证配置

OSS 访问密钥（AccessKey ID / AccessKey Secret / Endpoint / Bucket）在
`gate/oss/oss_client_facade.cpp` 中直接写死。

> **安全注意：** 当前实现为简化开发将凭证硬编码在源码中。生产环境部署前应改为
> 从环境变量或密钥管理服务（KMS / Vault）读取。

若需修改 OSS 配置，编辑 `gate/oss/oss_client_facade.cpp` 中的以下区域：

```cpp
// TODO(build): 生产环境应从环境变量或 KMS 读取
constexpr char kOssEndpoint[] = "oss-cn-hangzhou.aliyuncs.com";
constexpr char kOssBucket[]   = "mould-inspection-images";
constexpr char kOssKeyId[]    = "<your-access-key-id>";
constexpr char kOssKeySecret[]= "<your-access-key-secret>";
```

## 运行与验证

### 1. 确认环境

```bash
# 检查 muduo 动态库
ldconfig -p | grep muduo

# 设置库路径
export LD_LIBRARY_PATH=/path/to/muduo/lib:$LD_LIBRARY_PATH
```

### 2. 启动后端主进程

```bash
./build/services/backend/backend_main \
  --launch_plan_path=launch_plan.json
```

### 3. 验证网关模块启动

查看日志确认网关模块已加载：

```text
I0509 10:00:00.123456 12345 gateway_module.cpp:100] HttpGatewayModule started, listening on 0.0.0.0:8080
```

### 4. 健康检查

```bash
curl -s http://localhost:8080/health | python3 -m json.tool
```

预期输出：

```json
{
  "status": "ok",
  "active_connections": 0,
  "note": "health callback not registered"
}
```

### 5. 上传图片

```bash
curl -s -X POST http://localhost:8080/api/v1/frames/upload \
  -F "image=@test.jpg" \
  -F "node_id=CAM-001" \
  -F "capture_time=2026-05-09T10:30:00+08:00" | python3 -m json.tool
```

预期输出：

```json
{
  "request_id": "req-1715243400000-1",
  "message": "ok",
  "image_id": "img-20260509-abcdef"
}
```

### 6. 错误场景验证

```bash
# 缺少必填字段 → 400
curl -s -X POST http://localhost:8080/api/v1/frames/upload \
  -F "image=@test.jpg" | python3 -m json.tool

# 请求体过大 → 413
curl -s -X POST http://localhost:8080/api/v1/frames/upload \
  -F "image=@large_file.bin" \
  -F "node_id=CAM-001" \
  -F "capture_time=2026-05-09T10:30:00+08:00" | python3 -m json.tool
```

## 日志

网关模块使用 glog 输出日志，日志级别由 `launch_plan.json` 中的 `minloglevel` 控制：


| 值   | 级别                    |
| --- | --------------------- |
| 0   | INFO（含 WARNING、ERROR） |
| 1   | WARNING（含 ERROR）      |
| 2   | ERROR                 |
| 3   | FATAL                 |


关键日志事件：

- `HttpGatewayModule started` — 模块启动成功
- `HttpConnection from <ip>` — 新连接
- `ProcessRequest: /api/v1/frames/upload ...` — 收到上传请求
- `SHM publish failed` — 共享内存投递失败
- `OSS upload failed` — OSS 上传失败（将触发重试）

