# Anland 音频 RT 安全性修订说明

针对 `ec9ef2f` 的并发 / 实时安全性修订。对应提交 **`89e6ceb`**
（`backend/anland: make the RT process callback actually realtime-safe`）。

本文档修正 [ANLAND_AUDIO_FIX_REPORT.md](ANLAND_AUDIO_FIX_REPORT.md)（描述 `ec9ef2f`）
中**不准确**的若干处，其余根因分析与部署验证流程仍然有效。

---

## 1. 修订项对照

| # | `ec9ef2f` 的问题 | `89e6ceb` 的处理 |
|---|---|---|
| 1 | `connect_stream()` 无条件加 `PW_STREAM_FLAG_RT_PROCESS`，speaker 和 mic **两个** stream 都变成 RT 调度 | `connect_stream()` 增加显式 `bool rt_process` 参数：speaker 传 `true`，mic 传 `false` |
| 2 | mic ring 被并发访问：`on_audio_readable()` 在 loop thread 写，`on_source_process()` 移到 data thread 读 → `ring_head/ring_tail/ring_fill/ring` 的 C data race | mic 不再使用 RT_PROCESS，读写回到同一线程，**恢复原有单线程模型**；ring 未重写 |
| 3 | `on_capture_process()` 内含 `fprintf`，却在注释中宣称「整个 callback realtime-safe」 | RT callback 内**移除全部 stdio**；改为 `_Atomic events` 边沿位 + loop thread 周期 tick 落日志 |
| 4 | `defer_detach()` 从 data thread 调 `pw_loop_update_timer()`（未获官方 RT-safe 保证） | 改为「原子 flag + 非阻塞 pipe 写一个字节」；loop thread 用普通 io source 接收 |
| 5 | `update_silence_size()` 注释称「exactly one AAudio burst」，但实现会 clamp 到 480 帧 | 注释据实改写，明确说明 clamp 语义 |

顺带修掉一个**真实功能漏洞**（见 §5）：consumer 宣告的格式若恰好等于默认值
（48000/2，即绝大多数情况），`apply_format()` 会走「无变化」提前返回，导致 keep-alive
**从未被启用**。

---

## 2. speaker 与 mic 各自运行在哪个线程

| 对象 | 创建方式 | process callback 运行线程 | 依据 |
|---|---|---|---|
| `anland-speaker`（`a->capture`） | `connect_stream(..., rt_process = true)` → 含 `PW_STREAM_FLAG_RT_PROCESS` | **PipeWire realtime data thread**，**不受 thread-loop lock 保护** | PipeWire 官方对 `PW_STREAM_FLAG_RT_PROCESS` 的定义（`stream.h`） |
| `anland-mic`（`a->source`） | `connect_stream(..., rt_process = false)` → **不含** RT_PROCESS | **PipeWire thread loop 线程**（与 `on_audio_readable()` 同一线程） | 不加 RT 标志时 `process` 经 loop 分派 |

因此：

- `on_capture_process()`：data thread，**只能**碰不可变数据 + 原子量。
- `on_source_process()`：loop thread，与 `on_audio_readable()` 同线程 → ring 无竞争。
- `on_audio_readable()`、`on_notify()`、`on_reconnect_timer()`、`on_capture_state_changed()`、
  `apply_format()`、`anland_audio_set_fd()`、`anland_audio_start/stop()`：全部 loop thread。

代码中的对应注释：

- `struct anland_audio` 内 `---- state shared with the realtime process callback ----`
  与 `---- mic ring + loop-thread-only scratch ----` 两个分区的说明；
- `on_source_process()` 上方「Keep it that way: adding RT_PROCESS here would introduce a
  data race on ring_head/ring_tail/ring_fill without a lock-free ring rewrite」；
- `connect_stream()` 上方关于 flags 逐 stream 选择的说明。

---

## 3. RT callback 内剩余调用，逐条说明为什么可接受

`on_capture_process()` 现在只包含下列调用（已用脚本审计该函数体确认）：

