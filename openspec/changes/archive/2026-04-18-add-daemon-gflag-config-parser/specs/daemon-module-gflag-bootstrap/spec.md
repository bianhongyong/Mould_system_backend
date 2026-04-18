## ADDED Requirements

### Requirement: 解析器 SHALL 根据总配置文件路径与各模块 I/O 路径完成全量解析

解析器 MUST 接收**总配置文件路径**（由调用方提供），并从中读取**唯一**总配置文件 `launch_plan.txt`（文件名可为约定名，实际打开路径以调用方传入为准）。总文件 MUST 以字典形式包含全部业务模块的配置；每个模块子字典 MUST 包含 `module_name`、`resource`、`module_params`，以及指向该模块独立 I/O 配置文件的**路径字段**（字段名由 schema 固定，例如 `io_channels_config_path`）。解析器 MUST 对每个模块根据该路径加载 I/O 文件，并在 I/O 文件中读取并校验 `input_channel` 与 `output_channel`。解析器 MUST 在一次成功的解析流程中处理**所有**模块；对未知键、非法路径、文件缺失或格式错误 MUST 失败即停。

#### Scenario: 相对路径相对于总配置所在目录解析

- **WHEN** 某模块子字典中 I/O 路径为相对路径（例如 `channels/mould_io.txt`）
- **THEN** 解析器以总配置文件所在目录为基准解析为绝对路径并成功打开该文件（文件存在且可读时）

#### Scenario: 解析器校验总文件中的单模块块

- **WHEN** 解析器读取总配置中某一模块子字典
- **THEN** 它校验 `module_name`、`resource`、`module_params` 及 I/O 路径字段的存在性与类型，并校验对应 I/O 文件内 `input_channel`、`output_channel`；任一失败时返回错误且不产出有效完整结果

#### Scenario: 总文件包含多模块时全部解析

- **WHEN** 总配置中存在多个模块子字典
- **THEN** 解析器 MUST 为每个模块加载其 I/O 文件并产出按模块划分的参数与通道视图，供调用方消费

### Requirement: 配置键名 MUST 与工程内已注册的 GFLAG 名一致（数据契约）

`resource` 与 `module_params` 中出现的**每个键名** MUST 与工程某头文件/编译单元中已通过 gflags **事先注册**的 flag 名称一致。解析器在输出结构中 MUST 保留**完全相同的键名字符串**，以便赋值侧按同名写入 GFLAG。解析器 MUST NOT 引入别名或隐式重命名。若总配置导致多模块对同一 GFLAG 键产生不可调和的冲突，解析器 MUST 在校验阶段报错。

#### Scenario: 输出键名与 GFLAG 名一致

- **WHEN** 某模块子字典包含 `resource.startup_priority: 80`
- **THEN** 解析器输出的该模块 `resource` 映射中 MUST 包含键 `startup_priority` 与值 `80`

### Requirement: 测试程序主进程 SHALL 根据解析结果对 GFLAG 赋值

多进程测试程序的**主进程**在 `fork` 子进程之前（除非设计文档明确约定其它顺序），MUST 根据解析器输出的 `resource`/`module_params` 键值对，将每个键的值写入**同名**的已注册 GFLAG。若某键在 gflags 注册表中**不存在**对应 flag，测试 MUST 失败并报告该键名。

#### Scenario: 主进程将解析结果写入 GFLAG

- **WHEN** 解析成功且某键 `startup_priority` 在 gflags 中已注册
- **THEN** 测试主进程完成赋值后，`startup_priority` 的当前值与配置文件解析值一致

#### Scenario: 配置键无对应 GFLAG 时测试失败

- **WHEN** 解析结果包含键 `unknown_flag_key` 且 gflags 中未注册同名 flag
- **THEN** 测试主进程在赋值阶段失败并输出包含该键名的错误信息

### Requirement: 解析器 MUST 聚合全部模块的 input/output 通道用于共享内存分配准备

解析器在完成所有模块 I/O 文件加载与解析后 MUST 聚合全量 `input_channel` 与 `output_channel`，形成可供共享内存分配逻辑消费的全局通道拓扑数据结构。若不同模块对同名通道声明了不一致的类型或关键参数，解析器 MUST 报错并终止成功解析。

#### Scenario: 多模块通道聚合成功

- **WHEN** 总配置及全部模块 I/O 文件解析与校验通过
- **THEN** 解析器输出包含全局通道拓扑，且无未解决的跨模块冲突

#### Scenario: 通道冲突阻断成功解析

- **WHEN** 两个模块对同名通道声明了不一致的类型或关键参数
- **THEN** 解析器输出错误详情且不返回成功状态下的完整结果

### Requirement: 多进程测试程序（主进程 fork）SHALL 验证解析器与 GFLAG 在多进程下的行为

本变更 MUST 包含一个**多进程**测试程序（或同等测试目标）：由**主进程**完成解析与 GFLAG 赋值后，使用 `fork` 创建至少一个**子进程**，在父子进程按设计文档约定的分工下继续验证（例如子进程读取已继承的 GFLAG 或再次调用解析器等）。该程序**不是**正式守护进程。

#### Scenario: fork 后子进程按约定参与验证

- **WHEN** 测试主进程已完成解析与 GFLAG 赋值并 `fork` 出子进程
- **THEN** 子进程侧验证行为符合设计文档约定，且测试主进程可通过退出码或进程间约定收集成功/失败结论

#### Scenario: 测试主进程回收子进程

- **WHEN** 子进程结束运行
- **THEN** 测试主进程 MUST 使用 `waitpid`（或等价机制）回收子进程，避免僵尸进程

