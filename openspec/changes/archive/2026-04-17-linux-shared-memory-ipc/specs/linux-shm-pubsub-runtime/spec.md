## ADDED Requirements

### Requirement: `ShmPubSubBus` SHALL provide true multi-process IPC semantics
`ShmPubSubBus` SHALL deliver messages across independent OS processes through mapped shared memory channels rather than process-local queues.

#### Scenario: Broker and Infer exchange messages from separate processes
- **WHEN** Broker publishes to a mapped channel and Infer runs in another process
- **THEN** Infer receives the committed payload through shared memory without in-process fallback transport

### Requirement: Channel factory SHALL resolve single-node mode to Linux SHM runtime
`ChannelFactory` SHALL resolve single-node bus selection to the Linux shared-memory-backed `ShmPubSubBus` implementation.

#### Scenario: Default bus selection returns Linux SHM implementation
- **WHEN** runtime configuration requests single-node deployment
- **THEN** factory returns a bus instance backed by POSIX shared memory channel runtime