| 调用 | 为什么可以留在 RT 路径 |
|---|---|
| `pw_stream_dequeue_buffer()` / `pw_stream_queue_buffer()` | PipeWire 为 RT process callback 设计的标准取/还 buffer API；本 callback 正是通过 `PW_STREAM_FLAG_RT_PROCESS` 被声明为 RT 消费者，这是其预期用法 |
| 读取 `b->buffer->datas[0]` / `d->chunk` / `d->data` / `d->maxsize` | 指向本 stream 的 SPA buffer 元数据，由 PipeWire 在本次 process 期间保证有效；不涉及跨线程共享可变状态 |
| 读写 `a->keepalive_on`、`a->pcm_seen` | **RT 线程私有**：只有本 callback 读写，loop thread 从不触碰（`detach_audio_fd_locked()` 里对它们的复位只发生在该 stream 已不再被调度之后） |
| `atomic_load_explicit(&a->keepalive_len, acquire)` | 无锁原子读；acquire 与 loop thread 的 release store 配对，保证 payload 长度与 `silence[]` 的可见性 |
| `atomic_fetch_or_explicit(&a->events, ..., release)` | 无锁原子 RMW；把「需要打日志的边沿事件」交给 loop thread |
| `atomic_store_explicit(&a->last_errno, ...)` | 无锁原子写，仅为让 loop thread 能打印真实 errno |
| `atomic_exchange_explicit` / `atomic_store_explicit(&a->stop_requested/…)`（在 `defer_detach()` 内） | 无锁原子操作，用于一次性降级为「请 loop thread 收尾」 |
| `write(a->notify_write, &b, 1)`（在 `defer_detach()` 内） | 对**非阻塞** pipe 写 1 字节：纯 syscall，不分配、不休眠、可被 `EAGAIN` 立即返回。这是从其它线程唤醒事件循环的**文档化标准手法**，取代原先对 `pw_loop_update_timer()` 的跨线程调用 |
| 读取 `a->audio_fd` | loop thread 只在 `detach_audio_fd_locked()` 里改它，而该函数**必然先**把 `keepalive_len` 置 0（release）；RT 侧先 acquire 读 `keepalive_len`，为 0 就直接跳过，不会走到 `sendmsg`。故不会出现「用已关闭的 fd 发送」 |
| `sendmsg(..., MSG_DONTWAIT \| MSG_NOSIGNAL)` | 见下一节，**非阻塞但非严格 lock-free**；这是本 callback 里唯一需要坦白的调用 |
| `errno` 读取 | 线程局部（thread-local）变量，无共享 |

**已彻底移除**：`fprintf` / `printf` / `strerror` / `snprintf` / `malloc` / `free` /
`memcpy` / `pw_loop_update_timer` / `pw_thread_loop_*` / 任何锁。

---

## 4. 关于 `sendmsg(MSG_DONTWAIT)` 的准确表述

不宣称它是「完全 RT-safe」，准确说法是：

- **`MSG_DONTWAIT` 保证不阻塞**：不会为等待 socket 发送缓冲区空间而睡眠，缓冲区满时直接返回
  `EAGAIN`/`EWOULDBLOCK`，我们丢弃本周期。
- **但它并非 lock-free**：`unix_dgram_sendmsg()` 会取 socket 的锁（`spin_lock_bh` 级别，
  禁用抢占的短临界区），并按需分配一个 `skb`；在本消息尺寸（8 字节头 + ≤1920 字节负载）下
  走 per-CPU slab 缓存，**不触发可睡眠分配**。
- 因此它是「**不睡眠的 syscall**」，而不是「零开销、无锁」。对 48 kHz / 1024 帧的周期
  （约 47 次/秒）而言这个开销可以忽略，但这和「无锁」是两件事。
- 上游 `lfdevs/mutter` 的 Anland bridge 在同一条路径上也是这么做的（它的
  `on_capture_process()` 同样是 `sendmsg(MSG_DONTWAIT | MSG_NOSIGNAL)`），所以这里保持
  同构的行为，而不是引入一个自创的排空线程。

