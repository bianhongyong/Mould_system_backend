# HTTP Ingress Gateway — HTTP 接口规格

## 概述

网关模块对外提供 RESTful HTTP 接口，接收前端/设备端上传的覆膜检测图片并
投递到后端共享内存总线。

---

## 端点

### `POST /api/v1/frames/upload`

上传一张覆膜检测图片及其元数据。

#### 请求格式

`multipart/form-data`

#### 字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `image` | file | 是 | 待检测图片文件。支持的格式见 `image_validation.allowed_formats` |
| `node_id` | text | 是 | 采集节点标识，最大长度 256 字节 |
| `capture_time` | text | 是 | 采集时间，ISO-8601 格式，如 `2026-05-09T10:30:00+08:00` |
| `equipment_id` | text | 否 | 设备编号 |
| `batch_id` | text | 否 | 批次号 |
| `defect_type_hint` | text | 否 | 缺陷类型提示（预留字段） |
| `extra` | text | 否 | 扩展元数据（JSON 字符串） |

#### 请求示例

```
POST /api/v1/frames/upload HTTP/1.1
Host: gateway.example.com:8080
Content-Type: multipart/form-data; boundary=----FormBoundary7MA4YWxk

------FormBoundary7MA4YWxk
Content-Disposition: form-data; name="node_id"

CAM-001
------FormBoundary7MA4YWxk
Content-Disposition: form-data; name="capture_time"

2026-05-09T10:30:00+08:00
------FormBoundary7MA4YWxk
Content-Disposition: form-data; name="image"; filename="defect_001.jpg"
Content-Type: image/jpeg

<binary data>
------FormBoundary7MA4YWxk--
```

#### 成功响应

**HTTP 200 OK**

```json
{
  "request_id": "req-1715243400000-42",
  "message": "ok",
  "image_id": "img-20260509-abcdef"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `request_id` | string | 全局唯一请求标识，用于链路追踪 |
| `message` | string | `"ok"` |
| `image_id` | string | 生成的图片 ID，可用于后续查询 |

#### 错误响应

所有错误响应体格式一致：

```json
{
  "request_id": "req-1715243400000-99",
  "error": "short_error_code",
  "details": "人类可读的错误描述"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `request_id` | string | 全局唯一请求标识 |
| `error` | string | 短错误码 |
| `details` | string | 详细错误描述 |

---

## HTTP 状态码

| 状态码 | 含义 | 触发条件 |
|--------|------|----------|
| `200` | 成功 | 请求处理完毕，图片已投递到内部通道 |
| `400` | 请求格式错误 | 缺少必填字段、multipart 解析失败、字段超长 |
| `413` | 请求体过大 | 请求体超过 `max_body_size_bytes`（默认 50MB） |
| `415` | 不支持的媒体类型 | 图片格式不在 `allowed_formats` 列表内 |
| `422` | 参数校验失败 | `capture_time` 非合法时间格式、`node_id` 为空等 |
| `429` | 请求频率过高 | 单连接速率超过 `max_rate_per_conn`，或全局 QPS 超过 `qps_limit` |
| `503` | 服务不可用 | 并发连接数达到上限、系统正在关闭中 |

### 错误码对照

| HTTP 状态码 | `error` 值 | 说明 |
|-------------|------------|------|
| 400 | `missing_field` | 缺少必填字段 |
| 400 | `invalid_multipart` | multipart 格式解析失败 |
| 400 | `field_too_long` | 字段值超过 `max_field_length` |
| 413 | `body_too_large` | 请求体超过大小上限 |
| 415 | `unsupported_format` | 图片格式不被允许 |
| 422 | `invalid_timestamp` | 时间戳格式非法 |
| 422 | `empty_node_id` | 节点标识为空 |
| 429 | `rate_limited` | 触发限流 |
| 503 | `too_many_connections` | 并发连接数超限 |
| 503 | `shutting_down` | 服务器正在关闭 |

---

## `/health` 端点

**无需认证**的内部健康检查端点，返回 JSON 格式的运行时指标。

### 请求

```
GET /health HTTP/1.1
Host: gateway.example.com:8080
```

### 响应

**HTTP 200 OK**

```json
{
  "status": "ok",
  "active_connections": 5,
  "total_requests": 1024,
  "success_count": 1000,
  "failure_count": 24,
  "avg_latency_ms": 12.5,
  "p99_latency_ms": 45.2,
  "shm_rejected_count": 2,
  "oss_failure_count": 1,
  "oss_retry_count": 3
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | string | `"ok"` |
| `active_connections` | int | 当前活跃 TCP 连接数 |
| `total_requests` | int | 累计请求总数（由 pipeline 层提供） |
| `success_count` | int | 成功处理的请求数 |
| `failure_count` | int | 失败请求数 |
| `avg_latency_ms` | float | 平均处理延迟（毫秒） |
| `p99_latency_ms` | float | P99 处理延迟（毫秒） |
| `shm_rejected_count` | int | 共享内存写入被拒绝的次数 |
| `oss_failure_count` | int | OSS 上传失败次数 |
| `oss_retry_count` | int | OSS 上传重试次数 |

若未注册健康检查回调，返回：

```json
{
  "status": "ok",
  "active_connections": 5,
  "note": "health callback not registered"
}
```

---

## 速率限制

- **单连接速率：** 每秒最多 `max_rate_per_conn` 个请求，超出返回 `429`。
- **并发连接上限：** 最多 `max_connections` 个并发连接，超出返回 `503`。
- **全局 QPS 上限：** 由 `qps_limit` 配置（需 pipeline 层配合）。

---

## 请求体大小限制

默认最大请求体为 50MB（可通过 `max_body_size_bytes` 配置），超出返回 `413`。
