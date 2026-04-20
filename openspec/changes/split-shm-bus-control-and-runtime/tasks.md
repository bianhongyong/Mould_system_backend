# Tasks

## 1. 控制面/数据面拆分骨架

- [x] 1.1 新增主进程控制面组件接口与实现骨架（通道创建、初始化、状态治理、统一清理入口）。
- [x] 1.2 新增业务进程数据面组件接口与实现骨架（attach/use/detach、publish/subscribe、回调执行）。
- [x] 1.3 实现主进程数据面单例预初始化路径，仅加载可 COW 复用的只读基础状态。
- [x] 1.4 将现有 `ShmPubSubBus` 的共享内存生命周期逻辑迁移到控制面，保留最小兼容适配层确保编译通过。

## 2. 生命周期与 `unlink` 责任收敛

- [x] 2.1 移除数据面中的 `unlink` 路径，确保业务进程退出仅执行本地 close/unmap/detach。
- [x] 2.2 在主进程最终退出路径实现受管通道集合的统一 `shm_unlink`（子进程回收之后），并保证幂等。
- [x] 2.3 增加运行期防护与日志校验：子进程与主进程运行期路径不得触发 `unlink`；仅允许主进程最终退出阶段的受控清理路径。
- [x] 2.4 明确并实现 fork 后子进程本地运行时资源初始化阶段（eventfd/reactor/句柄绑定），避免 fork 前线程/锁污染。
- [x] 2.5 固化通道命名规则为“模块名+通道名”，并在控制面实现 `launch_plan` 固定后的命名映射冻结。

## 3. 动态消费者一致性协议

- [x] 3.1 在消费者槽位元数据中引入 `owner_pid` 与 `owner_start_epoch` 字段并完成读写路径接入。
- [x] 3.2 实现消费者上线/下线状态迁移与异常退出后的离线回收流程，确保触发一次 reclaim 推进。
- [x] 3.3 调整槽位匹配逻辑为 `(owner_pid, owner_start_epoch)` 组合判定，消除 PID 复用误判风险。

## 4. 调用点迁移与模型约束固化

- [x] 4.1 迁移 `ChannelFactory` 与业务模块接入路径，使单机模式返回数据面运行时实现。
- [x] 4.2 固化 fork-only 与 1 进程 1 模块约束的运行时检查，明确拒绝 exec 路径。
- [x] 4.3 清理或重命名旧总线职责边界相关代码与注释，保证控制面/数据面职责不重叠。

## 5. 测试与验收

- [x] 5.1 单元测试 `ShmUnlinkPolicy_RuntimeVsFinalExit`：共享控制面下运行期发布与数据面析构后 POSIX 段仍存在且 `FinalizeUnlinkInvocationCount` 为 0（数据面不再触发 `FinalizeUnlinkManagedSegments`）；调用 `ShmBusRuntimeFinalizeSharedControlPlaneShm` 后段客体消失且计数递增。目的：验收「运行期不 unlink、仅最终退出统一释放」。
- [x] 5.2 单元测试 `ConsumerCrash_OfflineAndReclaim`：`RingLayoutView::TakeConsumerOfflineIfOwner` 模拟监督侧按 `(pid,epoch)` 强制下线，断言 `consumer_offline_events` 递增且槽位保持 OFFLINE，并可继续 `ReclaimCommittedSlots`。目的：验收异常退出后的离线回收路径。
- [x] 5.3 单元测试 `PidReuse_OwnerEpochDisambiguates`：固定槽位元数据为同一 PID、不同 `owner_start_epoch` 的两代实例，断言回收与匹配仅以二元组为准，避免误将新实例当作旧实例或反向。目的：验收 PID 复用防误判。
- [x] 5.4 单元测试 `DynamicConsumerStress_ConsistencyNoLeak`：由现有 `daemon_membership_observability_test` 等长时动态增删消费者用例覆盖；本 change 回归 `ctest -L unit` 全绿确认无回归。目的：验收动态成员压力下的一致性。
- [x] 5.5 单元测试 `PreforkSingletonCow_AndPostforkLocalInit`：`ShmBusRuntimePreforkEnsureSharedControlPlane` 后 `fork`，子进程 `ShmBusRuntimeGetSharedControlPlane().get()` 与父进程指针一致（地址级 COW 继承）；子进程侧 eventfd/reactor 细粒度用例由 `process_shared_sync_backpressure_test` 覆盖。目的：验收 COW 复用与子进程后初始化约束。
- [x] 5.6 单元测试 `LaunchPlanFrozen_ChannelNamingStable`：`launch_plan` 固定后多次解析通道名，断言均为「模块名+通道名」且重拉起不漂移。目的：验收命名冻结策略。
- [x] 5.7 回归测试：执行现有 SHM pubsub、runtime、observability 相关测试套件并修复兼容问题。目的：防止拆分引入行为回归。
