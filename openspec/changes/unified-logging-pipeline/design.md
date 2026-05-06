## Context

- 仓库已采用 `backend_main` + `Supervisor::ForkModuleProcess` 的 **fork-only、单进程单模块** 模型；子进程与主进程之间已有 **READY 管道**（`ReadyPipeProtocol`），与日志无关。
- `mould::InitApplicationLogging` 当前固定 `FLAGS_logtostderr = 1`，日志默认进 **stderr**。
- 已讨论并拍板：**pipe 拓扑**、**允许丢日志** 的背压、**backend_main 与子进程同一套落盘**、日志进程内 **`LogDumpManager`** 负责异步写盘/滚动/压缩及 tmpfs 两阶段写入。

## Goals / Non-Goals

**Goals:**

- 生产级统一落盘：多模块 + 主进程日志进入同一套目录与滚动策略。
- 主进程仅负责 **fd 编排与生命周期**，不在用户态中继日志字节。
- 高负载下 **优先不阻塞业务线程**；丢日志时 **可计数、可观测**。
- 崩溃/掉电场景下尽可能利用 tmpfs → rename 降低半成品文件风险，并处理残留临时文件。

**Non-Goals:**

- 替代集中式日志平台（ELK、Loki 等）的全部能力；本变更聚焦本机文件管线。
- 跨主机日志聚合、远程 syslog 默认开箱（可作为后续变更）。
- 改变 SHM 通道拓扑或 IPC protobuf 消息形态（日志走独立 pipe，不走 SHM 业务通道）。

## Decisions

### D1：传输形态 — 每进程一条匿名 pipe，主进程保留 write 端不关闭

- **内容**：对 `backend_main` 与每个业务子进程各 `pipe()`（匿名 pipe），日志进程独占所有读端，通过 `poll`/`epoll` 区分来源并映射 **逻辑模块名**（子进程用 `launch_plan` 的 `module_name`，主进程用固定名如 `backend_main`）。**关键约束：主进程 `fork` 业务子进程后，保留 write_fd 不关闭。** 这意味着：
  - 主进程持有全部 write_fd；子进程通过 `dup2` 获得自己的 write_fd 副本
  - 子进程 crash 后，OS 关闭其 write_fd 副本，但主进程仍持有 write_fd → pipe 不断裂
  - 重启时新子进程 `dup2` 同一个 write_fd，日志模块读端无变化，无需重新注册
- **理由**：满足「不经主进程用户态转发」；读侧按 fd 区分模块；且**进程重启时日志管道无需重建映射**，消除控制通道与竞态条件。
- **备选**：主进程 fork 后关闭 write_fd（需控制通道传递新 read_fd，更复杂）。

### D2：子进程衔接方式 — 优先 `dup2` 写端到 `STDERR_FILENO`

- **内容**：在子进程入口最早阶段（任何 `LOG` 之前）将 pipe 写端 `dup2` 到 stderr；或保留专用 fd 并配合 glog `LogSink`/`FLAGS_logtostderr` 组合；**规范上推荐**「dup2 到 stderr + 保持 `logtostderr`」以兼容第三方库写 stderr 的行为。
- **理由**：最小侵入、与现有 `InitApplicationLogging` 习惯一致。
- **风险**：`dup2` 与 `InitGoogleLogging` 顺序必须在实现清单中固定并加测试。

### D3：日志进程内分层 — `LogCollector`（IO）与 `LogDumpManager`（落盘）分离

- **内容**：IO 线程仅负责 `read`、**按行缓冲**、组帧后调用 `AddLog`；`LogDumpManager` 负责队列、`std::shared_ptr<std::string>` 持有大字符串、异步写线程、`LogDump`/`GenLogFile`、压缩线程池与 `hist_files_`。
- **理由**：避免 epoll 回调里直接写盘阻塞读路径。

### D4：`LogDumpManager` 行为对齐已讨论接口

