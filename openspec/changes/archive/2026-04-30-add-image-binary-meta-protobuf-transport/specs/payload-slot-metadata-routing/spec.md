## MODIFIED Requirements

### Requirement: Payload descriptor and slot metadata MUST remain consistent
For each committed message, payload descriptor fields (`payload_type`, `size`, `checksum`, `schema_version`, `ttl`, `ref_count`, `storage_flags`) MUST match corresponding slot/header fields, and consumers MUST reject decode when any mirrored field diverges.

#### Scenario: Decode rejects descriptor-slot mismatch
- **WHEN** consumer validates a committed slot with inconsistent descriptor fields
- **THEN** decode is rejected and mismatch is reported through error metrics/logs

### Requirement: Image payload metadata SHALL be routable from slot/header
Image payloads SHALL store routable metadata in slot/header so consumers can branch handling before image decode. Metadata MUST include at least (`image_name`, `lamination_detect_flag`, `width`, `height`, `channels`, `format`, `timestamp`, `checksum_type`, `checksum_value`) and SHALL remain aligned with image protobuf metadata semantics.

#### Scenario: Consumer routes by lamination flag without pixel decode
- **WHEN** consumer reads image slot metadata for a message
- **THEN** it can branch processing by `lamination_detect_flag` before performing full image decode

#### Scenario: Consumer validates checksum metadata before decode
- **WHEN** consumer receives image metadata with checksum fields in slot/header
- **THEN** it validates checksum metadata and rejects decode on checksum inconsistency
