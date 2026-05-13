## ADDED Requirements

### Requirement: HTTP 服务启动与配置

网关 SHALL 基于 muduo 提供 HTTP 服务，监听地址与端口 SHALL 通过模块配置指定。

#### Scenario: 按配置启动监听

- **WHEN** 网关模块初始化完成且配置中指定监听地址为 `0.0.0.0:8080`
- **THEN** muduo HTTP 服务器在 `0.0.0.0:8080` 上接受连接

#### Scenario: 端口冲突

- **WHEN** 配置指定的端口已被占用
- **THEN** 网关 SHALL 记录错误日志并退出，由 supervisor 按重启策略处理

### Requirement: 连接与请求上限

网关 SHALL 对并发连接数、单请求体大小、单连接速率提供可配置上限，超出时拒绝服务并返回明确错误。

#### Scenario: 请求体超过上限

- **WHEN** 客户端发送的请求体超过配置的 `max_body_size`（默认 50MB）
- **THEN** 网关 SHALL 返回 HTTP 413 并关闭连接

#### Scenario: 并发连接数超限

- **WHEN** 当前活跃连接数已达到配置的 `max_connections` 上限
- **THEN** 网关 SHALL 拒绝新连接，返回 HTTP 503

#### Scenario: 速率超限

- **WHEN** 某连接的请求速率超过配置的 `max_rate_per_conn`（默认 10 req/s）
- **THEN** 网关 SHALL 返回 HTTP 429，并在响应头中包含 `Retry-After`

### Requirement: 多格式请求体解析

网关 SHALL 支持 `multipart/form-data` 格式的请求体解析，承载图片二进制与附属元数据字段。解析策略 SHALL 可插拔。

#### Scenario: 标准 multipart 上传

- **WHEN** 客户端发送 Content-Type 为 `multipart/form-data` 的请求，包含 `image` 文件字段和 `metadata` 文本字段
- **THEN** 网关解析出图片二进制流和元数据键值对，传入编排层

#### Scenario: 不支持的 Content-Type

- **WHEN** 客户端发送非 multipart 且无对应解析器的 Content-Type
- **THEN** 网关 SHALL 返回 HTTP 415（Unsupported Media Type）
