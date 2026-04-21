# Restart policy governance

## Purpose

Define governed restart behavior for child module failures without terminating master supervision.

## Requirements

### Requirement: Abnormal child exits MUST trigger governed restart handling
The master SHALL treat all child process exits as abnormal-path events and MUST evaluate restart eligibility using configured policy instead of immediate unconditional relaunch.

#### Scenario: Child exits and policy evaluation is invoked
- **WHEN** a module child process exits with any status
- **THEN** the supervisor runs restart policy evaluation before any relaunch action

### Requirement: Restart policy MUST enforce exponential backoff and retry/window limits
For each module, restart delay MUST grow exponentially from `restart_backoff_ms` and MUST respect `restart_max_retries` within `restart_window_ms` accounting.

#### Scenario: Repeated failures increase delay
- **WHEN** a module fails multiple times within policy window
- **THEN** the next relaunch delay increases exponentially from the configured base backoff

#### Scenario: Retry cap blocks immediate relaunch
- **WHEN** failures exceed `restart_max_retries` within `restart_window_ms`
- **THEN** the module is not relaunched immediately and fuse evaluation is applied

### Requirement: Fuse-open behavior SHALL suppress relaunch without terminating master
When policy enters fuse-open state for a module (based on `restart_fuse_ms` and policy counters), the master MUST record fuse-open status, continue monitoring, and MUST NOT force master process exit.

#### Scenario: Fuse opens for unstable module
- **WHEN** module failures trigger fuse condition
- **THEN** supervisor records fuse-open state and suppresses relaunch attempts for that module

#### Scenario: Master remains alive during module fuse-open
- **WHEN** one or more modules are fuse-open
- **THEN** the master process continues running supervision and observability loops
