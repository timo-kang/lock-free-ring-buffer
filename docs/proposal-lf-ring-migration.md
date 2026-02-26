# lf_ring IPC 마이그레이션 제안서

## 1. 배경: 현재 Raisin SharedMemory의 문제점

### 1.1 현재 아키텍처

Raisin의 로컬 IPC는 `SharedMemoryPublisher<T>` / `SharedMemorySubscriber<T>`를 통해 구현되어 있다.
각 토픽은 `/dev/shm` 경로에 6개의 공유 메모리 세그먼트를 생성한다:

```
/dev/shm/pub___<TOPIC>___control
/dev/shm/pub___<TOPIC>___header
/dev/shm/pub___<TOPIC>___desc
/dev/shm/pub___<TOPIC>___stats
/dev/shm/pub___<TOPIC>___msg___0 ~ 4    (5-slot ring buffer)
```

동기화는 `pthread_mutex_t` (PTHREAD_MUTEX_ROBUST) + futex wake/wait 조합으로 이루어지며,
메시지는 `setBuffer()` / `getBuffer()`를 통해 `std::vector<unsigned char>`로 직렬화된다.

### 1.2 구조적 위험 요소

#### 1.2.1 Mutex 기반 동기화의 한계

| 문제 | 설명 |
|------|------|
| **Kernel 전환 오버헤드** | `pthread_mutex_lock/unlock`은 contention 발생 시 futex syscall을 수반한다. 커널 진입/복귀에 200~400ns가 소요되며, 실제 데이터 전송(memcpy) 비용보다 동기화 비용이 지배적이다. |
| **Priority Inversion** | Publisher와 Subscriber가 동일 mutex를 공유하므로, 낮은 우선순위 스레드가 mutex를 보유한 상태에서 높은 우선순위 제어 루프가 블로킹될 수 있다. RT 스케줄러 환경에서 치명적이다. |
| **Starvation** | Linux의 기본 pthread mutex는 공정성(fairness)을 보장하지 않는다. Tight loop에서 publisher가 연속 publish하면 subscriber가 mutex를 획득하지 못해 기아(starvation) 상태에 빠진다. |
| **Writer Crash 시 Deadlock** | `PTHREAD_MUTEX_ROBUST` 설정으로 완화하고 있으나, writer 프로세스가 mutex 보유 중 비정상 종료하면 다음 `lock()` 호출에서 `EOWNERDEAD`를 반환한다. 이 복구 경로가 정상적으로 처리되지 않으면 해당 토픽의 모든 subscriber가 영구 차단된다. |

#### 1.2.2 직렬화 오버헤드

현재 모든 메시지는 publish/subscribe 시 `setBuffer()` / `getBuffer()`를 거치며:

- 각 필드별 `memcpy`를 반복 수행
- 중간 버퍼로 `std::vector<unsigned char>` 할당 (heap allocation 발생)
- 고정 크기 메시지(예: Command 72B)도 동일한 직렬화 경로를 통과

100Hz 제어 루프에서 매 사이클마다 불필요한 할당/복사가 반복된다.

#### 1.2.3 Writer 생존 감지의 한계

현재 writer 생존 여부는 `kill(pid, 0)` 시스템 콜로 확인한다:

- PID 재사용(recycling) 시 False positive 발생 가능
- 프로세스 단위 확인이므로 스레드 단위 writer 장애를 감지할 수 없음
- 주기적 polling이 필요하며, 감지 지연이 존재

#### 1.2.4 리소스 낭비

토픽당 6개의 shared memory 파일(control, header, desc, stats, msg*5)이 생성되어:

- 파일 디스크립터 소모가 크고
- 토픽 수가 증가하면 `/dev/shm` 관리가 복잡해지며
- 프로세스 종료 시 잔여 파일 정리(cleanup)가 누락될 수 있음

---

## 2. 구현 내용: lf_ring Lock-Free IPC 프리미티브

### 2.1 핵심 프리미티브

| 프리미티브 | 용도 | Producer | Consumer | 특징 |
|-----------|------|----------|----------|------|
| `SharedLatest<T>` | 최신값 전달 (overwrite) | 1 (SWMR) | N | Seqlock 기반, writer-death 감지 |
| `SPSCQueue<T, N>` | 고정 크기 FIFO | 1 | 1 | Monotonic index + bitmask, zero per-message overhead |
| `SharedRingBufferSPSC` | 가변 크기 FIFO | 1 | 1 | Variable-size payload (audio 등) |

