## MODIFIED Requirements

### Requirement: POSIX shared memory SHALL be the default IPC data plane
The middleware SHALL create channel data planes with `shm_open`, size them with `ftruncate`, and map them with `mmap` for all single-node IPC channels. Master process MUST initialize SHM control-plane resources before child launch, and child processes MUST attach/use existing mappings instead of creating control-plane topology.

#### Scenario: Channel bootstrap maps shared segment
- **WHEN** a publisher creates or opens a channel for the first time
- **THEN** the middleware initializes or attaches to a POSIX shared memory object and exposes a valid mapped address for runtime access

#### Scenario: Child process uses pre-initialized control-plane
- **WHEN** a module child starts after master SHM initialization
- **THEN** the child attaches to already created channel control/data structures and does not recreate topology objects

### Requirement: Shared segment naming and lifecycle MUST be deterministic
The middleware MUST use deterministic channel-derived shared memory names and SHALL close/unmap descriptors safely on shutdown. Channel objects MUST remain allocated across individual child failures and restarts; channel unlink MUST occur only during final master shutdown.

#### Scenario: Process restart reattaches existing segment
- **WHEN** a consumer process restarts while the channel segment already exists
- **THEN** the consumer reattaches to the same shared memory object without requiring channel recreation

#### Scenario: Channel persists across child failure
- **WHEN** a module child exits unexpectedly
- **THEN** channel SHM objects remain available for subsequent relaunch attach/reuse

### Requirement: Consumer membership state MUST follow real process liveness by PID
On child exit, master MUST offline consumer slots owned by that child PID in SHM membership/control structures while leaving channel identity intact.

#### Scenario: Child exit clears its consumer slots
- **WHEN** child PID `P` exits and had active consumer registrations
- **THEN** master marks all consumer slots owned by `P` as offline in SHM control metadata

#### Scenario: Other process memberships remain untouched
- **WHEN** child PID `P` exits in a multi-consumer channel
- **THEN** only slots bound to `P` are offlined and slots for other live PIDs remain active