- **Init**：创建压缩线程池（若启用）、确保目录、`CleanInvalidDumpFilesByFormat`、扫描历史、`hist_files_` 排序、处理临时目录残留、启动异步写线程。
- **AddLog**：线程停止快路径；可选过滤；单条超限丢弃；加锁入队；队列接近满时唤醒写线程。
- **写线程唤醒**：队列接近满、flush barrier、`stop`、溢出标记、超时（如 1000ms）批处理。
- **文本 `LogDump`**：`{module}\t>> {line}\n`，按 `\n` 拆分；定期 `flush`/`fsync` 策略按大小与时间阈值。
- **二进制 `LogDumpManagerRaw`（可选）**：`int32_t` 长度前缀 + payload，与文本模式通过配置互斥或分文件扩展名。
- **GenLogFile**：关文件 → flush/fsync → tmpfs 文件 rename 到最终路径 → 异步压缩 → 裁剪 `max_file_count` → 打开新文件；磁盘空间不足时跳过并计数/告警。
- **历史**：`hist_files_` 为按时间升序的 `std::list<std::string>`，支持插入/删最老。

### D5：背压 — 允许丢日志

- **内容**：写端 **非阻塞**（或等价：`write` 遇 `EAGAIN` 即丢弃并递增 `dropped_*`）；`LogDumpManager` 内存队列满时丢弃新条目或最旧条目（**须在实现中二选一并写入规格**）；周期性汇总 WARNING 或导出指标钩子。
- **理由**：用户明确「宁可丢日志、不可卡实时链路」。

### D6：日志进程自引用 — `google::LogSink`

- **内容**：`LogSink::send` 仅入队到 `LogDumpManager`，**禁止**在 sink 内再次 `LOG`。
- **理由**：避免死锁与递归。

### D7：启动顺序

- **内容**：主进程先创建全部 pipe 端点并 fork **日志模块进程**，待其 **collector 就绪**（自建握手：第二根 ready 管、eventfd、或读端首次注册完成信号）后，再对 `backend_main` 自身 `dup2` 并启动其余按 `startup_priority` 的模块批次。
- **理由**：避免子进程早写导致内核 pipe 缓冲区满而大量丢弃或阻塞（若误配阻塞）。
- **与 READY 关系**：业务模块仍使用现有 `ReadyPipeProtocol`；日志模块可有独立就绪条件，但 MUST 不破坏「高优先级批次 READY 门控低优先级批次」的既有语义（见 delta 规格）。

### D8：`launch_plan` 中的日志模块

- **内容**：日志模块为普通注册模块名（如 `Logging_module`），`startup_priority` 数值 **低于** 依赖日志输出的业务模块（数值更小表示更先启动，与现有语义一致），或在同批内明确日志先于其他随机模块的**确定性前置**规则（二选一，推荐 **单独更低优先级批次** 以兼容随机化）。

### D9：异常边界 — 进程 crash 与 pipe fd 生命周期

- **核心原则**：主进程在 `fork` 业务子进程后 **保留 write_fd 不关闭**。pipe 的生命周期由主进程持有决定，不绑定于某个子进程的存活。
- **各角色 fd 持有清单**：

  | 角色 | 持有 fd | 说明 |
  |------|---------|------|
  | 主进程 | 全部 write_fd（N 个）+ 自身独立 write_fd | fork 后不关闭 |
  | 业务子进程（存活时） | 自己的 write_fd（通过 dup2 到 stderr） | crash 后 OS 自动关闭 |
  | 日志模块 | 全部 read_fd（N+1 个，含主进程） | 启动时注册，永久不变 |

- **进程 crash → restart 流程**：
  1. 子进程异常退出 → OS 关闭其持有的 write_fd 副本
  2. 主进程 `SIGCHLD` 处理 → Supervisor 判定重启策略（退避/熔断）
  3. **创建新管道?** → 否。主进程仍持有 write_fd，原有 pipe 有效
  4. `fork` 新子进程 → `dup2(write_fd, STDERR_FILENO)` → 新子进程向同一条 pipe 写入
  5. 日志模块视角：read_fd 始终可读，无任何变化
