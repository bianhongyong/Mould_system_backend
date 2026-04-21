## Context

当前后端模块装配能力分散，模块类与模块名映射缺少统一注册契约，导致新增模块需要修改多处分支代码且无法统一校验重复名与可构造性。
主进程入口流程也尚未形成完整生命周期实现，已有的 `Supervisor`、`ReadyPipeProtocol`、`RestartPolicy` 与 launch_plan 解析能力需要在 `main.cpp` 内统一编排，以满足“分批启动 + 监控重启 + 优雅退出”目标。
此外，需求明确要求通过静态初始化路径完成模块注册，因此注册方案必须在二进制加载阶段建立注册表，`main` 启动时即可查询全部模块名集合。

## Goals / Non-Goals

**Goals:**
- 提供 `ModuleFactoryRegistry`，包含 `Register`、`Create`、`RegisteredNames` 三类只读/只增接口。
- 提供注册宏（如 `REGISTER_MOULD_MODULE`），使 `ModuleBase` 子类通过静态对象自动注册。
- 在编译期/构建期尽可能提前约束：注册类型必须继承 `ModuleBase` 且可构造。
- 重构 `main.cpp` 启动主循环，实现配置校验、分批启动、waitpid 监控、策略重启、优雅退出。
- 将注册表 `RegisteredNames()` 与 launch_plan 解析校验打通，阻止未注册模块进入启动阶段。

**Non-Goals:**
- 不改变模块内部业务处理逻辑与模块间通信协议。
- 不引入多线程内嵌多模块运行模型，仍保持 fork-only、1 进程 1 模块。
- 不新增运行时动态插件加载机制（如 `dlopen`），注册方式限定为编译期接入。

## Decisions

1. 使用“函数局部静态 + 宏触发静态对象构造”实现注册表生命周期  
   - 方案：`ModuleFactoryRegistry::Instance()` 返回函数局部静态单例；注册宏定义一个匿名命名空间静态注册器对象，在构造时调用 `Register`。  
   - 原因：避免静态初始化顺序问题；实现“main 前注册完成”。  
   - 备选：集中 if-else 手工映射。放弃原因：扩展性差，不满足需求“非运行时手工接入”。

2. 类型约束放在模板注册入口，失败即编译报错  
   - 方案：在 `RegisterType<T>()` 内 `static_assert(std::is_base_of_v<ModuleBase, T>)` 与 `static_assert(std::is_constructible_v<T, const ModuleConfig&> || ...实际构造签名...)`。  
   - 原因：尽早失败，防止运行时才发现类型不匹配。  
   - 备选：运行时 RTTI 校验。放弃原因：错误暴露晚，且无法保障可构造性。

3. 重名注册在注册阶段拒绝并记录明确错误  
   - 方案：`Register` 返回状态（或 `StatusOr<void>`）；若同名已存在则返回 `AlreadyExists`，并保留首个定义。  
   - 原因：符合“重复注册必须报错/拒绝覆盖”约束，避免行为不确定。  
   - 备选：后注册覆盖前注册。放弃原因：会引入隐式行为和排查困难。

4. 模块命名规则采用“显式别名优先，默认类名兜底”  
   - 方案：提供两个宏：`REGISTER_MOULD_MODULE(ModuleClass)` 默认类名字符串；`REGISTER_MOULD_MODULE_AS("alias", ModuleClass)` 用于稳定外部配置名。  
   - 原因：同时满足易用性与配置兼容性，减少后续重构改类名的风险。  
   - 备选：仅默认类名。放弃原因：类名变更会破坏 launch_plan 兼容性。

5. `main.cpp` 启动流程采用“批次 READY/FAILED 栅栏 + 随机批内顺序”  
   - 方案：按 `startup_priority` 分组，组内随机；每组等待所有子进程 READY 或 FAILED，再决定是否放行下一组。  
   - 原因：满足需求中明确的栅栏和随机启动约束，同时降低固定顺序耦合。  
   - 备选：全量并行启动。放弃原因：故障定位和依赖控制较弱。

6. 监控循环以 `waitpid` 事件驱动，统一走重启策略评估  
   - 方案：主循环消费退出事件，调用 `RestartPolicy` 计算延迟重启/熔断抑制；主进程始终存活。  
   - 原因：满足 fuse-open 与持续监控需求。  
   - 备选：模块崩溃直接退出主进程。放弃原因：不符合高可用目标。

## Risks / Trade-offs

- [静态注册未链接进最终二进制] -> 通过目标链接清单与注册自检日志校验，必要时采用强引用符号防止被裁剪。  
- [默认类名与配置模块名不一致] -> 明确推荐生产配置使用 `_AS` 别名宏，并在 launch_plan 校验阶段给出差异列表。  
- [重启风暴导致系统抖动] -> 依赖 `RestartPolicy` 的窗口计数与退避策略，触发熔断后保留主进程并输出诊断。  
- [优雅退出期间残留子进程] -> 退出阶段执行分层信号与 `waitpid` 回收，超时后升级信号，确保无僵尸。  

## Migration Plan

1. 引入注册表与宏，并将现有模块逐步切换到宏注册。
2. 为 launch_plan 解析入口增加 `registered_module_names` 参数，先在校验阶段启用告警，再升级为硬失败。
3. 重构 `main.cpp` 为新生命周期流程，同时保持原有 Supervisor/ReadyPipeProtocol/RestartPolicy 的复用接口稳定。
4. 在测试环境验证批次启动、崩溃重启、熔断与退出回收，再推广到默认启动路径。

## Open Questions

- `ModuleBase` 统一构造签名是否已经稳定，是否需要在注册宏层支持多签名适配器。
- 重名注册错误在启动阶段是“立即失败退出”还是“记录并继续（但该模块不可创建）”，需与现有故障策略统一。