모든 프리미티브는:
- **Lock-free**: mutex/futex 없이 atomic operation만 사용
- **단일 파일**: 토픽당 1개의 shared memory 파일 (header + control + ring 통합)
- **Zero-copy**: `T`가 trivially copyable이면 `memcpy` 한 번으로 전달 (직렬화 없음)

### 2.2 SharedLatest 동작 원리

```
Writer                          Reader
──────                          ──────
seq.store(odd)   ← 쓰기 시작    seq.load() → odd이면 재시도
memcpy(ring, &value)            memcpy(&out, ring)
seq.store(even)  ← 쓰기 완료    seq.load() → 변경 없으면 성공
heartbeat.store(now_ns)         heartbeat 확인 → writer 생존 판단
```

- **Seqlock 패턴**: sequence가 홀수이면 writer가 쓰는 중이므로 reader는 재시도
- **Torn read 방지**: 읽기 전후 sequence가 동일하면 일관된 스냅샷 보장
- **Writer-death 감지**: sequence가 홀수인 채 heartbeat가 오래되면 `ReadResult::kWriterDead` 반환

### 2.3 Adapter Layer

Raisin의 `Publisher<T>` / `Subscriber<T>` 인터페이스와 호환되는 어댑터를 구현했다:

```cpp
// Publisher 측 (예: JoyInterface에서 Command 발행)
lfring::SharedLatestPublisher<Command> pub("command");
pub.publish(cmd);  // → SharedLatest::write() 호출

// Subscriber 측 (예: Controller에서 Command 수신)
lfring::SharedLatestSubscriber<Command> sub("command",
    [](const Command& cmd) { /* 콜백 처리 */ },
    [](ReadResult r) { /* writer 사망 시 처리 */ });
sub.start();  // polling thread 시작
```

어댑터가 제공하는 기능:
- `publish()` / `subscribe()` — raisin과 동일한 사용 패턴
- Writer-death 콜백 — `kill(pid, 0)` 대체
- Topic→파일 경로 자동 매핑 (`/dev/shm/lf___<topic>`)
- Background polling thread (configurable interval)

### 2.4 고정 크기 메시지 래퍼

가변 크기 필드(std::vector, std::string)를 포함하는 raisin 메시지를 `SharedLatest<T>`에서 사용하기 위해 고정 크기 래퍼를 구현했다:

| 래퍼 | 원본 메시지 | 크기 | 변환 |
|------|-----------|------|------|
| `JoyFixed` | sensor_msgs::Joy (vector) | 144B | axes[16] + buttons[16] 고정 배열 |
| `TfFixed` | TransformStamped (string) | ~192B | frame name을 char[64]로 고정 |
| `JoyRangeFixed` | JoyRange (vector) | ~392B | mins/maxs/deadzones[16] 고정 배열 |

모든 래퍼는 `static_assert(is_trivially_copyable_v<T>)`로 컴파일 타임 검증된다.

---

## 3. 엔지니어링 문제 해결

### 3.1 동기화 비용 제거

| 항목 | Raisin (현재) | lf_ring (제안) |
|------|-------------|---------------|
| 동기화 메커니즘 | pthread_mutex + futex syscall | Lock-free atomics (userspace) |
| Publish 경로 lock 수 | 1 mutex acquire + release | 0 |
| Kernel 전환 | 매 publish/subscribe마다 | 없음 |
| Priority inversion | 가능 | 불가능 (lock 없음) |
| Starvation | 가능 (mutex 비공정성) | 불가능 (reader는 독립적) |

### 3.2 Writer Crash 안전성

| 항목 | Raisin (현재) | lf_ring (제안) |
|------|-------------|---------------|
| Crash 감지 | `kill(pid, 0)` polling | `ReadResult::kWriterDead` (heartbeat 기반) |
| PID 재사용 문제 | 있음 | 없음 (timestamp 기반) |
| Crash 후 복구 | mutex EOWNERDEAD 처리 필요 | 자동 (새 writer가 create하면 초기화) |
| Deadlock 가능성 | Writer crash 시 가능 | 불가능 (lock 없음) |

