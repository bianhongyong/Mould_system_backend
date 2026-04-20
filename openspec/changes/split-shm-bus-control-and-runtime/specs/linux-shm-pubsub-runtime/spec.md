## MODIFIED Requirements

### Requirement: `ShmPubSubBus` SHALL provide true multi-process IPC semantics

Linux SHM 发布订阅运行时 SHALL 在控制面与数据面拆分后继续提供真实跨进程 IPC 语义：控制面负责共享内存通道初始化与生命周期治理，数据面负责跨独立 OS 进程的读写与投递，不得退回进程内队列传输。

#### Scenario: Broker 和 Infer 在不同进程通过拆分后运行时通信

- **WHEN** 控制面已完成通道段初始化，Broker 数据面发布消息且 Infer 运行在另一进程
- **THEN** Infer 通过共享内存读取已提交负载，且消息路径不使用进程内 fallback 传输

### Requirement: Channel factory SHALL resolve single-node mode to Linux SHM runtime

`ChannelFactory` SHALL 在单机模式下解析为 Linux SHM 数据面运行时实例。该数据面实例 SHALL 假定控制面已完成通道生命周期准备，并仅执行 attach/use/detach，不执行 `shm_unlink`；主进程 SHALL 以单例方式持有该数据面基础实例，供 fork 子进程通过 COW 复用只读基础状态。

#### Scenario: 单机模式选择数据面运行时

- **WHEN** 业务进程在单机部署模式下请求总线实例
- **THEN** 工厂返回 SHM 数据面实现，并且该实现不触发全局 `unlink`

#### Scenario: fork 后复用主进程单例基础状态

- **WHEN** 主进程创建单例数据面基础实例后 fork 业务子进程
- **THEN** 子进程可直接复用该基础只读状态，并在本进程完成必要的本地运行时资源初始化

