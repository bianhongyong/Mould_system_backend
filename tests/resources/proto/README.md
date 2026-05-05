# Backend integration proto map

## channel -> proto

- `TelemetryIngress` -> `sensor_fusion/sensor_fusion_telemetry.proto::TelemetryIngress`
- `PerceptionTrack` -> `sensor_fusion/sensor_fusion_telemetry.proto::PerceptionTrack`
- `SensorFusionAuditEvent` -> `sensor_fusion/sensor_fusion_telemetry.proto::SensorFusionAuditEvent`
- `RiskScore` -> `risk_eval/risk_eval_core.proto::RiskScore`
- `DiagnosisAlert` -> `risk_eval/risk_eval_core.proto::DiagnosisAlert`
- `RiskEvalAuditEvent` -> `risk_eval/risk_eval_core.proto::RiskEvalAuditEvent`
- `PlanTrajectory` -> `path_plan/path_plan_core.proto::PlanTrajectory`
- `CommandHint` -> `path_plan/path_plan_core.proto::CommandHint`
- `PathPlanAuditEvent` -> `path_plan/path_plan_core.proto::PathPlanAuditEvent`
- `ActuatorCommand` -> `actuator_coord/actuator_coord_core.proto::ActuatorCommand`
- `SyncHeartbeat` -> `actuator_coord/actuator_coord_core.proto::SyncHeartbeat`
- `ActuatorCoordAuditEvent` -> `actuator_coord/actuator_coord_core.proto::ActuatorCoordAuditEvent`
- `ImageFrame` -> `common/proto/image_frame.proto::ImageFrame`

## unified envelope

- `unified_output_envelope.proto::UnifiedOutputEnvelope`
  - 用 `oneof payload` 聚合所有输出通道消息类型
  - 单条 envelope 同一时刻只能持有一个具体 payload 类型