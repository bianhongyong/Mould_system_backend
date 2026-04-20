# SHM recovery and observability

## Purpose

Daemon-callable consumer membership control, generation-safe quorum updates, and shared-memory health metrics.

## Requirements

### Requirement: Data plane SHALL expose daemon-callable consumer online control primitives
Consumer online control primitives SHALL be driven by the parent-process control plane and applied to the data plane. On add/remove calls, the system SHALL perform parameter validation, explicit state transition, and one reclaim advancement after remove to preserve reclaimability.

#### Scenario: Parent process offlines abnormal consumer
- **WHEN** parent process detects abnormal exit of a consumer owner process and triggers remove
- **THEN** consumer transitions to `OFFLINE`, minimum-read sequence no longer includes that consumer, and one reclaim advancement is executed

### Requirement: Consumer instance identity MUST use `(owner_pid, owner_start_epoch)` tuple
The system MUST use `(owner_pid, owner_start_epoch)` as the unique consumer instance identity. Online confirmation, offline reclaim, and slot reuse decisions MUST be based on this tuple instead of PID alone.

#### Scenario: PID reuse does not misclassify old consumer as online
- **WHEN** old consumer exits and its PID is reused by a new process with different `owner_start_epoch`
- **THEN** the system identifies them as different instances and does not carry old instance state into the new one

### Requirement: Dynamic consumer changes SHALL keep channel state consistent and leak-free
The system SHALL support dynamic consumer add/remove/recreate operations while preserving channel-state consistency under high-frequency membership changes, and MUST guarantee no handle leaks, no slot leaks, and no zombie-online states.

#### Scenario: High-frequency dynamic consumer churn
- **WHEN** consumer online/offline/recreate operations are repeatedly executed under continuous publish traffic
- **THEN** membership state remains consistent and resource usage stays stable without growth leaks

### Requirement: Membership updates MUST be atomic without global read-write freeze
Active consumer set updates SHALL be published via generation-based atomic switch, so add/remove operations do not require pausing producer/consumer data operations.

#### Scenario: Consumer add/remove does not expose half-updated quorum view
- **WHEN** daemon performs consumer add/remove and publishes a new membership generation
- **THEN** reclaim quorum calculations read either old generation or new generation, never a mixed intermediate state

### Requirement: Consumer lifecycle SHALL use simplified online/offline model
The middleware SHALL use only `ONLINE` and `OFFLINE` consumer states. Rejoining is treated as adding a new consumer entry, and historical message catch-up is not required.

#### Scenario: Rejoined consumer starts from latest slot
- **WHEN** a previously removed consumer is added again
- **THEN** middleware treats it as a new consumer and initializes its cursor at the latest ring slot, so messages produced during offline period are not replayed

### Requirement: Shared memory health metrics MUST be observable
The middleware MUST expose metrics for buffer occupancy, producer block duration, consumer lag, reclaim counts, membership transition counts, and online/offline events.

#### Scenario: Monitoring captures IPC pressure signals
- **WHEN** observability backend scrapes middleware metrics
- **THEN** it receives current occupancy, lag, and recovery counters for each active channel
