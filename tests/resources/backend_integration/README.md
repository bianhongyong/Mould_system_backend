# Backend integration test resources

- launch plan: `launch_plan.json`
- expected startup priority order: `FrameSourceModule -> FeatureExtractModule -> ResultSinkModule`
- expected interaction: parent process can parse all per-module I/O files and build a full chained topology
- complex launch plan: `launch_plan_complex.json`
- complex module I/O configs:
  - `sensor_fusion_io.json`
  - `risk_eval_io.json`
  - `path_plan_io.json`
  - `actuator_coord_io.json`
- compete 模式四进程压测（1 生产者 + 3 消费者，`CompeteStress`，`delivery_mode=compete`）：
  - `launch_plan_compete_stress.json`（`shm_slot_count` 已略增大，便于极短间隔压测）
  - `compete_stress_producer_io.json` / `compete_stress_consumer_io.json`
  - 发布条数约 **60 秒** 量级：`kStressRunDurationSeconds` 与 **`kPublishIntervalMicros`** 推导（默认 15µs → **4,000,000** 条）。`kPublishIntervalMicros==0` 时为固定 **400 万** 条 yield 模式近似负载。
  - 发布间隔由 **`kPublishIntervalMicros`** 控制（默认 **15µs**；设为 **0** 则仅 `yield`、竞争最激烈）。间隔过小易出现 `Publish failed`/背压日志，属环容量与消费速度限制，需与 compete 语义缺陷区分。
  - 运行后端后于**启动目录**生成 `compete_stress_out/`，生产者写完后执行 `validate_compete_stress.sh` 校验全局无丢包、无重复
