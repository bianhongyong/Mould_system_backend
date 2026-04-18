# Tasks

## 1. 配置结构与解析器实现

- [x] 1.1 定义总配置文件路径语义：解析器 API 由调用方传入 `launch_plan` 的完整路径（或项目约定的等价方式），作为打开总配置的唯一依据。
- [x] 1.2 定义 `launch_plan.txt` 结构：顶层字典内含全部模块子字典（不使用数组）；每模块含 `module_name`、`resource`、`module_params`，以及指向独立 I/O 文件的路径字段（名称在 schema 中固定并与文档一致）。
- [x] 1.3 定义模块 I/O 配置文件格式：单文件内字典形式包含该模块的 `input_channel` 与 `output_channel`。
- [x] 1.4 实现路径解析：各模块 I/O 路径写在总配置中；相对路径 MUST 相对于总配置文件所在目录解析，绝对路径按文件系统解析；路径无效或文件缺失时失败即停并报告已解析绝对路径（若适用）。
- [x] 1.5 实现解析器：加载总配置 → 逐模块加载 I/O 文件 → 全模块校验 → 通道聚合与跨模块冲突检测。
- [x] 1.6 定义解析器对外 API 与输出数据结构文档；明确多进程（`fork`）与 GFLAG 赋值顺序约定（供多进程测试与后续守护进程集成对齐）。

## 2. GFLAG 契约与测试主进程赋值

- [x] 2.1 保证输出结构中 `resource`/`module_params` 的 key 与工程内已注册 GFLAG 名一致（无别名）；文档说明配置键须与头文件中的 `DEFINE_*` 等声明保持同步。
- [x] 2.2 完成标量类型解析（string/int/float/bool）及错误上报。
- [x] 2.3 在多进程测试程序主进程中实现：解析成功后，在 `fork` 前将各键值写入同名 GFLAG（使用 gflags 支持的合法 API）；对注册表中不存在的键失败退出。
- [x] 2.4 不实现守护进程正式程序内的 GFLAG 赋值与生产级运行期锁定（由后续变更接入）。

## 3. 多进程测试程序（主进程 `fork`）

- [x] 3.1 实现独立多进程测试程序（或 CMake 测试目标）：主进程完成解析与 GFLAG 赋值后，通过 `fork` 创建若干子进程，按文档约定继续验证（传入总配置文件路径，覆盖相对模块 I/O 路径场景）。
- [x] 3.2 覆盖至少一种多进程模式（例如：子进程读取继承的 GFLAG；或子进程再次调用解析器——与 1.6 约定一致）。
- [x] 3.3 正确处理子进程退出与 `waitpid`（或等价）回收，避免僵尸进程；将测试纳入可自动化执行流程（如 `ctest` 或项目约定方式）。

## 4. 单元测试（一条单测对应一条行动项）

以下单测均写在解析器所在模块的测试目录下；每条 `- [ ]` 对应一个测试用例（或一个 `TEST`/`TEST_F` 等），实现时保持一一映射，便于评审与回归。

