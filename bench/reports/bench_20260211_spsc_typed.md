# Benchmark Report (2026-02-11, Typed SPSC)

Focus: 2-thread comparison for robotics-like payloads (`RobotState`) and small raw payload reference.

| Benchmark | Threads | real_time (ns/op) | items/s | items/s/producer |
|---|---:|---:|---:|---:|
| `BM_Typed_SPSC_RobotState/real_time/threads:2` | 2 | 20.219 | 24,729,256.679 |  |
| `BM_Typed_MPSC_RobotState/real_time/threads:2` | 2 | 25.933 | 19,280,633.923 | 19,280,633.923 |
| `BM_Ring_SPSC_Small/real_time/threads:2` | 2 | 13.129 | 38,084,651.436 |  |
| `BM_Ring_MPSC_Small/real_time/threads:2` | 2 | 20.490 | 24,402,397.408 | 24,402,397.408 |
| `BM_Latest_RobotState_RW/real_time/threads:2` | 2 | 5.270 | 87,096,875.016 |  |
