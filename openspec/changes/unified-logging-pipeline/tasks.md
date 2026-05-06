## 1. 规格与配置契约

- [x] 1.1 确认 `launch_plan` 中日志模块命名、`startup_priority` 与资源字段（目录、tmpfs 路径、压缩开关、队列长度等）的 schema 与解析路径
- [x] 1.2 将 `design.md` 中仍标记 Open Questions 的项（队列满丢弃最新/最旧、压缩实现选型）收敛为默认值并回写规格或设计

## 2. 主进程与 fd 编排

- [x] 2.1 在 `backend_main` 启动路径实现 pipe 创建、日志模块 fork、`CLOEXEC`/fork 后关闭语义，与现有 READY 管道解耦
- [x] 2.2 实现日志采集端就绪握手，并在就绪后才启动依赖日志的业务优先级批次
- [x] 2.3 为 `backend_main` 自身衔接写端（`dup2` 或等价）并保证与控制台输出策略一致（可配置是否保留 tty）
- [x] 2.3.1 主进程入口处 `sigignore(SIGPIPE)` 或等效处理，防止日志模块 crash 后写入孤儿 pipe 触发进程终止
- [x] 2.4 模块重启路径：主进程保留 write_fd，新子进程 dup2 同一 write_fd 到 stderr，日志模块无感知
- [x] 2.5 Supervisor 熔断后清理：当模块不再重启时关闭 write_fd，触发日志模块端 POLLHUP 清理
- [x] 2.6 日志模块自身 crash 处理：主进程关闭所有旧 write_fd 并重建 pipe 拓扑后再拉起日志模块

## 3. 日志模块进程

- [x] 3.1 新增继承 `ModuleBase` 的日志模块类，完成 `REGISTER_MOULD_MODULE_AS` 注册与最小通道配置（若无需 SHM 业务通道则按项目惯例处理）
- [x] 3.2 实现 `LogCollector`：`epoll`/`poll` 读 pipe、按行缓冲、调用 `LogDumpManager::AddLog`；单路 `POLLHUP` 处理（关闭该读端、从 epoll 移除、丢弃剩余缓冲行）
- [x] 3.3 集成 `google::LogSink` 将本进程 glog 入队，避免递归
- [x] 3.4 进程退出：`LogDumpManager` flush/stop 顺序与信号处理

## 4. LogDumpManager 与落盘

- [x] 4.1 实现 `Init`：目录、非法文件清理、历史扫描、临时目录残留处理、写线程与可选压缩线程池
- [x] 4.2 实现 `AddLog`、有界队列、丢弃计数、大日志丢弃与唤醒条件（队列近满、barrier、stop、超时）
- [x] 4.3 实现文本 `LogDump`（`{module}\t>> ` 前缀）、按大小滚动、`tmpfs` → 最终路径 `rename`、磁盘满行为
- [ ] 4.4 可选：实现 `LogDumpManagerRaw` 与配置互斥
- [x] 4.5 实现异步压缩与 `hist_files_` 维护（排序、删最老、压缩后替换条目）

## 5. 子进程日志出口

- [x] 5.1 封装子进程入口日志初始化：在首次 `LOG` 前完成 pipe 写端与 stderr 衔接，并与 `InitApplicationLogging` 行为协调
- [x] 5.2 非阻塞写与 `EAGAIN` 丢弃计数（按模块）接入

## 6. 文档与运维

- [x] 6.1 更新架构/运维文档：目录权限、tmpfs 建议、丢日志策略说明、回滚步骤
- [x] 6.2 提供示例 `launch_plan` 片段（含日志模块）

## 7. 单元测试与集成验证（Google Test）

以下测试均使用 **GTest** 注册，可通过 `gtest_discover_tests` 或既有 CMake 模式纳入构建；目的为锁定规格行为、防止回归。

- [x] 7.1 **`LogDumpManagerInitCreatesDirectoriesAndHistList`**  
  - **内容**：在临时目录下调用 `Init`，断言根目录与临时目录存在；预置若干合法/非法命名文件，断言非法被清理、合法进入历史列表且排序正确。  
  - **目的**：验证初始化与 `CleanInvalidDumpFilesByFormat`、历史扫描的契约。

- [x] 7.2 **`LogDumpManagerAddLogFormatsTextLines`**  
  - **内容**：向 `AddLog` 投递多行字符串（含内嵌 `\n`），驱动写线程 flush，读取生成文件，断言每行前缀为 `{module}\t>> `。  
  - **目的**：验证文本 `LogDump` 行格式与模块前缀。

- [x] 7.3 **`LogDumpManagerRotationRenamesFromTemp`**  
  - **内容**：配置较小单文件上限与 `log_file_temp_path`，写入超限数据，断言滚动发生、临时文件经 `rename` 出现在最终目录、新文件继续接收日志。  
  - **目的**：验证滚动与两阶段写入路径。

- [x] 7.4 **`LogDumpManagerDropsWhenQueueFullAndIncrementsCounter`**  
  - **内容**：阻塞或减慢写线程（测试注入钩子或队列长度极小），并发调用 `AddLog` 直至队列拒绝，断言丢弃计数单调增加且进程不死锁。  
  - **目的**：验证背压「允许丢日志」与可观测性。

- [x] 7.5 **`LogDumpManagerFlushBarrierNotifiesWaiters`**  
  - **内容**：提交一批日志后发起 flush barrier，断言 barrier 返回前文件已落盘且等待线程被唤醒。  
  - **目的**：验证 flush 屏障与写线程协作。

- [x] 7.6 **`LogCollectorLineBufferingAcrossReads`**  
  - **内容**：向 pipe 分两次 `write` 半行与剩余半行，驱动 collector，断言仅在一行完整时调用 mock `AddLog` 一次。  
  - **目的**：验证行缓冲与半包拼接。

- [x] 7.7 **`LogSinkNoReentrancyDeadlock`**  
  - **内容**：注册 `LogSink` 入队到内存结构，从队列消费时调用 `LOG` 不应导致同线程重入崩溃；可用单线程确定性测试。  
  - **目的**：验证日志进程内 glog → Sink 无递归死锁。

- [x] 7.8 **`PipeNonBlockingWriteIncrementsDropOnEAGAIN`**（可选轻量集成）  
  - **内容**：构造小容量 pipe，非阻塞写端循环写直至 `EAGAIN`，断言包装层丢弃计数增加。  
  - **目的**：验证非阻塞背压路径与计数。

- [x] 7.9 **`SupervisorRestartReusesSameLogPipe`**（若主进程逻辑可单测注入）  
  - **内容**：模拟模块退出并重启，断言 write_fd 不变、日志模块 read_fd 无需重新注册、新子进程通过同一 pipe 写入正常。  
  - **目的**：验证重启时"主进程保留 write_fd、复用同一 pipe"的 fd 生命周期契约。