- [x] 4.1 单测 `ParseSuccess_SingleModule_Minimal`：总配置仅含一个模块、`resource`/`module_params` 仅含合法最小字段，I/O 文件含完整 `input_channel`/`output_channel`；断言解析成功且模块视图字段齐全。解决的问题：防止「最小合法配置」在后续改动中被破坏，建立基线行为。
- [x] 4.2 单测 `ParseSuccess_MultiModule_AllIoReachable`：总配置含多个模块，各模块 I/O 路径均有效且内容合法；断言每个模块均被解析且聚合通道拓扑包含全部模块贡献。解决的问题：避免漏解析某一模块或聚合时丢模块，保证全量启动数据完整。
- [x] 4.3 单测 `ParseFails_LaunchPlanFileNotFound`：传入不存在的总配置文件路径；断言返回明确错误（含路径信息），且不产出「部分成功」的聚合结果。解决的问题：运维路径错误时能快速定位，避免静默失败或半成品状态。
- [x] 4.4 单测 `ParseFails_ModuleIoFileNotFound`：总配置合法但某模块 `io_channels_config_path`（或最终字段名）指向不存在文件；断言失败并报告模块标识与解析后的 I/O 绝对路径。解决的问题：通道文件缺失时错误可行动，避免 SHM 分配阶段才崩溃。
- [x] 4.5 单测 `ParseSuccess_RelativeIoPath_ResolvedAgainstLaunchPlanDir`：I/O 为相对路径，工作目录与总配置目录不一致；断言仍相对总配置所在目录解析成功。解决的问题：消除「相对路径相对谁」的歧义，避免仅在当前工作目录下偶发成功。
- [x] 4.6 单测 `ParseSuccess_AbsoluteIoPath`：I/O 为绝对路径且文件存在；断言成功。解决的问题：部署使用绝对路径时解析行为与相对路径一致可靠。
- [x] 4.7 单测 `ParseFails_MissingModuleName`：某模块子字典缺少 `module_name`；断言校验失败。解决的问题：防止匿名模块进入聚合结果，避免下游日志与调试无法关联模块。
- [x] 4.8 单测 `ParseFails_MissingResource`：模块子字典缺少 `resource`；断言失败。解决的问题：强制显式给出资源块，避免默认空表掩盖配置遗漏。
- [x] 4.9 单测 `ParseFails_MissingModuleParams`：模块子字典缺少 `module_params`；断言失败。解决的问题：强制显式给出业务参数块，与 `resource` 对称，避免半套配置进入下游。
- [x] 4.10 单测 `ParseFails_MissingIoPathField`：模块子字典缺少 I/O 路径字段；断言失败。解决的问题：防止未声明通道文件却继续解析，导致空拓扑或越界访问。
- [x] 4.11 单测 `ParseFails_IoFileMissingInputChannel`：I/O 文件缺少 `input_channel`；断言失败。解决的问题：保证输入侧通道定义存在，避免仅输出侧「看似能跑」。
- [x] 4.12 单测 `ParseFails_IoFileMissingOutputChannel`：I/O 文件缺少 `output_channel`；断言失败。解决的问题：保证输出侧通道定义存在，避免发布/消费不对称拖到运行期。
- [x] 4.13 单测 `ParseFails_UnknownKeyInLaunchPlanModule`：模块子字典出现 schema 未定义的顶层键；断言失败（失败即停策略）。解决的问题：防止拼写错误字段被静默忽略，减少「以为配上了其实没生效」。
- [x] 4.14 单测 `ParseFails_InvalidScalarTypeInResource`：`resource` 中某值类型与声明/约定不符（如应为 int 却为非数字字符串）；断言错误信息包含模块键与字段路径。解决的问题：尽早暴露类型错误，避免写入 GFLAG 时才出现难以理解的转换失败。
- [x] 4.15 单测 `ParseSuccess_ScalarTypesRoundTrip`：对 string/int/float/bool 各至少一例合法配置；断言输出结构中类型与值正确。解决的问题：保证标量解析与后续 GFLAG 类型一致，避免窄化/精度问题。
- [x] 4.16 单测 `AggregateChannels_TwoModulesNoConflict`：两模块定义不同通道名；断言全局拓扑同时包含两侧且元数据正确。解决的问题：验证聚合合并逻辑正确，而非仅单模块路径。
- [x] 4.17 单测 `AggregateChannels_FailsOnCrossModuleSameNameInconsistent`：两模块对同名通道给出不一致的关键参数；断言解析失败并带冲突说明。解决的问题：在解析阶段阻断 SHM 尺寸/角色不一致，避免分配后运行期数据损坏。
- [x] 4.18 单测 `ParseFails_MultiModuleGflagKeyConflict`：两模块 `resource`/`module_params` 对同一 GFLAG 键给出冲突值（按 schema 定义的冲突规则）；断言失败。解决的问题：避免多模块抢写同一全局 flag 时行为未定义。
- [x] 4.19 单测 `ParseFails_ModuleNameMismatchWithDictKey`（若 schema 要求模块字典键与 `module_name` 一致）：故意不一致；断言失败。解决的问题：防止总索引与自报模块名漂移，避免运维张冠李戴。
- [x] 4.20 单测 `ErrorMessage_ContainsModuleAndFieldPath`：任选一类校验失败用例；断言错误字符串包含模块标识与字段路径（或项目约定格式）。解决的问题：大规模配置下快速人工排错，减少「只知道失败了不知道哪一段」的成本。
- [x] 4.21 单测 `Fork_MultiChild_EachReadsOwnModuleParamsGflags`：多进程单测（仍放在解析器测试目录或项目约定的单测目标中）：主进程完成 `launch_plan.txt` 与各模块 I/O 的解析后，将各模块 `module_params`（及测试约定内需继承的 `resource`）按同名规则写入 GFLAG；主进程按模块数 `fork` 出与模块个数相同的子进程（每个子进程对应一个模块）；每个子进程绑定唯一模块标识（例如通过环境变量、命令行参数或 `fork` 前写入的进程私有约定，避免子进程间争抢同一全局标识）；每个子进程读取 gflags 中对应该模块 `module_params` 的各键的当前值，并与解析器输出的该模块期望值逐项断言一致。解决的问题：验证「父进程先解析并写 GFLAG → 再 `fork`」后，子进程地址空间中可见的 flag 与该模块配置一致，防止 COW/继承语义或赋值顺序错误导致子进程读到错参；与第 3 节独立测试程序互补（本条强调按模块划分子进程断言）。
