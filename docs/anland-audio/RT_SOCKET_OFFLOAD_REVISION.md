# Anland 音频：socket 移出 realtime callback（修订 2）

对应提交 **`4d17eff`**
（`backend/anland: take the audio socket out of the realtime callback`）。

本文档是 [RT_SAFETY_REVISION.md](RT_SAFETY_REVISION.md)（描述 `89e6ceb`）的后续修订，
回答四个问题：

- **A** speaker PCM 从 RT process callback 到 Android socket 的完整线程路径
- **B** `audio_fd` 为什么不存在 close/reuse race
- **C** 每一个 RT thread 读写字段的分类（atomic / immutable / RT-private）
- **D** `keepalive_len` 的状态机
- **E** timer 是 one-shot 还是 periodic
- **F** 编译检查结果

---

## 0. 本次修正的 6 个问题

| # | 问题 | 处理 |
|---|---|---|
| 1 | `audio_fd` 被 RT thread 直接 `sendmsg`，loop thread 同时改写/关闭 → data race + fd close/reuse 生命周期风险 | socket 完全移出 RT thread；新增 **sender thread** 独占持有传输 fd，RT 只往无锁队列投递 |
| 2 | 删掉 `audio_fd >= 0` 后，detached 时 real PCM 分支仍可能 `sendmsg(-1, …)` | RT callback **不再持有任何 fd**，无 fd 可判；silence 分支改由 `keepalive_len` 把关 |
| 3 | `anland_audio_start()` / `build_pw()` 无条件 `update_silence_size()`，导致无 consumer 时 `keepalive_len` 也可能非 0 | 恢复明确 gate：只有 `play_format_known`（本 attachment 收到过 PLAYBACK FORMAT）才 arm；attach/detach 归零 |
| 4 | `pcm_seen` / `keepalive_on` 由 RT 读写、loop thread 又 reset → data race | 改名为 `rt_pcm_seen` / `rt_keepalive_on`，**RT-private**；loop 只 bump `_Atomic epoch`，RT 自己发现 epoch 变化后 reset |
| 5 | `arm_reconnect()` 用 `interval == NULL`（one-shot），且 connected 时直接 return 不重新 arm → `drain_rt_events()` 通常只跑一次 | 改为**真正 periodic**：`pw_loop_update_timer(..., &val, &val, false)`，只 arm 一次，永久生效 |
| 6 | `defer_detach()` 的 publication 顺序（先 stop_requested 后 serial）| 该机制**整体删除**（不再需要）；现存 publication 顺序为 `keepalive_len`(release) → `epoch`(release) |

未被扩大范围的部分：mic 仍**不使用** `PW_STREAM_FLAG_RT_PROCESS`；mic ring 仍是 loop-thread-only；
SOCK_SEQPACKET framing 校验逐字保留；speaker keep-alive 行为保留；未触碰其他 Niri 代码。

---

## A. speaker PCM 的完整线程路径

