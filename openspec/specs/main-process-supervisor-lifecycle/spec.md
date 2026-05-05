# Main process supervisor lifecycle

## Purpose

Define master supervisor process model, startup orchestration, and priority-gated lifecycle behavior.

## Requirements

### Requirement: Master supervisor SHALL run modules in fork-only single-instance processes
The master runtime SHALL start module instances using `fork()` only, and each child process MUST host exactly one module instance derived from `ModuleBase`. Before startup, the master MUST obtain `RegisteredNames()` from `ModuleFactoryRegistry` and validate that every launch plan module name resolves to exactly one registered factory.

#### Scenario: One module per child process
- **WHEN** the launch plan contains module `A` and module `B`
- **THEN** the master spawns two child processes and each child initializes exactly one corresponding module instance

#### Scenario: Unknown module registration blocks startup
- **WHEN** launch plan module name `X` has no registered factory
- **THEN** the master rejects startup with an explicit module registration error

### Requirement: Startup orchestration MUST follow priority batches with defined randomization
The master SHALL group modules by `startup_priority` and start lower numeric priority batches first. For first-start within the same priority, order MUST be randomized. During restart handling within the same priority, order MUST be randomized only when the pending restart set size is greater than one. Startup progression across priorities MUST be guarded by READY/FAILED barrier outcomes from `ReadyPipeProtocol`.

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

### Requirement: Master process SHALL continuously supervise children and stay alive under policy-controlled failures
After initial startup, the master MUST enter a `waitpid`-driven monitor loop for all child module processes. On abnormal exit, the master MUST evaluate `RestartPolicy` and either schedule delayed restart or apply fuse suppression while keeping the master process alive. During shutdown, the master MUST reap children and finalize control-plane cleanup without leaving zombie processes.

#### Scenario: Abnormal exit triggers policy restart decision
- **WHEN** child module process exits unexpectedly
- **THEN** the master evaluates restart policy and performs delayed restart or suppression according to policy result

#### Scenario: Fuse-open module does not terminate master process
- **WHEN** a module hits fuse-open suppression state
- **THEN** the master remains running and continues supervising other modules

#### Scenario: Graceful shutdown reaps all child processes
- **WHEN** master receives termination signal
- **THEN** it terminates/reaps all children and exits without zombie processes
