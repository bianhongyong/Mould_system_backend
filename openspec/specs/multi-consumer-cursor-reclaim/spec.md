# Multi-consumer cursor reclaim

## Purpose

Per-consumer read cursors, minimum-sequence reclaim, and predictable behavior on add/remove.

## Requirements

### Requirement: Output channels SHALL maintain per-consumer read cursors
The middleware SHALL maintain independent read sequences per online consumer so one lagging consumer does not stall faster consumers.

#### Scenario: Fast consumer continues despite lagging peer
- **WHEN** two consumers subscribe and one consumer slows down
- **THEN** the fast consumer continues consuming newly committed slots using its own cursor

### Requirement: Reclaim MUST be gated by minimum readable sequence
A committed slot MUST NOT be reclaimed until all online consumer cursors have advanced past that slot and retention constraints are satisfied.

#### Scenario: Reclaim advances after slow consumer catches up
- **WHEN** the minimum consumer cursor moves beyond an old slot
- **THEN** the middleware marks that slot reclaimable and returns its space to producer capacity

### Requirement: New consumer cursor SHALL start at latest slot
When a new consumer is added, its initial cursor MUST point to the current latest ring slot so it only consumes messages produced after joining.

#### Scenario: Newly added consumer does not replay historical backlog
- **WHEN** a consumer is added while ring already contains committed historical slots
- **THEN** the new consumer cursor starts from latest slot and skips old committed slots

### Requirement: Consumer removal MUST trigger reclaim reevaluation
When a consumer is removed (set to offline), middleware MUST recompute minimum cursor and trigger one reclaim pass to release reclaimable slots promptly.

#### Scenario: Removing slowest consumer unlocks reclaim
- **WHEN** the current minimum cursor belongs to a removed consumer
- **THEN** middleware recalculates minimum cursor using remaining online consumers and reclaims newly eligible slots in the triggered reclaim pass
