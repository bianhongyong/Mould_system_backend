## ADDED Requirements

### Requirement: 主进程 MUST 为统一日志管线编排 pipe 端点并在日志收集端就绪后衔接各进程写端

主进程 MUST 在启动依赖日志输出的业务模块批次之前，完成指向日志模块进程的 pipe 读端分配，并确保日志模块进程中负责读取的实现已进入可接受写入的就绪状态（通过独立握手或等价机制约定）。主进程 MUST 为自身与每个将 fork 的业务子进程准备互不共享的写端（业务子进程的写端在子进程内可见；主进程写端仅在主进程内可见），并在 fork 子进程时正确关闭继承集合中不应泄露的 pipe 端（例如子进程不应持有其他子进程的写端）。所有非意图继承的 fd MUST 设置 `FD_CLOEXEC` 或在 fork 后显式关闭，以避免 fd 泄漏与安全风险。

#### Scenario: 日志模块先于依赖其输出的业务批次进入可读就绪

- **WHEN** `launch_plan` 将日志模块配置在低于业务模块的 `startup_priority` 数值批次（更先启动）且日志模块报告采集就绪
- **THEN** 主进程仅在该就绪之后才向业务批次中的子进程发放已衔接的写端语义（含 `dup2` 时机满足设计文档）

#### Scenario: 子进程不继承无关写端

- **WHEN** 主进程 fork 模块 A 的子进程
- **THEN** 该子进程 fd 表中不包含模块 B 的 pipe 写端（除非设计明确允许的调试模式）

### Requirement: 模块重启时复用同一日志 pipe，主进程保留 write_fd

当某业务模块因重启策略被重新 fork 时，主进程 MUST 保持对应 write_fd 打开，不创建新 pipe。日志管道的生命周期绑定于主进程而非单个子进程。新子进程通过 `dup2(write_fd, STDERR_FILENO)` 接入同一条 pipe，日志模块侧的 read_fd 无需任何变更。日志落盘中的模块名始终保持一致。

#### Scenario: 主进程 fork 后保留 write_fd 不关闭

- **WHEN** 主进程为模块 `M` 创建 pipe 并 fork 其子进程
- **THEN** 主进程 MUST 在 fork 后保留 write_fd 不关闭，且子进程通过 `dup2(write_fd, STDERR_FILENO)` 获得其副本

#### Scenario: 子进程 crash 后重启复用同一 pipe

- **WHEN** 模块 `M` 子进程异常退出，OS 关闭其 write_fd 副本
- **THEN** 主进程仍持有 write_fd → pipe 不断裂，日志模块 read_fd 仍在 epoll 中且无 `POLLHUP`；主进程 fork 新子进程后通过 `dup2` 同一 write_fd 继续写入

#### Scenario: 子进程入口关闭无关 write_fd

- **WHEN** 子进程刚 `fork` 完毕，尚未进入模块逻辑
- **THEN** 子进程 MUST 关闭所持有的除自己 write_fd 之外的所有其他模块的 write_fd，避免 fd 泄漏与误写

### Requirement: Supervisor 判定不再重启时关闭对应 write_fd

当某模块达到熔断阈值或 `launch_plan` 配置为不可重启时，Supervisor MUST 关闭其对应的 write_fd，使日志模块 read_fd 收到 `POLLHUP` 并正常清理。

#### Scenario: 熔断后日志模块回收读端 fd

- **WHEN** 模块 `M` 多次重启失败，Supervisor 判定熔断，不再尝试拉起
- **THEN** 主进程 MUST 关闭模块 `M` 的 write_fd；日志模块端 read_fd 收到 `POLLHUP`，从 epoll 移除并关闭该 fd
