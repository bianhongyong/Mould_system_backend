## MODIFIED Requirements

### Requirement: 模块通道定义 SHALL 从 `.txt` 配置文件加载
每个模块的 `input_channel` 与 `output_channel` SHALL 定义在**该模块独立的 I/O 配置文件**中（与 `launch_plan.txt` 分离）。`launch_plan.txt` 中对应模块子字典 SHALL 包含指向该 I/O 文件的**路径字段**。在共享内存分配前，调用方 SHALL 通过**配置解析器**、以**总配置文件路径**为入口，加载总配置并按各模块路径加载全部 I/O 文件，解析并校验**所有**模块的通道定义，并获得解析器聚合后的全局拓扑数据结构。

#### Scenario: 由总配置路径与模块路径加载单模块通道
- **WHEN** 总配置中模块 `MouldDefectDetector` 子字典包含有效的 I/O 配置文件路径且该文件存在
- **THEN** 解析器加载该文件并输出该模块的 `input_channel` 与 `output_channel`（作为全局结果的一部分）

#### Scenario: 由总配置聚合全部模块通道
- **WHEN** 总配置中存在多个模块子字典且各自 I/O 路径均有效
- **THEN** 解析器 MUST 加载每个模块 I/O 文件、解析其中 `input_channel` 与 `output_channel`，并聚合为全局通道拓扑供共享内存分配准备使用
