# Benchmark Report (2026-02-10)

Focus: `SharedLatest<RobotState>` vs typed MPSC ring buffer for `RobotState`.

Notes:
- `SharedLatest` is SWMR latest-value (not FIFO). Throughput here is reads/writes, not queued messages.
- `BM_Latest_RobotState_RW` reports reader-side successful reads/s while a writer runs concurrently.
- Ring benchmarks are MPSC (1 consumer thread, N-1 producer threads). `items_per_second` is consumed messages/s.

| Benchmark | Threads | real_time (ns/op) | items/s | items/s/producer |
|---|---:|---:|---:|---:|
| `BM_Latest_RobotState_Write/real_time` | 1 | 4.207 | 237,693,413.604 |  |
| `BM_Latest_RobotState_Read/real_time` | 1 | 2.049 | 488,113,348.894 |  |
| `BM_Latest_RobotState_RW/real_time/threads:2` | 2 | 4.841 | 94,427,781.468 |  |
| `BM_Typed_MPSC_RobotState/real_time/threads:2` | 2 | 25.343 | 19,729,155.039 | 19,729,155.039 |
| `BM_Typed_MPSC_RobotState/real_time/threads:4` | 4 | 54.973 | 13,643,045.211 | 4,547,681.737 |
| `BM_Typed_MPSC_RobotState/real_time/threads:8` | 8 | 107.442 | 8,143,918.923 | 1,163,416.989 |
| `BM_Typed_MPSC_RobotState_Burst/128/real_time/threads:2` | 2 | 3,138.882 | 20,389,422.982 | 20,389,422.982 |
| `BM_Typed_MPSC_RobotState_Burst/128/real_time/threads:4` | 4 | 7,089.710 | 13,540,750.688 | 4,513,583.563 |
| `BM_Typed_MPSC_RobotState_Burst/128/real_time/threads:8` | 8 | 13,800.336 | 8,115,744.287 | 1,159,392.041 |
