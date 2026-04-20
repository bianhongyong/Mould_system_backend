## ADDED Requirements

### Requirement: SHM 总线实现 MUST 分离控制面与数据面职责

系统 MUST 将共享内存总线拆分为主进程控制面组件与业务进程数据面组件。控制面 MUST 负责通道段创建、初始化、生命周期治理与状态管理；数据面 MUST 负责附着、发布订阅与回调执行。

#### Scenario: 主进程初始化通道段，业务进程仅附着使用

- **WHEN** 主进程完成通道拓扑初始化并启动业务进程
- **THEN** 业务进程仅执行 attach/use/detach，不重新创建全局生命周期状态

### Requirement: 数据面组件 MUST 在主进程单例化并支持 COW 复用

系统 MUST 在主进程维持唯一数据面组件实例，并在 fork 前完成只读基础状态预初始化。子进程 fork 后 MUST 复用该只读状态并执行本地后附着初始化，不得在 fork 前启动数据面线程化执行资源。

#### Scenario: 子进程复用主进程数据面只读状态

- **WHEN** 主进程完成数据面单例预初始化并 fork 拉起业务子进程
- **THEN** 子进程复用主进程只读数据面状态，且仅在子进程内创建 eventfd/reactor 等进程本地资源

### Requirement: 通道命名 MUST 固定且 `shm_unlink` MUST 仅由主进程在最终退出路径执行

系统 MUST 按“模块名+通道名”生成通道共享内存名称。`launch_plan` 一旦确定，该命名映射 MUST 保持稳定。

在**运行期**（主进程尚未进入最终退出阶段），数据面组件 MUST NOT 执行 `shm_unlink`，主进程控制面 MUST NOT 执行 `shm_unlink`。

主进程进入**最终退出**阶段时，主进程 MUST 在子进程已全部回收或已停止附着之后，对受管通道集合执行 `shm_unlink`，且 MUST 保证幂等。

#### Scenario: 业务进程正常退出

- **WHEN** 业务进程退出并释放本地资源
- **THEN** 进程仅关闭/解除映射本地句柄，不执行 `shm_unlink`

#### Scenario: 主进程运行期

- **WHEN** 主进程处于正常运行并监督子进程
- **THEN** 主进程不执行 `shm_unlink`

#### Scenario: 主进程最终退出

- **WHEN** 主进程进入最终退出阶段且子进程已处理完毕
- **THEN** 主进程对受管通道执行 `shm_unlink`，且重复调用不产生额外副作用

### Requirement: 运行模型 MUST 固化为 fork-only 且单进程单模块

系统运行模型 MUST 满足一进程仅运行一个模块实例（1 process = 1 module），并且业务进程启动路径 MUST 使用 fork-only，MUST NOT 使用 exec。

#### Scenario: 进程模型检查

- **WHEN** 启动器拉起业务模块实例
- **THEN** 每个子进程仅承载一个模块实例且不进入 exec 路径