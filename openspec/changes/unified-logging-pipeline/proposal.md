## Why

当前各进程通过 glog 默认 `logtostderr` 将日志写入 stderr，在由 `backend_main` 拉起的多进程模型下，输出易汇聚到启动终端，缺乏统一落盘、滚动、压缩与可运维指标，不满足生产级日志治理需求。需要在不改变既有 fork-only、单进程单模块模型的前提下，引入专用日志进程与集中落盘管线。

## What Changes

- 新增**日志模块进程**（`ModuleBase` 子类，可注册为独立 `launch_plan` 模块），负责从内核管道读取各进程日志字节流并交给落盘管理器。
- **主进程统筹、字节不经主进程用户态转发**：为 `backend_main` 与各业务子进程各建立一条指向日志进程的匿名 **pipe** 写端；子进程在首次产生日志前完成 stderr（或约定 fd）与写端的衔接；日志进程持有多路读端并用 `poll`/`epoll` 多路复用。
- 引入 **`LogDumpManager`（及可选 `LogDumpManagerRaw`）**：有界内存队列、异步写盘线程、按模块前缀的文本行格式、文件大小滚动、`tmpfs` 两阶段写入后 `rename` 到持久目录、可选异步压缩与历史文件数上限；启动时清理格式不匹配残留文件并扫描历史列表。
- **背压策略**：管道/队列饱和时**允许丢弃日志**，并暴露可观测丢弃计数（按模块或按 fd），默认**不阻塞**业务线程写日志路径。
- 日志进程自身 glog 输出通过 **`google::LogSink`** 入队至同一 `LogDumpManager`，避免递归打日志。
- **`backend_main` 纳入同一落盘路径**：主进程持有独立写端，落盘内容以固定逻辑模块名区分（例如 `backend_main`）。
- **`launch_plan` / 启动编排**：约定日志模块的启动优先级或编排顺序，保证日志收集端就绪后再衔接各进程写端（具体顺序见 `design.md`）；主进程在 fork 子进程时正确设置 `CLOEXEC` 与 fd 继承边界。

## Capabilities

### New Capabilities

- `unified-process-logging`：多进程 pipe 汇聚、日志采集与行重组、`LogDumpManager` 异步落盘/滚动/压缩/历史管理、glog `LogSink` 接入、丢日志背压与指标、两阶段 tmpfs 写入契约。

### Modified Capabilities

- `main-process-supervisor-lifecycle`：在保持 fork-only 与 READY 屏障的前提下，增加与日志 pipe 编排、fd 生命周期及重启时管道重建相关的主进程职责说明（以 delta 规格表述，不削弱原有单模块单进程与批次 READY 语义）。

## Impact

- **代码**：`services/backend/main.cpp` 及监管启动路径、`common/include/ms_logging.hpp` 或等价初始化、`common/comm` 监管与模块工厂、新增日志模块与 `LogDumpManager` 实现单元所在目录（具体路径在实现阶段确定）。
- **配置**：`launch_plan` 增加日志模块条目与资源字段（gflags/路径等由设计细化）；运维需配置日志根目录、`tmpfs` 临时路径与压缩选项。
- **依赖**：继续基于 glog/gflags；压缩与线程池依赖现有或新增第三方（在设计中锁定格式与命令行工具边界）。
- **运维**：磁盘满、高负载下可能出现**可观测的丢日志**；需文档化预期。
