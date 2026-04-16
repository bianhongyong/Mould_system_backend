## ADDED Requirements

### Requirement: Module channel definitions SHALL be loaded from `.txt` config files
Each module SHALL provide a `.txt` configuration file that declares its input channels and output channels using a validated syntax.

#### Scenario: Module startup loads channel config
- **WHEN** a module process starts
- **THEN** runtime loads and validates that module's `.txt` channel definition before bus initialization

### Requirement: Runtime MUST aggregate all module channel configs before SHM allocation
The middleware MUST merge channel definitions from all modules into a global channel topology to compute producer/consumer counts and allocation parameters per channel.

#### Scenario: Shared memory sizing uses aggregated consumers
- **WHEN** runtime prepares shared memory for channel `X`
- **THEN** it uses the aggregated topology for `X` (including consumer count and configured parameters) instead of single-module local assumptions

### Requirement: Conflicting channel definitions MUST fail fast
If modules define conflicting channel roles or incompatible channel parameters, runtime MUST reject initialization with explicit validation errors.

#### Scenario: Startup blocked on channel parameter conflict
- **WHEN** two module configs define channel `X` with incompatible capacity/ttl constraints
- **THEN** runtime aborts startup and reports conflict details for correction