代码中该表述写在 `sendmsg` 调用上方，与本节一致。

---

## 5. 顺带修掉的真实漏洞：keep-alive 可能从未启用

`ec9ef2f` 用 `audio_fd >= 0 && play_attached` 作为 silence 分支的门禁，而
`play_attached` 只在 `apply_format()` **通过**「格式有变化」检查之后才被置位：

```c
const bool format_changed = (rate != *cur_rate || channels != *cur_channels);
if (!format_changed && !quantum_changed)
    return;                     /* ← 默认值恰好等于真实格式时直接返回 */
...
if (playback)
    a->play_attached = true;    /* ← 永远不会执行 */
```

而 `DEFAULT_RATE = 48000`、`DEFAULT_PLAY_CHANNELS = 2` 与 Android 侧实际读回的
48000/2 恰好相同 —— 这正是用户环境的情况。于是 keep-alive 会被静默禁用。

`89e6ceb` 的处理：

- silence 的唯一门禁改为原子量 `keepalive_len`（0 = 禁用）；
- `apply_format()` 在提前返回**之前**无条件调用 `update_silence_size()`（幂等），
  因此默认格式相同的首次宣告也会正确启用；
- `build_pw()` 重建 stream 时也重新发布一次，使 PipeWire 服务重启后的重建路径无需等
  consumer 重复宣告。

格式/quantum 的「是否有变化」判断语义未变 —— 该判断仍然只用于决定是否
`pw_stream_update_params()` 重新协商。

---

## 6. SOCK_SEQPACKET framing：未改动

以下逻辑逐字保留（`ec9ef2f` 已正确，本次未触碰）：

- `recv(..., MSG_DONTWAIT | MSG_TRUNC)` + `n > sizeof(rx)` 拒绝；
- `AUDIO_MSG_FORMAT` 要求 `h.size == sizeof(struct audio_format) == avail` 完全一致；
- `valid_format()` 边界校验；
- `AUDIO_MSG_PCM` 要求 `h.size == avail` 且整除 `2 * cap_channels`（整帧）；
- `sendmsg` 后检查 `n != sizeof(h) + size` 视为 framing 失效并停用该 attachment。

唯一措辞变化：注释里「One SEQPACKET datagram per period」改为「One whole message per
period」，因为 framing 断言现在主要来自协议层，而不是对 socket 类型的假设。

---

## 7. 验证状态

| 项 | 状态 |
|---|---|
| C 静态检查（`-Wall -Wextra`，含 printf 格式类型检查） | ✅ 0 warning |
| `cc` crate 等价参数编译为目标文件 | ✅ OK |
| 其余 C 单元（camera / display_producer / socket_utils）未受影响 | ✅ OK |
| 用到的原子/API 在 PipeWire 1.0.5 头文件中存在 | ✅ 已核对 |
| 原子变量无非原子直访（脚本审计 6 个 `_Atomic` 字段） | ✅ 0 处 |
| RT callback 内无 `fprintf`/`strerror`/`pw_loop_update_timer`（脚本审计函数体） | ✅ 干净 |
| 仅改动 `anland_audio.c` 一个文件 | ✅ |
| `cargo build` / aarch64 交叉编译 | ❌ 本机无 Rust 工具链与 aarch64 sysroot |
| Android 真机出声 / `pw-top` 进入 `R` / consumer 重连 | 🔴 需部署到目标 ARM64 DroidSpaces 设备验证 |

部署与验证命令仍见
[ANLAND_AUDIO_FIX_REPORT.md](ANLAND_AUDIO_FIX_REPORT.md) §10。

---

## 8. 提交

```
89e6ceb  backend/anland: make the RT process callback actually realtime-safe
97f2136  docs/anland-audio: add speaker keep-alive fix report and patch
ec9ef2f  backend/anland: keep PipeWire speaker stream alive
```

分支 `fix/anland-audio`。`89e6ceb` 仅修改
[`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c)
（+262 / −106），未触碰该文件之外的任何代码。
