# SHM bus control/runtime split

## Purpose

Define control-plane and data-plane split responsibilities for SHM bus runtime, including fork/COW reuse and lifecycle ownership.

## Requirements

### Requirement: SHM bus implementation MUST separate control-plane and data-plane responsibilities

The system MUST split shared-memory bus into parent-process control-plane components and business-process data-plane components. Control plane MUST own channel segment creation, initialization, lifecycle governance, and state management; data plane MUST own attach, publish/subscribe, and callback execution.

#### Scenario: Parent process initializes channel segments and business processes only attach and use

- **WHEN** parent process finishes channel topology initialization and starts business processes
- **THEN** business processes only perform attach/use/detach and do not recreate global lifecycle state

### Requirement: Data-plane component MUST be singleton in parent process and support COW reuse

The system MUST keep a unique data-plane component instance in parent process and pre-initialize read-only base state before fork. After fork, child processes MUST reuse that read-only state and perform local post-attach initialization, and MUST NOT start threaded data-plane execution resources before fork.

#### Scenario: Child process reuses parent data-plane read-only state

- **WHEN** parent process completes data-plane singleton pre-initialization and forks business child processes
- **THEN** child process reuses parent read-only data-plane state and only creates process-local resources (for example, eventfd/reactor) inside child process

### Requirement: Channel naming MUST be stable and `shm_unlink` MUST only run in parent final-exit path

The system MUST generate channel shared-memory names by "module-name + channel-name". Once `launch_plan` is determined, this naming map MUST remain stable.

During **runtime** (while parent process has not entered final-exit stage), data-plane components MUST NOT execute `shm_unlink`, and parent control plane MUST NOT execute `shm_unlink`.

When parent process enters **final-exit** stage, it MUST execute `shm_unlink` for managed channel set after child processes are fully reclaimed or stopped attaching, and MUST guarantee idempotence.

#### Scenario: Business process exits normally

- **WHEN** business process exits and releases local resources
- **THEN** the process only closes/unmaps local handles and does not execute `shm_unlink`

#### Scenario: Parent process during runtime

- **WHEN** parent process is running normally and supervising child processes
- **THEN** parent process does not execute `shm_unlink`

#### Scenario: Parent process final exit

- **WHEN** parent process enters final-exit stage and child processes are already handled
- **THEN** parent process executes `shm_unlink` for managed channels, and repeated invocation does not produce extra side effects

### Requirement: Runtime model MUST be fixed to fork-only and one-module-per-process

Runtime model MUST satisfy one process runs exactly one module instance (1 process = 1 module), and business-process startup path MUST be fork-only and MUST NOT use exec.

#### Scenario: Process model check

- **WHEN** launcher starts business module instances
- **THEN** each child process hosts exactly one module instance and does not enter exec path