- **fd 继承安全策略**：
  - 子进程 fork 后在其入口关闭所有不应持有的其他模块 write_fd（通过 fd 白名单或范围约定）
  - 主进程持有 write_fd 默认不设 `FD_CLOEXEC`，但需审计确保仅目标子进程继承到对应 write_fd
- **Supervisor 熔断后清理**：
  - 当 Supervisor 判定某模块"不再重启"（达到熔断阈值或 `launch_plan` 配置无重启策略），主进程 MUST 关闭该模块对应的 write_fd
  - 日志模块读端此时收到 `POLLHUP` → 按单路连接终止处理（关闭读端、从 epoll 移除、记录事件）
  - 此路径在正常运行中极少触发，但必须实现以释放日志模块 fd
- **竞态条件分析**：
  - **新子进程先写入 vs 旧数据残留在 pipe 缓冲区** → 不构成竞态。pipe 保序，日志模块按序读出，旧数据在前、新数据在后，且"允许丢日志"原则下无需分割标记
  - **日志模块自身 crash** → 主进程持有的 write_fd 变成孤儿（有写端无读端，`SIGPIPE` 若未屏蔽会终止主进程）。主进程 MUST `sigignore(SIGPIPE)` 或在重启日志模块前关闭全部旧 write_fd，重建整个 pipe 拓扑
  - **多个模块同时 crash** → 各自独立处理，pipe 不互相影响

## Risks / Trade-offs

- **[Risk] 丢日志导致现场难以复盘** → Mitigation：丢弃计数、模块维度指标、磁盘/队列水位日志；运维文档明确策略。
- **[Risk] 非阻塞写 + glog 组合行为不一致** → Mitigation：集中封装「进程日志出口」初始化单元测试覆盖。
- **[Risk] fd 泄漏或子进程继承错误写端** → Mitigation：子进程入口关闭无关 write_fd 的审计表与代码审查清单。`FD_CLOEXEC` 不作为安全边界（fork-only 模式下子进程继承全部 fd）。
- **[Risk] 主进程持有 write_fd 使 pipe 不断裂，日志模块无法通过 EOF 感知子进程退出** → Mitigation：日志模块不需要感知进程退出即可正常工作；运维监控通过 Supervisor 的进程管理而非管道 EOF 获知进程状态。
- **[Risk] 日志模块 crash 时主进程收到 SIGPIPE** → Mitigation：主进程 MUST `sigignore(SIGPIPE)` 或以 `MSG_NOSIGNAL`/`SO_NOSIGPIPE` 等效处理；重启日志模块前关闭所有旧 write_fd。
- **[Risk] 熔断后 write_fd 未关闭成为孤儿** → Mitigation：Supervisor 判定不再重启后 MUST 关闭对应 write_fd，确保日志模块可回收读端 fd。
- **[Risk] tmpfs 满导致 rename 失败** → Mitigation：回退直接写最终路径或停写并告警（实现阶段选定并写入规格）。
- **[Trade-off] `std::async` 生命周期** → 实现阶段可改为 `jthread` + condition_variable；设计不强制。

## Migration Plan

1. 新增 `launch_plan` 日志模块条目与 gflags/路径配置；默认关闭新管线时保持旧行为（若需 **BREAKING** 完全切换，可在实现时以 flag 控制双轨一周）。
2. 部署时创建日志根目录与（可选）tmpfs 挂载点；校验磁盘配额与权限。
3. 灰度：先在测试环境验证高负载丢日志计数与滚动压缩正确性，再上生产。
4. 回滚：关闭日志模块或回退 `launch_plan` 与二进制；恢复纯 stderr 终端输出。

## Open Questions

- `LogDumpManager` 队列满时丢弃 **最新** 与丢弃 **最旧** 的策略选择（影响延迟 vs 新鲜度）。
- 压缩格式（tar.xz / gz / lz4 / zst）与是否依赖外部进程/库的最终选型。
- 日志模块是否参与与普通模块完全相同的 SHM 拓扑（若不需要消费业务通道，是否使用空/最小拓扑）。
