# Ready pipe protocol

## Purpose

Define child-to-parent readiness signaling and lifecycle state transitions during module startup.

## Requirements

### Requirement: Child processes SHALL report READY through parent-managed pipe channels
Each module child process MUST publish a READY signal to the parent through a dedicated pipe endpoint after module initialization completes. Parent MUST treat successful `fork` as non-ready transitional state.

#### Scenario: Fork succeeds but READY not yet received
- **WHEN** child process is created successfully
- **THEN** parent records state as `FORKED` or `INITING` and does not mark startup success

#### Scenario: READY signal marks startup success
- **WHEN** child sends a valid READY message through the pipe
- **THEN** parent transitions module state to `READY` and allows orchestration decisions that require readiness

### Requirement: Parent MUST maintain lifecycle states for each module instance
Parent supervision logic MUST track at least `FORKED`, `INITING`, `READY`, `RUNNING`, and `FAILED` states and persist the reason for each failure transition.

#### Scenario: READY timeout transitions to FAILED
- **WHEN** `ready_timeout_ms` elapses without a valid READY message
- **THEN** parent transitions the module to `FAILED` with timeout reason

#### Scenario: Runtime activity advances READY to RUNNING
- **WHEN** module enters normal service loop after READY acknowledgement
- **THEN** parent marks the module as `RUNNING`
