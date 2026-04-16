# SHM recovery and observability

## Purpose

Daemon-callable consumer membership control, generation-safe quorum updates, and shared-memory health metrics.

## Requirements

### Requirement: Data plane SHALL expose daemon-callable consumer online control primitives
The middleware data plane SHALL expose consumer online control primitives for daemon invocation, including consumer add and remove operations with parameter validation and explicit transition rules.

#### Scenario: Daemon removes offline consumer through data-plane control API
- **WHEN** daemon decides a consumer is offline and calls remove
- **THEN** middleware transitions the consumer state to `OFFLINE`, excludes it from minimum-read calculations, and triggers one reclaim pass

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
