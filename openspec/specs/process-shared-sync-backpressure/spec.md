# Process-shared sync and backpressure

## Purpose

Cross-process wakeups via `eventfd` and `epoll`, notification fd mapping in ring layout, and observability-first backpressure in the initial phase.

## Requirements

### Requirement: Cross-process notification SHALL use `eventfd + epoll`
The middleware SHALL use `eventfd + epoll` as the cross-process notification mechanism so consumers can block on subscribed channels without polling.

#### Scenario: Producer notifies all online consumers after publish
- **WHEN** producer commits a message for a channel
- **THEN** producer writes notification events to each online consumer's mapped `eventfd` for that channel

### Requirement: Notification fd mapping SHALL be published in ring layout under daemon-fork model
Under the daemon-orchestrated `fork` process model, ring layout SHALL expose `(channel, consumer) -> eventfd` notification mapping that inherited child processes can use directly.

#### Scenario: Consumer binds inherited fds from shared mapping
- **WHEN** consumer process starts from daemon `fork` and attaches ring layout
- **THEN** consumer reads mapped notification fds and registers them into local `epoll` set for subscribed channels

### Requirement: Backpressure handling in module 6 MUST be observability-first
Module 6 SHALL treat producer blocking/backpressure as observability-only scope: emit structured waiting logs without introducing timeout/failure-tier policy changes in this phase.

#### Scenario: Slow consumer causes producer waiting
- **WHEN** producer cannot make forward progress due to slow consumers
- **THEN** runtime records waiting start/end, duration, and occupancy/lag snapshots, while preserving current blocking behavior