### 3.3 직렬화 제거

| 항목 | Raisin (현재) | lf_ring (제안) |
|------|-------------|---------------|
| 직렬화 방식 | setBuffer/getBuffer (필드별 복사) | memcpy 1회 (trivially copyable) |
| 힙 할당 | 매 publish마다 vector 할당 | 없음 (shared memory 직접 사용) |
| Command (72B) 비용 | 직렬화 + vector 할당 + memcpy | memcpy 72B (1회) |

### 3.4 리소스 효율성

| 항목 | Raisin (현재) | lf_ring (제안) |
|------|-------------|---------------|
| 토픽당 shm 파일 수 | 6개 | 1개 |
| 파일 디스크립터 소모 | 토픽 × 6 | 토픽 × 1 |
| Command 토픽 메모리 | control + header + desc + stats + msg×5 | header(64B) + control(320B) + ring(72B) ≈ 456B |

---

## 4. 벤치마크 결과

테스트 환경: 16-core CPU @ 5.27 GHz, L1d 48 KiB, L2 1 MiB, L3 96 MiB, **SCHED_FIFO priority 80**, 스레드별 CPU 코어 고정

### 4.1 Handoff 레이턴시 비교 (1 writer → 1 reader, 왕복)

#### Spin 기반 (데이터 전송 비용만 측정)

| Transport | JointState (16B) | IMU (80B) | 비고 |
|-----------|----------------:|---------:|------|
| **Raisin SharedMemory** | **719 ns** | **736 ns** | mutex + futex |
| **SharedLatest** | **71 ns** | **57 ns** | seqlock |
| **SPSCQueue** | **76 ns** | **78 ns** | lock-free FIFO |

#### Futex 기반 (실제 프로덕션 경로, SCHED_FIFO)

| Transport | JointState (16B) | 비고 |
|-----------|----------------:|------|
| **Raisin SharedMemory** | **1.45 μs** | mutex + serialize + futex_wake + kernel wake |
| **SharedLatest** | **1.10 μs** | seqlock + memcpy + futex_wake + kernel wake |

**Spin 경로: SharedLatest는 Raisin 대비 10.1~12.8배 빠르다.**
**Futex 경로: SharedLatest는 Raisin 대비 1.3배 빠르다** (커널 futex round-trip ~1μs가 지배적).

Raisin의 레이턴시가 페이로드 크기(16B vs 80B)에 무관하게 ~719-736ns로 일정한 것은 mutex syscall 오버헤드가 지배적이기 때문이다. lf_ring은 커널 전환이 없으므로 페이로드 크기에 비례하는 합리적인 성능을 보인다. Futex 경로에서는 커널 context switch 비용(~500ns)이 양쪽 모두 동일하므로, 전송 속도 차이가 압축되어 1.3배로 나타난다.

### 4.2 Adapter 오버헤드 (추상화 비용)

| 측정 항목 | Raw SharedLatest | Adapter | 차이 |
|-----------|----------------:|--------:|-----:|
| Command Write (72B) | 20.4 ns | 20.4 ns | < 0.1 ns |
| Command Read (72B) | 0.84 ns | 0.91 ns | +0.07 ns |
| VelCmd Write (56B) | 20.4 ns | 20.4 ns | < 0.1 ns |

**Adapter 추상화 비용은 0.1ns 이하로, 사실상 무시할 수 있다.**
컴파일러가 인라이닝으로 간접 호출을 제거하기 때문이다.

### 4.3 Adapter Handoff 레이턴시 (2 스레드)

| Transport | Command (72B) |
|-----------|-------------:|
| Adapter SharedLatest Handoff | 54 ns |
| Adapter SPSCQueue Handoff | 81 ns |

500Hz 제어 루프(2ms 주기) 기준 (SCHED_FIFO):
- Raisin futex handoff: 1.45μs → 주기의 **0.07%**
- lf_ring futex handoff: 1.10μs → 주기의 **0.06%**
- lf_ring spin handoff: 71ns → 주기의 **0.004%**

### 4.4 Multi-Reader 확장성 (SharedLatest)

| Reader 수 | Reader당 레이턴시 |
|----------:|---------:|
| 1 | 7.79 ns |
| 2 | 6.17 ns |
| 4 | 3.51 ns |
| 8 | 2.77 ns |

