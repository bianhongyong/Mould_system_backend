# Backend integration test resources

- launch plan: `launch_plan.json`
- expected startup priority order: `FrameSourceModule -> FeatureExtractModule -> ResultSinkModule`
- expected interaction: parent process can parse all per-module I/O files and build a full chained topology