```
┌─ PipeWire realtime data thread ────────────────────────────────────────────┐
│ on_capture_process()                        (唯一进入点，RT)               │
│   1. pw_stream_dequeue_buffer(capture)                                     │
│   2. epoch 变化？→ reset rt_pcm_seen / rt_keepalive_on                     │
│   3. chunk.size > 0 ?                                                      │
│        是 → payload = 图产出的 PCM（原样，不做任何长度改写）                │
│        否 → len = keepalive_len（acquire）；len==0 则本周期什么都不做       │
│             len>0 → payload = a->silence（immutable，零填充）               │
│   4. send_ring_push(&a->send, &hdr, payload, size)                         │
│        · 一个 slot = 一条完整 AUDIO_MSG_PCM（4B len + 8B hdr + payload）    │
│        · 满则整条丢弃并置 AUDIO_EV_QUEUE_FULL，绝不拆分                     │
│   5. sender_wake(a)  → 往非阻塞 pipe 写 1 字节                             │
│   6. pw_stream_queue_buffer(capture)                                       │
│   （全程不触碰 fd / loop / 锁 / stdio / 堆）                               │
└────────────────────────────────────────────────────────────────────────────┘
                                   │  SPSC slot queue (64 KiB, 无锁)
                                   ▼
┌─ anland-audio sender thread ──────────────────────────────────────────────┐
│ sender_thread()                          (独占持有 sock_fd)                │
│   loop:                                                                    │
│     while (sender_send_one() == 0) ;      ← 排空队列                       │
│     == -EAGAIN → poll(sock_fd, POLLOUT, 100ms)，保留 slot 不丢             │
│     == -EPIPE  → stop_requested = true; sender_wake(); break               │
│     队列空      → poll(ctl_read, POLLIN, 100ms) → 读掉 poke → 回到循环     │
│   sender_send_one():                                                       │
│     send_ring_peek() 取 offset/len → 构造 1~2 个 iovec（跨环时拆两段，       │
│     sendmsg 仍只交付一条消息）→ sendmsg(fd, …, MSG_DONTWAIT|MSG_NOSIGNAL)   │
│     成功且 n == len → send_ring_consume(len)                               │
│   exit: close(sock_fd)                   ← fd 由本线程关闭                  │
└────────────────────────────────────────────────────────────────────────────┘
                                   │  AF_UNIX socketpair (message socket)
                                   ▼
                          Anland Android consumer → AAudio → 扬声器

┌─ PipeWire thread loop（anland-audio）──────────────────────────────────────┐
│ on_tick() 每 1s：service_rt_state()                                        │
│    · 把 RT 置的 events 边沿位落成日志（stdio 只能在这里做）                 │
│    · stop_requested（sender 置）→ detach_audio_fd_locked()                 │
│ on_audio_readable()：读 mic PCM / FORMAT，写 mic ring；HUP/ERR → detach     │
│ apply_format()：收到 PLAYBACK FORMAT → play_format_known=true; arm_keepalive│
└────────────────────────────────────────────────────────────────────────────┘
```

延迟特征：队列深度上限 64 KiB（48 kHz/1024 帧立体声约 10 个周期）。稳态下 sender 被
`sender_wake()` 立刻唤醒，**不引入额外常驻延迟**；只有内核发送缓冲满时才会积累，且由环的
容量上限封顶，超限丢弃整条消息（计数并打日志），不会无界增长。

---

## B. 为什么不存在 close/reuse race

关键点：**RT thread 不持有、不读取、不使用任何文件描述符**，因此不存在
「RT 读到 fd → 别人 close → 号码被复用 → RT 对陌生 fd 发送」这条路径。

fd 的所有权与生命周期：

| fd | 谁创建 | 谁使用 | 谁关闭 | 关闭时机 |
|---|---|---|---|---|
| `audio_fd`（本端） | `anland_audio_set_fd()` 中的 `F_DUPFD_CLOEXEC` | loop thread（`on_audio_readable` 读 mic） | `pw_loop_destroy_source()`（`pw_loop_add_io(..., close=true)`） | 仅在 `detach_audio_fd_locked()` 内 |
| `sock_fd`（发给 sender 的复制） | 同上（同一个 dup） | **sender thread 独占**（`sendmsg`） | **sender thread 自己** `close(fd)` | sender 函数返回前 |
| `ctl_read` | `pipe()` | sender thread | loop thread，**在 join 之后** | `stop()` |
| `ctl_write` | `pipe()` | RT callback（1 字节 poke） | loop thread，**在 stop flag + join 之后** | `stop()` |

顺序保证（`detach_audio_fd_locked()`）：

```
1. keepalive_len = 0                (release)   ← RT 不再投递新的 silence
2. epoch++                          (release)   ← RT 必然 reset 私有状态
3. play_format_known = false                   ← 不再 re-arm
4. sender_stop = true; sender_wake()            ← sender 退出信号
5. pthread_join(sender)                         ← 等它真正结束
      └─ sender 在返回前 close(sock_fd)
6. 清空队列残留；ring_reset()
7. pw_loop_destroy_source(io)                   ← 此刻才关闭 audio_fd
```

