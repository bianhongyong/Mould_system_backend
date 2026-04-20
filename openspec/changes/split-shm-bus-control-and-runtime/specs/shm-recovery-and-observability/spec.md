## MODIFIED Requirements

### Requirement: Data plane SHALL expose daemon-callable consumer online control primitives
消费者在线控制原语 SHALL 由主进程控制面统一驱动并对数据面生效。控制面调用添加/移除操作时，系统 SHALL 执行参数校验、显式状态迁移，并在移除后触发一次 reclaim 推进以维持通道可回收性。

#### Scenario: 主进程将异常消费者下线
- **WHEN** 主进程检测到消费者所属业务进程异常退出并触发 remove
- **THEN** 该消费者状态迁移为 `OFFLINE`，最小读序列计算不再包含该消费者，并执行一次 reclaim 推进

## ADDED Requirements

### Requirement: 消费者实例身份 MUST 使用 `(owner_pid, owner_start_epoch)` 组合判定
系统 MUST 使用 `(owner_pid, owner_start_epoch)` 作为消费者实例唯一身份。任何在线状态确认、离线回收与槽位复用判定 MUST 基于该组合，而非仅基于 PID。

#### Scenario: PID 复用不误判旧消费者在线
- **WHEN** 旧消费者退出后其 PID 被新进程复用，但 `owner_start_epoch` 不同
- **THEN** 系统将其识别为不同实例，不会将旧实例状态错误沿用到新实例

### Requirement: 动态消费者变更 SHALL 保持通道状态一致且无泄漏
系统 SHALL 支持消费者动态上线/下线与重建，在高频增删场景下维持通道状态一致，并且 MUST 保证无句柄泄漏、无槽位泄漏、无僵尸在线状态。

#### Scenario: 高频动态增删消费者压力
- **WHEN** 在持续发布流量下反复进行消费者上线、下线与重建
- **THEN** 通道成员状态保持一致，资源使用稳定，无增长性泄漏