Reader는 공유 상태에 쓰지 않으므로(read-only atomic load) 코어 간 cache-line bouncing이 없다.
Controller + Logger + GUI가 동일 command 토픽을 구독해도 성능 저하가 최소화된다.

### 4.5 Free-Running Throughput (SPSCQueue)

| 페이로드 | Throughput | 레이턴시 |
|---------|----------:|---------:|
| JointState (16B) | 232 M/s | 2.15 ns/op |
| IMU (80B) | 131 M/s | 3.81 ns/op |

---

## 5. 마이그레이션 계획

### 5.1 원칙

- **Non-breaking**: Adapter layer를 통해 토픽 단위로 점진적 교체
- **Application 코드 무변경**: publish/subscribe 인터페이스가 동일하므로 비즈니스 로직 수정 불필요
- **Service 패턴 제외**: Request/Response(turn_on_microphone 등)는 현행 유지

### 5.2 메시지 타입 인벤토리

raisin_master 코드베이스에서 확인된 전체 메시지 타입:

#### 고정 크기 (직접 SharedLatest 사용 가능)

| 메시지 타입 | 크기 | 필드 |
|-----------|-----:|------|
| `JointState` | 16B | joint_position, joint_velocity |
| `JointTarget` | 40B | target_position, target_velocity, p_gain, d_gain, feedforward_torque |
| `VelocityCommand` | 56B | x_vel, y_vel, yaw_rate, pitch_angle, body_height, pan_dir, tilt_dir |
| `Command` | 72B | x_pos, y_pos, x_vel, y_vel, yaw_rate, pitch_angle, body_height, pan_dir, tilt_dir |
| `Imu` | 80B | quaternion_wxyz, angular_velocity_xyz, linear_acceleration_xyz |
| `QuadrupedState` | 152B | position[3], quaternion[4], joint_position[12] |
| `Timestamp` | 8B | sec, nanosec |

#### 가변 크기 (고정 크기 래퍼 필요)

| 원본 메시지 | 가변 요소 | 래퍼 | 래퍼 크기 | 최대 용량 |
|-----------|----------|------|--------:|----------|
| `JointStates` | vector\<JointState\> | `JointStatesFixed` | ~528B | 최대 32 관절 |
| `ActuatorState` | string name | `ActuatorStateFixed` | ~72B | 이름 최대 32자 |
| `ActuatorStates` | vector\<ActuatorState\> | `ActuatorStatesFixed` | ~2.3KB | 최대 32 액추에이터 |
| `RobotState` | vectors + nested | `RobotStateFixed` | ~2.5KB | 전체 로봇 상태 통합 |
| `BatteryState` | vectors + strings | `BatteryStateFixed` | ~172B | 최대 16 셀 |
| `Joy` | vector\<float/int\> | `JoyFixed` | ~144B | 최대 16 축/버튼 |
| `TransformStamped` | string frame_id | `TfFixed` | ~192B | 프레임명 최대 64자 |
| `JoyRange` | vector\<double\> | `JoyRangeFixed` | ~392B | 최대 16 축 |

### 5.3 토픽별 마이그레이션 매핑

#### Tier 1: 고빈도 센서 및 제어 루프 (SharedLatest)

로봇의 핵심 제어/센서 경로. 최신값만 의미가 있고, 이전 값은 필요 없는 토픽.
이 토픽들은 레이턴시에 가장 민감하며 마이그레이션 효과가 가장 크다.

| 토픽 | 메시지 타입 | 크기 | 빈도 | Publisher → Subscriber | lf_ring 구조체 |
|------|-----------|-----:|------|----------------------|----------------|
| `command` | Command | 72B | ~100Hz | JoyInterface → Controller | `SharedLatest<Command>` |
| `imu` | Imu | 80B | ~200Hz+ | IMU 드라이버 → Controller | `SharedLatest<Imu>` |
| `joint_states` | JointStates | ~528B | ~100Hz+ | 액추에이터 드라이버 → Controller | `SharedLatest<JointStatesFixed>` |
| `joint_targets` | JointTargets | ~1.3KB | ~100Hz | Controller → 액추에이터 드라이버 | `SharedLatest<JointTargetsFixed>` |
| `quadruped_state` | QuadrupedState | 152B | ~100Hz | StateEstimator → Controller | `SharedLatest<QuadrupedState>` |
| `robot_state` | RobotState | ~2.5KB | ~50Hz | Controller → GUI/Logger | `SharedLatest<RobotStateFixed>` |
| `actuator_states` | ActuatorStates | ~2.3KB | ~100Hz | 액추에이터 드라이버 → Monitor | `SharedLatest<ActuatorStatesFixed>` |

