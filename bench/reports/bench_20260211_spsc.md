# Benchmark Report (2026-02-11)

Focus: `SharedRingBufferSPSC` throughput vs existing MPSC baseline and `SharedLatest` (2-thread RW).

| Benchmark | Threads | real_time (ns/op) | items/s | items/s/producer |
|---|---:|---:|---:|---:|
| `BM_Ring_SPSC_Small/real_time/threads:2` | 2 | 13.453 | 37,167,748.279 |  |
| `BM_Ring_MPSC_Small/real_time/threads:2` | 2 | 25.658 | 19,487,301.129 | 19,487,301.129 |
| `BM_Locked_MPSC_Small/real_time/threads:2` | 2 | 42.036 | 11,894,651.952 | 11,894,651.952 |
| `BM_Latest_RobotState_RW/real_time/threads:2` | 2 | 4.502 | 103,835,194.302 |  |
| `BM_Typed_MPSC_RobotState/real_time/threads:2` | 2 | 24.848 | 20,122,557.784 | 20,122,557.784 |
