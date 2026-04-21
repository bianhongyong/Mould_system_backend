# Main process supervisor lifecycle

## Purpose

Define master supervisor process model, startup orchestration, and priority-gated lifecycle behavior.

## Requirements

### Requirement: Master supervisor SHALL run modules in fork-only single-instance processes
The master runtime SHALL start module instances using `fork()` only, and each child process MUST host exactly one module instance derived from `ModuleBase`. The launch plan module entry name MUST resolve to exactly one registered factory.

#### Scenario: One module per child process
- **WHEN** the launch plan contains module `A` and module `B`
- **THEN** the master spawns two child processes and each child initializes exactly one corresponding module instance

#### Scenario: Unknown module registration blocks startup
- **WHEN** launch plan module name `X` has no registered factory
- **THEN** the master rejects startup with an explicit module registration error

### Requirement: Startup orchestration MUST follow priority batches with defined randomization
The master SHALL group modules by `startup_priority` and start lower numeric priority batches first. For first-start within the same priority, order MUST be randomized. During restart handling within the same priority, order MUST be randomized only when the pending restart set size is greater than one.

#### Scenario: First-start randomization within same priority
- **WHEN** three modules share `startup_priority=20` during initial startup
- **THEN** the master starts all three in randomized order for that batch

#### Scenario: Single pending restart is not randomized
- **WHEN** only one module in priority `20` is pending restart
- **THEN** the master restarts that module directly without random selection

### Requirement: Higher-priority batch MUST gate lower-priority startup on READY or failure handling
A priority batch MUST NOT release the next batch until all modules in the current batch become `READY` or enter an explicit failure-handling path defined by restart policy/state transitions.

#### Scenario: Lower priority waits for READY barrier
- **WHEN** priority `10` modules are still in `INITING`
- **THEN** no priority `20` module is started

#### Scenario: Failed module enters policy path and unblocks batch decision
- **WHEN** a priority `10` module fails to report READY before timeout
- **THEN** the module transitions to failure handling and the batch proceeds only according to configured policy outcome
