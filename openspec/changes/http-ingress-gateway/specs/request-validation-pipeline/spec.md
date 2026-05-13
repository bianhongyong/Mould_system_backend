## ADDED Requirements

### Requirement: 必填字段校验

网关 SHALL 对请求中的必填字段进行校验，校验失败 SHALL 返回非 2xx 状态码，且 SHALL NOT 将数据发布到 SHM。

#### Scenario: 缺少必填字段

- **WHEN** 请求中缺少必填字段（如 `node_id`、`capture_time`）
- **THEN** 网关 SHALL 返回 HTTP 422，响应体包含缺失字段列表

#### Scenario: 字段类型错误

- **WHEN** 请求中 `capture_time` 字段格式不是 ISO 8601 时间戳
- **THEN** 网关 SHALL 返回 HTTP 422，响应体指出类型错误的具体字段

#### Scenario: 字段长度超限

- **WHEN** 请求中 `node_id` 字段长度超过配置的 `max_field_length`
- **THEN** 网关 SHALL 返回 HTTP 422，响应体指出超限字段

### Requirement: 图片格式校验

网关 SHALL 对上传的图片进行格式校验（至少校验支持的图片类型：JPEG、PNG、BMP），校验失败 SHALL 返回非 2xx 且不发布到 SHM。

#### Scenario: 支持的图片格式

- **WHEN** 上传图片为 JPEG 格式（magic bytes `FF D8 FF`）
- **THEN** 网关通过格式校验，继续编排流程

#### Scenario: 不支持的图片格式

- **WHEN** 上传文件不是 JPEG/PNG/BMP（如 GIF magic bytes `47 49 46`）
- **THEN** 网关 SHALL 返回 HTTP 422，响应体说明不支持的图片格式

### Requirement: 流水线阶段编排

网关 SHALL 以固定阶段流水线处理每个请求：解析 → 校验 → 组包 → 发 SHM → 登记持久化。每个阶段 SHALL 可通过策略接口注入不同实现。

#### Scenario: 流水线正常执行

- **WHEN** 一个合法的 multipart 请求到达
- **THEN** 请求依次经过解析、校验、组包、SHM 发布、OSS 登记五个阶段，每阶段成功后才进入下一阶段

#### Scenario: 流水线中途失败

- **WHEN** 校验阶段失败（缺少必填字段）
- **THEN** 流水线在此阶段终止，后续阶段不执行，HTTP 返回校验失败状态码

### Requirement: 载荷组装 — 分层 proto 设计

网关 SHALL 将校验通过的图片与元数据组装为 protobuf 消息，采用分层设计：

- **公共层**：`ImageFrameIngress` 消息的顶层字段（`request_id`、`node_id`、`capture_time`、`image_data`、`image_format`），所有场景共用
- **扩展层**：`scenario_extensions`（`repeated google.protobuf.Any`）承载每种场景各自定义的独立 proto 消息
- **透传层**：`extra_metadata`（`map<string, string>`）承载未被映射到公共字段的额外表单字段

不同场景下图片的元信息可能存在差异，差异化部分通过场景 proto 分离，顶层 `ImageFrameIngress` 保持稳定。新增场景只需定义新的 proto 消息，无需修改顶层 schema。

#### Scenario: 组装 ImageFrameIngress proto 消息

- **WHEN** 校验通过，元数据包含 `node_id`="camera-01"、`capture_time`="2026-05-08T10:30:00Z"，图片为 JPEG 二进制
- **THEN** 网关构造的 `ImageFrameIngress` proto 消息中顶层字段与入参一致，消息序列化为 proto 二进制格式发布到 SHM 通道

#### Scenario: 场景扩展承载

- **WHEN** 请求包含场景相关字段（如 `product_type`、`threshold` 等）
- **THEN** 未被映射到公共字段的元数据写入 `extra_metadata` map，供下游按场景类型解析

### Requirement: 请求标识追踪

网关 SHALL 为每个请求生成唯一标识（如 UUID），贯穿日志、SHM 载荷、OSS 对象，实现全链路可追溯。

#### Scenario: 请求标识注入日志

- **WHEN** 一个请求到达网关
- **THEN** 所有该请求相关的日志行 SHALL 包含同一个 `request_id` 字段

#### Scenario: 请求标识注入 SHM 载荷

- **WHEN** 网关发布帧到 SHM 图片通道
- **THEN** protobuf 消息中 SHALL 包含 `request_id` 字段，值为该请求的唯一标识
