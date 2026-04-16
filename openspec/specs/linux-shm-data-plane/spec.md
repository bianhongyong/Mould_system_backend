# Linux SHM data plane

## Purpose

POSIX shared memory (`shm_open` / `ftruncate` / `mmap`) as the single-node IPC data plane for channel segments.

## Requirements

### Requirement: POSIX shared memory SHALL be the default IPC data plane
The middleware SHALL create channel data planes with `shm_open`, size them with `ftruncate`, and map them with `mmap` for all single-node IPC channels.

#### Scenario: Channel bootstrap maps shared segment
- **WHEN** a publisher creates or opens a channel for the first time
- **THEN** the middleware initializes or attaches to a POSIX shared memory object and exposes a valid mapped address for runtime access

### Requirement: Shared segment naming and lifecycle MUST be deterministic
The middleware MUST use deterministic channel-derived shared memory names and SHALL close/unmap descriptors safely on shutdown.

#### Scenario: Process restart reattaches existing segment
- **WHEN** a consumer process restarts while the channel segment already exists
- **THEN** the consumer reattaches to the same shared memory object without requiring channel recreation
