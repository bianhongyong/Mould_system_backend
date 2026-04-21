## ADDED Requirements

### Requirement: Launch plan resources MUST conform to a strict schema
Each module resource definition MUST include all required fields: `startup_priority`, `cpu_set`, `restart_backoff_ms`, `restart_max_retries`, `restart_window_ms`, `restart_fuse_ms`, and `ready_timeout_ms`. Missing required fields MUST cause startup rejection.

#### Scenario: Missing required field rejects startup
- **WHEN** module resource omits `restart_window_ms`
- **THEN** the master rejects startup with a schema validation error naming the missing field

### Requirement: cpu_set format MUST be validated and invalid values MUST block master startup
`cpu_set` MUST accept a comma-separated CPU list format such as `"0,2,4"`. Any malformed token, duplicate syntax error, or out-of-range value MUST cause master startup failure.

#### Scenario: Valid cpu_set passes validation
- **WHEN** module resource contains `cpu_set="0,2,4"`
- **THEN** schema validation succeeds for CPU affinity format

#### Scenario: Invalid cpu_set fails validation
- **WHEN** module resource contains `cpu_set="0,a,4"`
- **THEN** the master aborts startup and reports an invalid `cpu_set` error

### Requirement: Legacy cpu_id SHALL be mapped compatibly to cpu_set
If legacy `cpu_id` is provided and `cpu_set` is absent, parser SHALL map `cpu_id=N` to `cpu_set="N"` before final validation. If both are present, `cpu_set` MUST be authoritative.

#### Scenario: cpu_id-only configuration is migrated
- **WHEN** module resource includes `cpu_id=3` and no `cpu_set`
- **THEN** parser emits effective `cpu_set="3"` and continues validation

#### Scenario: cpu_set overrides cpu_id
- **WHEN** module resource includes `cpu_id=3` and `cpu_set="1,3"`
- **THEN** parser validates and uses `cpu_set="1,3"` as the effective affinity set