即：**任何 fd 的 close 都发生在「可能使用它的那个线程已经确定退出」之后**，且每个 fd 只有
一个 owner 负责关闭（无 double close）。第 4→5 步是这里的关键：`pthread_join` 保证了跨线程
的 happens-before，而不是靠「应该没人用了」的假设。

`pthread_join` 的阻塞上界：sender 只在两处阻塞，都是 `poll(..., SEND_IDLE_MS)`（100 ms），
所以 join 是有界的（最坏约 100 ms），detach 不会挂死 loop thread。

唯一的异常路径：`pthread_create()` 失败。此时判定为「不存在 sender 线程」（无论失败原因是
`EAGAIN` 还是延迟错误），把该 fd 交给 `detach_audio_fd_locked()` 的
「`sender_started == false`」分支关闭，并**放弃这次 attachment**（不假装有播放通路）。

---

## C. RT thread 读写的每个字段分类

`on_capture_process()` 触及的全部字段：

| 字段 | 分类 | 依据 |
|---|---|---|
| `a->capture`（`pw_stream *`） | **immutable during RT** | loop thread 只在 `build_pw()`/`teardown_pw()` 改写，而重建发生在 stream 已 destroy 之后，RT 回调不会再被调用 |
| `b->buffer->datas[0]` / `d->data` / `d->chunk` / `d->maxsize` | **本次 process 私有** | 由 PipeWire 在本次回调期间保证有效，不跨线程共享 |
| `a->silence[]` | **immutable after start** | 只由 `calloc` 置零；`anland_audio_start()` 之后从不写入 |
| `a->send`（`struct send_ring`） | **SPSC，无锁** | `head` 由 RT 唯一写、`tail` 由 sender 唯一写，均为 `_Atomic uint64_t`；两侧用 acquire/release 配对。`buf`/`size` 在 start 后 immutable |
| `a->keepalive_len` | **atomic** | `_Atomic size_t`；RT 用 `acquire` load，loop 用 `release` store |
| `a->epoch` | **atomic** | `_Atomic uint32_t`；RT `acquire` load，loop `release` RMW |
| `a->events` | **atomic** | `_Atomic uint32_t`；RT `release` RMW，loop `acquire` exchange |
| `a->rt_epoch_seen` | **RT-private** | 只有 RT callback 读写；loop thread 从不触碰 |
| `a->rt_pcm_seen` | **RT-private** | 同上（这是问题 4 的修正点） |
| `a->rt_keepalive_on` | **RT-private** | 同上（这是问题 4 的修正点） |
| `a->ctl_write`（int） | **write-once，之后只读** | `anland_audio_start()` 里设定，之后 loop thread 只在 `stop()` 中把它置 `-1`——而那时 loop 已 `pw_thread_loop_stop()`、RT 回调不会再运行。RT 侧只做 `write()` |
| `a->audio_fd` / `a->sock_fd` | **RT 完全不读** | 修订 2 的核心；这两个字段不再出现在 RT callback 中 |
| `a->play_*` / `a->cap_*` | **RT 完全不读** | 只有 loop thread 的 format 协商使用 |

已用脚本核对：RT callback 函数体内不出现 `audio_fd` / `sock_fd` / `sendmsg` / `close` /
`poll` / `recv` / `pw_loop_*` / `pw_thread_loop_*` / `fprintf` / `malloc` / 任何锁。

---

## D. `keepalive_len` 状态机

`keepalive_len` 是**唯一**决定 RT callback 是否投递静音的门禁。

