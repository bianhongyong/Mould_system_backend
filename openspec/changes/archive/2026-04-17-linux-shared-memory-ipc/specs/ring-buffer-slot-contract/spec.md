## ADDED Requirements

### Requirement: Ring buffer layout SHALL include fixed metadata regions
Each channel shared segment SHALL contain `RingHeader`, `ConsumerTable`, `SlotMeta[N]`, and `PayloadRegion` in a fixed layout with versioned header metadata.

#### Scenario: Reader validates expected layout version
- **WHEN** a process maps an existing channel segment
- **THEN** it verifies header magic/version and rejects attachment if layout is incompatible

### Requirement: Slot visibility SHALL follow two-phase commit
Producers SHALL reserve slot state, write payload and metadata, and only then atomically mark the slot as committed for consumers.

#### Scenario: Consumer never reads half-written payload
- **WHEN** a consumer polls a slot while producer write is in progress
- **THEN** the consumer observes non-committed state and skips payload read until commit is published
