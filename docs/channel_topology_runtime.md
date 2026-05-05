# 通道拓扑运行时配置

`ShmPubSubBus` 现在要求在共享内存映射前先加载模块通道拓扑。
`ChannelFactory` 会从环境变量 `MOULD_MODULE_CHANNEL_CONFIGS` 读取模块配置文件。

## 环境变量

- 名称：`MOULD_MODULE_CHANNEL_CONFIGS`
- 格式：`moduleA=/abs/path/moduleA.txt;moduleB=/abs/path/moduleB.txt`
- 分隔符：
  - 模块之间使用 `;`
  - 模块名与配置文件路径之间使用 `=`

如果该变量缺失、为空、格式错误，或指向了无效配置文件，`ChannelFactory::Create()` 会快速失败并返回 `nullptr`。

## 模块 `.txt` 语法

每行表示一条通道声明：

- `output <channel_name> [key=value ...]`
- `input <channel_name> [key=value ...]`

参数示例：

- `slot_payload_bytes=128`
- `delivery_mode=broadcast`（默认）：发布时唤醒所有在线消费者，每条消息每个订阅者各投递一次。
- `delivery_mode=compete`：发布时唤醒所有在线消费者；消息槽内置原子 `claimed` 标志，消费者通过 CAS 抢占该标志，仅抢到者执行 handler；所有消费者均独立推进游标，无需游标同步。取值不区分大小写；非法值在聚合拓扑时失败。

示例（抢占消费通道）：

```txt
output inference.tasks delivery_mode=compete shm_consumer_slots=4
```

支持注释和空行：

```txt
# broker 模块通道配置
output broker.frames slot_payload_bytes=128
input infer.results
```

## 示例

```bash
export MOULD_MODULE_CHANNEL_CONFIGS="broker=/opt/mould/config/broker_channels.txt;infer=/opt/mould/config/infer_channels.txt"
```

然后启动通过 `ChannelFactory` 创建总线的进程。

## 校验与失败场景

运行时在以下情况会拒绝启动：

- 行内角色关键字非法（不是 `input` 或 `output`）
- 通道名包含非法字符
- 单个模块文件内重复声明同一输出通道
- 跨模块对同一通道出现多个生产者
- 跨模块对同一通道出现参数冲突

## 说明

- `comm` 只消费聚合后的拓扑；解析与聚合实现位于 `common/config`。
- 共享内存段容量使用拓扑推导得到的队列深度，而不是单模块的局部假设。

## 核心数据结构

聚合后的拓扑以 `channel` 为主键组织，核心结构如下：

- `ChannelEndpointConfig`
  - `channel`：通道名
- `params`：该声明携带的参数键值对（例如 `slot_payload_bytes`）
- `ModuleChannelConfig`
  - `module_name`：模块名
  - `input_channels`：该模块消费的通道列表
  - `output_channels`：该模块生产的通道列表
- `ChannelTopologyEntry`
  - `channel`：通道名
  - `producers`：该通道的生产者模块列表
  - `consumers`：该通道的消费者模块列表
  - `consumer_count`：该通道消费者数量（`consumers.size()`）
  - `params`：该通道聚合后的参数
- `ChannelTopologyIndex`
  - 类型：`unordered_map<string, ChannelTopologyEntry>`
  - 含义：`channel -> 拓扑条目`

## 聚合实现方法

实现位置：`common/config/src/channel_topology_config.cpp`

### 1) 解析阶段

- 入口：`ParseModuleChannelConfigFile(...)`
- 行语法：`input/output + channel + 可选 key=value`
- 会进行以下校验：
  - 角色关键字必须是 `input` 或 `output`
  - 通道名字符合法
  - 单模块内不允许重复 `output` 同一通道
  - 参数 token 必须是 `key=value`

### 2) 聚合阶段

- 入口：`BuildChannelTopologyIndex(...)`
- 对每个模块执行合并：
  - `output` 合并到对应通道的 `producers`
  - `input` 合并到对应通道的 `consumers`
  - 参数并入通道级 `params`
- 参数冲突规则：
  - 同一 `channel` 下同一个 `key` 若值不同，直接失败
- 生产者冲突规则：
  - 同一 `channel` 若生产者数量大于 1，直接失败
- 最终计算：
  - `consumer_count = consumers.size()`

### 3) 从文件列表直接构建

- 入口：`BuildChannelTopologyIndexFromFiles(...)`
- 输入：`[(module_name, config_path), ...]`
- 流程：逐个解析模块文件 -> 调用聚合函数生成全局拓扑

### 4) 拓扑驱动容量计算

- 函数：`ResolveSlotPayloadBytesForChannel(...)`
- 优先级：
  1. 若配置 `slot_payload_bytes`，直接使用
  2. 否则回退默认槽 payload 大小

注意：`consumer_count` 是**按通道独立计算**，不是全局消费者总数。