## ADDED Requirements

### Requirement: Module factory registry SHALL provide static registration, creation, and name enumeration
The system SHALL provide a `ModuleFactoryRegistry` that maps module names to constructors for `ModuleBase` subclasses. Registration MUST be connected at compile/link time through registration macros and static initialization, so that all linked modules are queryable before entering `main`. The registry MUST expose read-only `RegisteredNames()` for launch-plan consistency checks and MUST allow creation by module name.

#### Scenario: Create registered module instance by name
- **WHEN** module class `MouldDefectDetector` is linked and registered through `REGISTER_MOULD_MODULE` or `REGISTER_MOULD_MODULE_AS`
- **THEN** `ModuleFactoryRegistry::Create("MouldDefectDetector", ...)` (or alias name) returns a valid `ModuleBase` instance

#### Scenario: Unknown module creation fails with explicit error
- **WHEN** caller requests `Create("UnknownModule", ...)` and no factory exists
- **THEN** registry returns a clear not-registered error and does not create any instance

### Requirement: Registration constraints SHALL reject invalid or duplicate module declarations
The registration mechanism MUST enforce that registered types inherit from `ModuleBase` and are constructible by the expected module constructor signature via compile-time assertions. Duplicate module-name registration MUST be rejected and MUST NOT silently overwrite existing mappings.

#### Scenario: Compile-time rejection for non-ModuleBase type
- **WHEN** a class that does not inherit `ModuleBase` is passed to `REGISTER_MOULD_MODULE`
- **THEN** compilation fails with a static assertion indicating base-class constraint violation

#### Scenario: Duplicate registration is rejected
- **WHEN** two registrations target the same module name
- **THEN** registry reports duplicate-name registration failure and preserves deterministic mapping behavior without overwrite
