## ADDED Requirements

### Requirement: OSS 封装层窄接口

网关 SHALL 在阿里云 OSS 官方 SDK 之上实现一层封装（`IBlobStorageUploader` 接口），编排层与业务逻辑 SHALL NOT 直接使用官方 SDK 的公开 API。

#### Scenario: 编排层仅依赖封装接口

- **WHEN** 编排层发起 OSS 上传
- **THEN** 代码中 SHALL NOT 出现 `#include` 阿里云 OSS SDK 头文件；SHALL 仅调用 `IBlobStorageUploader::UploadAsync` 方法

#### Scenario: 封装层可替换

- **WHEN** 在测试环境中注入 `NoOpUploader` 实现
- **THEN** 编排层代码无需修改即可正常工作，上传调用为无操作

### Requirement: 异步 OSS 上传

网关 SHALL 在图片已进入 SHM 发布流程后，将同一图片（或约定副本）异步上传至 OSS。

#### Scenario: 异步上传不阻塞 HTTP 响应

- **WHEN** SHM Publish 成功后发起 OSS 上传
- **THEN** HTTP 200 响应在 OSS 上传完成前即可返回客户端，上传在后台异步执行

#### Scenario: OSS 上传成功

- **WHEN** 异步上传任务执行完成且 OSS 返回成功
- **THEN** 网关 SHALL 记录成功日志，包含 `request_id`、OSS object key、耗时

### Requirement: 上传配置可配

OSS 的 Bucket、对象 key 规则、Content-Type、超时、重试次数与退避策略 SHALL 可通过配置指定。

#### Scenario: 自定义 key 规则

- **WHEN** 配置中指定 key 规则为 `images/{node_id}/{date}/{image_id}.jpg`
- **THEN** 生成的 OSS object key 按规则填充占位符（`node_id` 来自元数据，`date` 为当天日期）

#### Scenario: 上传重试

- **WHEN** OSS 上传因网络超时失败且已配置重试次数为 3
- **THEN** 网关 SHALL 以指数退避（1s, 2s, 4s）重试最多 3 次；全部失败后记录错误并放弃

### Requirement: OSS 失败可观测

OSS 上传失败 SHALL 可观测（日志/指标），且 SHALL NOT 改变「已成功发 SHM 则返回 200」的契约。

#### Scenario: 上传失败日志

- **WHEN** OSS 上传最终失败（含重试耗尽）
- **THEN** 网关 SHALL 记录 ERROR 级别日志，包含 `request_id`、OSS 错误码、失败原因

#### Scenario: 重试中日志

- **WHEN** OSS 上传在重试中
- **THEN** 网关 SHALL 记录 WARNING 级别日志，包含当前重试次数和下次重试时间

### Requirement: OSS 对象可追溯

网关 SHALL 保证 OSS 对象与业务键（如 `image_id` / `request_id`）之间的可追溯映射，通过日志、元数据或侧车存储之一实现。

#### Scenario: 日志中的映射

- **WHEN** OSS 上传成功
- **THEN** 日志中 SHALL 同时包含 `request_id`、`image_id`、OSS bucket 和 object key

### Requirement: 真实 OSS 上传

本期 SHALL 使用真实阿里云 OSS（Bucket: mould-sys）跑通上传全链路（建连、PutObject、可验证对象存在）。OSS 凭证（AccessKey ID/Secret、Endpoint 等）SHALL 在 `OssClientFacade` 源码中直接写死，不做环境变量或配置文件外置。

#### Scenario: 凭证写死在源码

- **WHEN** `OssClientFacade` 初始化 OSS Client
- **THEN** AccessKey ID、AccessKey Secret、Endpoint、Bucket 名称直接从源码常量中读取

#### Scenario: 真实上传可验证

- **WHEN** 网关向真实 OSS 上传图片
- **THEN** 可通过 OSS 控制台或 SDK 下载接口验证对象存在且内容与上传一致

### Requirement: NoOp 上传替代

网关 MAY 提供 NoOp 或本地文件上传实现供 CI 和无网环境使用；SHALL NOT 以 NoOp 作为真实 OSS 验收的唯一手段。

#### Scenario: NoOp 上传器

- **WHEN** 配置中指定使用 NoOp 上传器
- **THEN** `UploadAsync` 调用立即回调成功，不产生任何网络请求

#### Scenario: 本地文件上传器

- **WHEN** 配置中指定使用本地文件上传器且指定输出目录
- **THEN** 上传的图片 SHALL 写入到本地文件系统的指定目录中，文件名为 `{request_id}.jpg`