| 事件 | `keepalive_len` | 其它动作 | 代码位置 |
|---|---|---|---|
| 引擎启动（无 consumer） | `0`（`calloc` + 显式 store） | — | `anland_audio_start()` |
| **transport attach**（`anland_audio_set_fd(fd)`） | 保持 `0` | 启动 sender thread；`play_format_known = false` | `anland_audio_set_fd()` |
| 收到任意非 PLAYBACK 消息（mic PCM / CAPTURE FORMAT） | 保持 `0` | — | `on_audio_readable()` |
| **收到 `AUDIO_MSG_FORMAT` 且 `role == AUDIO_ROLE_PLAYBACK`** | `silence_bytes_for(quantum, channels)`（clamp 到 `KEEPALIVE_FRAMES`，再 clamp 到 `sizeof(silence)`） | `play_format_known = true`；`keepalive_len`(release) 先于 `epoch++`(release) | `apply_format()` → `arm_keepalive()` |
| 之后每次 PLAYBACK FORMAT（含格式未变） | 重新 arm（幂等） | 这就是「默认 48000/2 也会 arm」的修正 | 同上 |
| **consumer 断开 / HUP / sender 报错** | `0`(release) | 先归零，再 `epoch++`，再停并 join sender；`play_format_known = false` | `detach_audio_fd_locked()` |
| **reconnect（新 transport attach）** | 保持 `0`，直到新 consumer 再次发 PLAYBACK FORMAT | 新 attachment 从「未确认」开始 | `anland_audio_set_fd()` |
| **PipeWire rebuild**（`build_pw()`，例如 sound service 重启） | 仅当 `a->io != NULL` **且** `play_format_known` 为真才 re-arm；否则保持 `0` | 这正是需求里「只有 consumer 仍 attached 且 playback format 已确认时才能 re-arm」 | `build_pw()` |

RT 侧的处理：

```c
uint32_t epoch = atomic_load_explicit(&a->epoch, memory_order_acquire);
if (epoch != a->rt_epoch_seen) {          /* attach/detach/format 变化 */
    a->rt_epoch_seen = epoch;
    a->rt_pcm_seen = false;
    a->rt_keepalive_on = false;           /* RT 自己 reset，不由 loop thread 写 */
}
size = atomic_load_explicit(&a->keepalive_len, memory_order_acquire);
if (size > 0 && size <= sizeof(a->silence)) { payload = a->silence; ... }
```

`epoch` 的意义：即使 detach 与 reconnect 发生在两次 process 周期之间（RT 从未观察到
`keepalive_len` 变成 0 的那一瞬间），epoch 也一定变了，RT 仍会正确 reset 并重新开始计数，
不会把上一任 consumer 的 `rt_pcm_seen` / `rt_keepalive_on` 带到新 attachment。

---

## E. timer 是 one-shot 还是 periodic

**periodic**（真正的周期 timer）。

```c
/* True periodic tick: pw_loop_update_timer() with a non-NULL interval re-arms itself, so
 * this only has to be armed once (anland_audio_start) and keeps firing forever. */
static void arm_tick(struct anland_audio *a)
{
    struct timespec val = { .tv_sec = TICK_SECS, .tv_nsec = 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(a->loop), a->tick_timer,
                         &val, &val, false);   /* interval = &val, 非 NULL */
}
```

- `TICK_SECS = 1`，`val` 同时作为首次到期时间和 **interval**。
- 只在 `anland_audio_start()` 里调用一次；不再有任何「重新 arm」的逻辑。
- `on_tick()` 不再依赖外部重新 arm：即使 `pw_connected == true` 而提前返回，timer 自身会在
  1 秒后再次触发，因此 `service_rt_state()`（RT 事件落日志 + 响应 sender 的 stop 请求）
  是**持续周期运行**的，而不是像 `89e6ceb` 那样实际只跑一次。

`89e6ceb` 的缺陷与修正对照：