#### Tier 2: 원격 제어 및 입력 (SharedLatest)

| 토픽 | 메시지 타입 | 크기 | 빈도 | Publisher → Subscriber | lf_ring 구조체 |
|------|-----------|-----:|------|----------------------|----------------|
| `{source}/vel_cmd` | VelocityCommand | 56B | variable | Remote → JoyInterface | `SharedLatest<VelocityCommand>` |
| `{source}/joy` | Joy | ~144B | ~20Hz | Joy → JoyInterface | `SharedLatest<JoyFixed>` |
| `{id}/tf` | TransformStamped | ~192B | variable | Transform → Transform | `SharedLatest<TfFixed>` |
| `joy_range` | JoyRange | ~392B | 2Hz | JoyInterface → GUI | `SharedLatest<JoyRangeFixed>` |

#### Tier 3: 상태 모니터링 (SharedLatest)

| 토픽 | 메시지 타입 | 크기 | 빈도 | lf_ring 구조체 |
|------|-----------|-----:|------|----------------|
| `battery_state` | BatteryState | ~172B | ~1Hz | `SharedLatest<BatteryStateFixed>` |
| `{spec}_microphone_status` | Bool | 1B | 1Hz | `SharedLatest<bool>` |
| `{spec}_microphone_volume` | Float64 | 8B | 1Hz | `SharedLatest<double>` |
| `{spec}_speaker_volume` | Float64 | 8B | 1Hz | `SharedLatest<double>` |
| `joy_sig` | Int16 | 2B | event | `SharedLatest<int16_t>` |
| `password_status` | PasswordStatus | ~24B | event | `SharedLatest<PasswordStatus>` |
| `heartbeat` | uint64 | 8B | periodic | `SharedLatest<uint64_t>` + `is_writer_alive()` |

#### Tier 4: 스트리밍 데이터 (SPSC FIFO)

모든 메시지가 소비되어야 하는 토픽.

| 토픽 | 메시지 타입 | 빈도 | lf_ring 구조체 |
|------|-----------|------|----------------|
| `{spec}_audio_data` | AudioData | 20Hz | `SharedRingBufferSPSC` (가변 크기 audio chunk) |
| `gui_message` | Log | 10Hz | `SharedRingBufferSPSC` (가변 크기 로그) |

#### Tier 5: 마이그레이션 대상 외

| 토픽 | 사유 |
|------|------|
| `turn_on/off_*_microphone` | Service (Request/Response 패턴) |
| `set_*_volume`, `set_joy_range` | Service |
| `load_process`, `password` | Service |
| `process_list` | 가변 크기 + 저빈도, 현행 유지 |
| Remote-only 토픽 | TCP/WebSocket 기반, 로컬 IPC 아님 |

### 5.4 단계별 실행 계획

