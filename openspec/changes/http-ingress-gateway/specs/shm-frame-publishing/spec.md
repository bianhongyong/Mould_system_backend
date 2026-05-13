## ADDED Requirements

### Requirement: 作为图片通道生产者

网关 SHALL 作为指定图片通道的生产者，遵守 ChannelTopology 对通道名、生产者唯一性、槽位配置的约束。

#### Scenario: 启动时通道校验

- **WHEN** 网关启动并解析通道拓扑配置
- **THEN** 网关声明的图片通道 SHALL 在拓扑中定义为该通道的唯一生产者，与其他模块无冲突

#### Scenario: 违反生产者唯一性

- **WHEN** 拓扑配置中同一图片通道存在多个生产者声明（含网关）
- **THEN** ChannelTopology 校验 SHALL 失败，网关 SHALL NOT 启动

### Requirement: SHM Publish 成功即确认

网关 SHALL 在 SHM Publish 成功（按运行时既有提交语义）后，对该请求返回 HTTP 200。

#### Scenario: Publish 成功返回 200

- **WHEN** 校验通过且 SHM Publish（两阶段提交）成功完成
- **THEN** 网关 SHALL 返回 HTTP 200，响应体包含 `request_id` 和 `image_id`

#### Scenario: Publish 失败返回错误

- **WHEN** SHM Publish 因背压或槽位满而失败
- **THEN** 网关 SHALL 返回 HTTP 503，响应体包含错误类型 `BACKPRESSURE` 和 `request_id`

### Requirement: 多线程 Publish 安全

网关 SHALL 支持在多线程环境下执行 SHM Publish，以 SHM ring buffer 运行时库的线程安全契约为准。

#### Scenario: 多请求并发 Publish

- **WHEN** 多个 HTTP 请求同时完成校验并进入 SHM Publish 阶段
- **THEN** 各 Publish 操作 SHALL 独立完成，无数据竞争或游标冲突（依 SHM 运行时契约）

### Requirement: HTTP 200 语义契约

HTTP 200 SHALL 仅表示请求已被接受且图片已成功发布到 SHM 图片通道，SHALL NOT 隐含 OSS 上传已成功。

#### Scenario: OSS 失败仍返回 200

- **WHEN** SHM Publish 成功但后续异步 OSS 上传失败
- **THEN** 网关仍返回 HTTP 200（OSS 失败单独记录在日志/指标中）

#### Scenario: SHM 失败不返回 200

- **WHEN** SHM Publish 失败
- **THEN** 网关 SHALL NOT 返回 HTTP 200，且 SHALL NOT 发起 OSS 上传
