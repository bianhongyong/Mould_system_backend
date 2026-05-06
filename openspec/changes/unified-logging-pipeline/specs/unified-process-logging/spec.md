## ADDED Requirements

### Requirement: 系统 MUST 通过每进程独立 pipe 将日志字节流送达日志模块进程且不经由主进程用户态转发

主进程 MUST 在 fork 业务子进程之前创建并保管与各进程配对的匿名 pipe（或等价内核双向字节通道中的半双工 pipe）端点分配策略：每个业务子进程与其写端一一对应；`backend_main` MUST 拥有自身独立写端。日志模块进程 MUST 独占所有读端并在用户态通过 `poll`/`epoll`（或等价）多路复用读取。主进程 MUST NOT 在正常运行路径中对业务日志执行用户态 `read`/`write` 中继。

#### Scenario: 子进程日志进入对应读端

- **WHEN** 某业务子进程向已衔接的 stderr 写入一行 glog 文本
- **THEN** 日志模块进程从与该子进程配对的 pipe 读端可读得该字节流（与内核调度顺序一致），且主进程未对该字节流执行用户态拷贝转发

#### Scenario: 主进程日志使用独立写端

- **WHEN** `backend_main` 产生日志
- **THEN** 其字节流通过为主进程分配的独立 pipe 写端进入日志模块进程，并在落盘内容中以配置约定的逻辑名（例如 `backend_main`）区分于业务模块名

### Requirement: 日志模块进程 MUST 基于 `ModuleBase` 实现并可将采集流量交给 `LogDumpManager`

日志模块 MUST 注册为 `ModuleFactoryRegistry` 中的常规模块名，由 `launch_plan` 启动。其运行期 MUST 包含「采集侧」组件：从 pipe 读端读取原始字节、按行缓冲重组，并将完整逻辑行提交给 `LogDumpManager::AddLog`（或等价 API）。`LogDumpManager` MUST 在独立异步线程中批量写盘并支持停止与 flush 屏障语义。

#### Scenario: 行缓冲避免半行前缀污染

- **WHEN** 单次 `read` 仅返回半条日志行
- **THEN** 采集侧在下一次读数据拼接完成前不向 `AddLog` 提交未完成行

#### Scenario: 读端收到 POLLHUP 时关闭该路并清理资源

- **WHEN** Supervisor 判定某模块不再重启，关闭对应 write_fd，日志模块相应 read_fd 收到 `POLLHUP`（或 `read` 返回 0）
- **THEN** `LogCollector` 的 epoll 返回 `POLLHUP`（或 `read` 返回 0），采集侧 MUST 仅将此 fd 视为单路连接终止：关闭该读端 fd、从 epoll 中移除、并将对应模块的未完成缓冲行丢弃（可计数），MUST NOT 因此触发 LogCollector 整体退出或暂停其余管道的读取

### Requirement: `LogDumpManager` MUST 支持文本落盘格式、异步队列、文件滚动与两阶段写入

文本写路径 MUST 将每条逻辑行写为 `{module_name}\t>> {log_line}\n`（`module_name` 为逻辑模块标识）。`AddLog` MUST 接受可共享的大字符串载体（例如 `std::shared_ptr<std::string>`）以降低拷贝。系统 MUST 维护有界队列；写线程 MUST 在队列接近满、flush 请求、停止标志、溢出标记或超时（默认可配置，例如 1000ms）之一满足时被唤醒并批量落盘。单条日志超过配置的文件大小上限时 MUST 丢弃并计数。

`GenLogFile`（或等价）MUST 在单文件超过配置大小时滚动：关闭当前文件、执行 flush/fsync、若配置临时目录则先将临时文件 `rename` 到最终持久路径再打开新文件；MUST 支持按 `max_file_count`（或等价配置）删除最老历史文件。若配置压缩，MUST 在滚动后将已关闭文件提交异步压缩任务，压缩成功后 MUST 删除明文（或按设计保留一份）并更新历史文件列表。

#### Scenario: 滚动后新日志写入新文件

- **WHEN** 当前文件达到配置大小上限并触发滚动
- **THEN** 后续 `AddLog` 对应内容写入新生成的文件路径

### Requirement: `LogDumpManager::Init` MUST 初始化目录、历史与非法残留清理

初始化 MUST 确保目标日志目录存在；MUST 根据当前配置的命名/格式规则清理不匹配的历史文件（`CleanInvalidDumpFilesByFormat` 或等价）；MUST 扫描合法历史文件并加入 `hist_files_`（或等价结构）并按时间排序；MUST 处理临时目录中上次崩溃遗留的未完成文件（策略：删除、搬移或恢复为可查询状态之一，须在实现与运维文档中固定）；MUST 启动异步写线程；若启用压缩 MUST 创建压缩线程池。

#### Scenario: 格式切换后旧格式文件被清理策略处理

- **WHEN** 配置从格式 A 切换为格式 B 且 Init 运行
- **THEN** 不符合格式 B 规则的历史文件按清理策略被移除或隔离，不再干扰滚动计数

### Requirement: 背压策略 MUST 允许丢弃日志且不默认阻塞业务写路径

当 pipe 写端为非阻塞配置时，若 `write` 返回 `EAGAIN`（或等价不可写），系统 MUST 丢弃该次写入对应的数据并递增可查询的丢弃计数。当 `LogDumpManager` 内存队列无法接受新条目时，系统 MUST 丢弃并计数，且 MUST NOT 将业务线程无限期阻塞在日志提交路径上作为默认策略。

#### Scenario: 可观测丢日志

- **WHEN** 连续高负载导致多次写端 `EAGAIN` 或队列拒绝
- **THEN** 系统暴露按模块（或按 pipe 逻辑源）聚合的丢弃计数，且可通过周期性 WARNING 或指标接口被运维消费

### Requirement: 日志模块进程内 glog MUST 通过 `google::LogSink` 接入同一 `LogDumpManager` 且无递归日志

日志模块 MUST 注册自定义 `LogSink`，在 `send` 路径将格式化消息投递至 `LogDumpManager` 队列。`LogSink` 实现 MUST NOT 调用 `LOG` 或其它会再次进入 `LogSink` 的路径。

#### Scenario: Sink 投递不触发重入

- **WHEN** 日志模块内部调用 `LOG(INFO)` 产生一条消息
- **THEN** 该消息经 `LogSink` 进入队列并最终落盘，且不导致进程崩溃或死锁

### Requirement: 可选二进制落盘模式 MUST 与文本模式在配置上可区分

若实现 `LogDumpManagerRaw`（或等价），其写盘格式 MUST 为序列化的 `[int32_t length][payload]...` 重复结构，且与文本模式通过配置互斥或文件命名/扩展名明确区分，避免历史扫描逻辑混淆。

#### Scenario: 二进制模式写入定长头

- **WHEN** 二进制模式启用且收到一条 payload
- **THEN** 文件中出现先 4 字节小端长度再 payload 的字节序列（具体 endianness 在实现与文档中固定）