```
Phase 1 ✅ 완료: Adapter Layer + Fixed-Size Wrappers + Tests + Benchmarks
   └── lf_ring 라이브러리 내 구현 완료
       ├── raisin_adapter.hpp (Publisher/Subscriber 어댑터)
       ├── raisin_messages.hpp (전체 메시지 타입 + 래퍼)
       ├── test_raisin_adapter.cpp (18개 통합 테스트)
       └── benchmark_raisin_comparison.cpp (성능 벤치마크)

Phase 1.5 ✅ 완료: Futex Notification + SCHED_FIFO Benchmarks
   └── SharedLatest에 futex 기반 알림 기능 추가
       ├── futex.hpp (futex_wake/wait 헬퍼)
       ├── write_and_notify() / wait_for_update() / notify_counter() API
       ├── test_shared_latest.cpp (6개 futex 테스트 추가, 총 73개 테스트 통과)
       ├── benchmark_futex.cpp (SCHED_FIFO + CPU 코어 고정)
       ├── benchmark_raisin_shm.cpp (Raisin vs lf_ring futex 비교)
       ├── stress_test.cpp (crash recovery + contention + 센서 루프 시뮬레이션)
       └── 결과: futex handoff 1.10μs (lf_ring) vs 1.45μs (Raisin), SCHED_FIFO 하
               crash recovery 510ns vs 20.9μs, 센서 루프 e2e 2.5-7.1x 개선
               (raisin 2kHz 422회/1kHz 78회 deadline 초과, lf_ring 전 구간 0회)

Phase 2: 핵심 제어 루프 마이그레이션 (최우선)
   ├── command: JoyInterface → Controller
   ├── imu: IMU 드라이버 → Controller
   ├── joint_states: 액추에이터 드라이버 → Controller
   ├── joint_targets: Controller → 액추에이터 드라이버
   ├── quadruped_state: StateEstimator → Controller
   └── 검증: 기존 raisin 테스트 스위트 통과 확인

Phase 3: 로봇 상태 + 원격 제어 마이그레이션
   ├── robot_state: Controller → GUI/Logger
   ├── actuator_states: 액추에이터 드라이버 → Monitor
   ├── battery_state: BMS → Monitor/GUI
   ├── vel_cmd: Remote → JoyInterface
   └── heartbeat: kill(pid,0) → is_writer_alive() 전환

Phase 4: 입력 + 래퍼 필요 토픽 마이그레이션
   ├── joy: JoyFixed 래퍼 적용
   ├── tf: TfFixed 래퍼 적용
   ├── joy_range: JoyRangeFixed 래퍼 적용
   └── 스칼라 토픽: mic status/volume, speaker volume, joy_sig

Phase 5: 스트리밍 토픽 마이그레이션
   ├── audio_data: SharedRingBufferSPSC
   └── gui_message: SharedRingBufferSPSC

Phase 6: 정리
   └── 마이그레이션 완료된 토픽의 기존 SharedMemory 코드 제거
```

### 5.4 예상 효과 요약

| 지표 | 현재 (Raisin SHM) | 마이그레이션 후 (lf_ring) |
|------|-------------------|------------------------|
| Handoff 레이턴시 (spin) | ~719 ns | ~71 ns (**10.1x 개선**) |
| Handoff 레이턴시 (futex, SCHED_FIFO) | ~1.45 μs | ~1.10 μs (**1.3x 개선**) |
| Futex round-trip (SCHED_FIFO) | — | **1.10 μs** (2ms 예산의 0.06%) |
| 동기화 방식 | pthread_mutex + futex | Lock-free atomics |
| Writer crash 처리 | 20.9μs EOWNERDEAD (블로킹) | **510ns** (비블로킹, heartbeat) |
| Writer crash 시 데이터 | ⚠ torn data 가능 | 안전 (seq odd → false 리턴) |
| 토픽당 shm 파일 수 | 6개 | 1개 |
| 직렬화 | setBuffer/getBuffer (heap alloc) | memcpy 1회 (zero-alloc) |
| Priority inversion | 가능 (PRIO_INHERIT 미설정) | 불가능 |
| Priority inversion worst-case | **49ms** (7 readers 시) | 없음 |
| Deadlock | Writer crash 시 가능 | 불가능 |
| Multi-reader (7 readers, avg) | 2,584 ns/read (mutex 직렬화) | 327 ns/read (**8x 개선**) |
| Multi-reader (8 readers, spin) | Mutex 직렬화 | 2.77 ns/read (lock-free 동시 접근) |
| 센서 루프 e2e avg (1kHz, controller) | 51.3 μs (mutex+futex) | 20.2 μs (futex) (**2.5x 개선**) |
| 센서 루프 e2e max (1kHz, controller) | 2,483.1 μs | 970.9 μs (**2.6x 개선**) |
| 센서 루프 e2e avg (2kHz, controller) | 73.0 μs (mutex+futex) | 10.3 μs (futex) (**7.1x 개선**) |
| 센서 루프 e2e max (2kHz, controller) | 2,480.3 μs | 498.5 μs (**5.0x 개선**) |
| 센서 루프 missed deadlines (2kHz) | 422 | 0 |
| 센서 루프 missed deadlines (1kHz) | 78 | 0 |
| 센서 루프 reader 대기 시 CPU | 0% (futex_wait) | 0% (futex_wait) |
