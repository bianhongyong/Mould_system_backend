# Payload slot metadata routing

## Purpose

Keep payload descriptors aligned with slot/header metadata and support image routing fields.

## Requirements

### Requirement: Payload descriptor and slot metadata MUST remain consistent
For each committed message, payload descriptor fields (`payload_type`, `size`, `checksum`, `schema_version`, `ttl`, `ref_count`, `storage_flags`) MUST match corresponding slot/header fields.

#### Scenario: Decode rejects descriptor-slot mismatch
- **WHEN** consumer validates a committed slot with inconsistent descriptor fields
- **THEN** decode is rejected and mismatch is reported through error metrics/logs

### Requirement: Image payload metadata SHALL be routable from slot/header
Image payloads SHALL store routing metadata (`image_name`, `lamination_detect_flag`, `width`, `height`, `channels`, `format`, `timestamp`) in slot/header metadata so consumers can route without decoding image bytes first.

#### Scenario: Consumer routes by lamination flag without pixel decode
- **WHEN** consumer reads image slot metadata for a message
- **THEN** it can branch processing by `lamination_detect_flag` before performing full image decode