| | `89e6ceb` | `4d17eff` |
|---|---|---|
| `pw_loop_update_timer` 参数 | `&val, NULL, false` → one-shot | `&val, &val, false` → periodic |
| connected 时 | 直接 return，不重新 arm → 之后不再触发 | periodic 自行再触发，无需重 arm |
| `drain_rt_events()` 实际频率 | 通常**只运行一次** | 每 1 秒 |

注意：periodic timer 在无 consumer / PipeWire 未连接时同样每秒触发一次——这是有意的，
既用于重连重试也用于事件落日志，代价是一次 timerfd 唤醒/秒。

---

## F. 编译检查结果

本机（x86_64，无 Rust 工具链）实际执行：

| 检查 | 命令要点 | 结果 |
|---|---|---|
| 严格告警 | `gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter -pthread` | ✅ **0 warning / 0 error** |
| `cc` crate 等价参数 | `gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w` | ✅ 目标文件生成成功 |
| 不加 `-pthread` | `gcc -fsyntax-only …`（无 `-pthread`） | ✅ 通过（glibc ≥ 2.34 中 pthread 已并入 libc；Arch ARM64 满足） |
| 其余 C 单元 | `anland_camera.c`、`display_producer.c`、`socket_utils.c` | ✅ 全部通过 |
| RT callback 纯净性 | 脚本审计函数体，检查 `audio_fd`/`sock_fd`/`sendmsg`/`close`/`poll`/`recv`/`pw_loop_*`/`fprintf` | ✅ 全部不出现 |
| 谁调用 `sendmsg` | `grep -n sendmsg` | ✅ 只有 `sender_send_one()`（sender thread） |
| `pcm_seen`/`keepalive_on` 残留 | `grep -rn` | ✅ 已全部改名 `rt_*`，仅 RT callback 读写 |
| **无锁队列单元测试** | 独立 harness 复制 SPSC slot ring 逻辑，128 B 小环强制 wrap，200 轮 push/peek/consume + 满环/超额/灌满测试 | ✅ **ALL RING TESTS PASSED**（覆盖 length prefix 跨环、payload 跨环、整条不可拆分、超额拒绝） |

> 队列测试过程中出现的 3 次失败均为**测试 harness 自身**问题（断言基准取错、测试间未清空队列、
> payload 期望值硬编码为另一测试的填充模式），已逐一定位并修正；ring 实现本身未改动。

未完成的验证（与之前一致，不因本次修订而改变）：

- ⚪ `cargo build` / `cargo build --release`：本机无 Rust 工具链（MSRV 1.87），无 aarch64 sysroot
  （ARM64 release 构建已在目标设备完成）
- ✅ Android 真机出声、`pw-top` 进入 `R`、KGSL —— **已真机验证通过**
- 🟡 consumer disconnect/reconnect —— 未做完整稳定性验证

---

## 附：提交链

```
4d17eff  backend/anland: take the audio socket out of the realtime callback   ← 本修订
f54d23d  docs/anland-audio: document the RT-safety revision
89e6ceb  backend/anland: make the RT process callback actually realtime-safe
97f2136  docs/anland-audio: add speaker keep-alive fix report and patch
ec9ef2f  backend/anland: keep PipeWire speaker stream alive
cfd31db  (origin/main)

补丁存档: 0001-*.patch (ec9ef2f) / 0002-*.patch (89e6ceb) / 0003-*.patch (4d17eff)
```

`4d17eff` 仅修改
[`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c)
（+535 / −299），未触碰该文件之外的任何代码。

> **真机验证状态更新（后续）**：上述「未完成的验证」是本次修订**当时**的快照。
> speaker / playback 主链路已在目标 ARM64 DroidSpaces 设备验证通过（ARM64 release 构建、
> 新 binary 运行、节点创建、`pw-play` 进入 `streaming`、`pw-top` `R`/`RATE`/`QUANT` 非 0、
> Android 扬声器实际出声）。**Firefox 播放与 mic capture 仍未完成**，
> disconnect/reconnect 未做稳定性验证。当前状态以
> [README.md](README.md)「真机验证状态」一节为准。
