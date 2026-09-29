# Anland 音频：send queue 单写者契约与 sender 细节修复（修订 4）

对应提交 **`bcc887e`**
（`backend/anland: keep the send queue head producer-owned and tidy the sender`）。

本文档是 [QUEUE_FRAMING_REVISION.md](QUEUE_FRAMING_REVISION.md)（描述 `cab5b98`）的后续修订，
共 5 项，均为此前埋下的缺陷或不准确表述。

---

## 1. detach 不得复位 producer-owned 的 `head`（计数器破坏竞态）

### 原代码（`cab5b98`）

```c
/* 错误 */
atomic_store_explicit(&a->send.head, 0, memory_order_release);
atomic_store_explicit(&a->send.tail, 0, memory_order_release);
```

### 为什么是错的

`playback_ready = false` **不能**保证一个已经进入 `on_capture_process()` 的 RT callback
立刻停止。它可能正好执行到 push 的中途：

```
RT:   head_old = load(head)            // 例如 4096
      ... 正在往 ring 写数据 ...
loop:                                  // detach
      playback_ready = false
      epoch++
      join(sender)
      store(head, 0)                   // ← 破坏：计数器被清零
      store(tail, 0)
RT:   store(head, head_old + total)    // → head = 4096 + total，tail = 0
```

结果：`head - tail` 变成一个巨大的值，`send_ring_push()` 的容量判断
`(head - tail) + total > size` 恒为真 → **该 attachment 之后再也推不进任何数据**，
甚至影响后续 attachment。

### 修复

detach 只丢弃**已经发布**的数据：把 `tail` 推到 `head`，绝不写 `head`。

```c
{
    uint64_t head = atomic_load_explicit(&a->send.head, memory_order_acquire);
    atomic_store_explicit(&a->send.tail, head, memory_order_release);
}
```

- `head` 的唯一 writer 始终是 RT producer → **单写者契约不被破坏**；
- 上述"在途 push"随后发布的那个 slot，其 `slot.epoch` 是 detach 刚刚越过的旧 epoch；
- 下一个 sender 在 `sender_send_one()` 里比对 epoch 后**直接丢弃**它（`cab5b98` 已实现），
  不会送给新 consumer。

单写者契约现在显式写进了结构体注释：

```c
struct send_ring {
    uint8_t           *buf;
    size_t             size;
    /* Single-writer contract, which is what makes the queue lock-free: `head` is written
     * ONLY by the realtime producer, `tail` ONLY by the sender thread. The loop thread may
     * read `head` and advance `tail` when it drops published data on detach, but it must
     * never write `head` ... */
    _Atomic uint64_t   head;      /* producer-owned */
    _Atomic uint64_t   tail;      /* consumer-owned */
};
```

> 注：`head`/`tail` 是单调递增的 `uint64_t` 字节计数器，不会回绕复用，
> 因此"不归零"不影响环的 `head & (size-1)` 取模定位。

---

## 2. `send_ring_push()` 的未对齐 struct 指针（C UB）

### 原代码

```c
uint8_t header[sizeof(struct send_slot)];
struct send_slot *slot = (struct send_slot *)header;   /* 未对齐 */
slot->len = ...;                                        /* 解引用 → UB */
```

`uint8_t` 数组只有 1 字节对齐要求，把它 cast 成 `struct send_slot *` 再解引用，
在要求对齐的目标上是未定义行为（aarch64 通常容忍，但不能依赖）。

### 修复

用真正的 struct，iovec 指向它：

```c
struct send_slot slot;
slot.len = (uint32_t)total;
slot.epoch = epoch;
slot.msg = *hdr;

struct iovec src[2] = {
    { .iov_base = &slot, .iov_len = sizeof(slot) },
    { .iov_base = (void *)payload, .iov_len = payload_len },
};
```

---

## 3. wake pipe 用 `pipe2()`，失败即初始化失败

### 原代码

```c
if (pipe(fds) < 0) goto fail;
for (int i = 0; i < 2; i++) {
    int fl = fcntl(fds[i], F_GETFL, 0);
    if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);   /* 失败被忽略 */
    int fdfl = fcntl(fds[i], F_GETFD, 0);
    if (fdfl >= 0) fcntl(fds[i], F_SETFD, fdfl | FD_CLOEXEC);
}
```

**风险**：RT callback 会走 `sender_wake()` → `write()`。如果 `F_SETFL` 失败且被忽略，
写端仍是阻塞的，pipe 满时 `write()` 会**阻塞 realtime 线程**。

### 修复

```c
if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
    goto fail;      /* 初始化失败 */
```

`pipe2()` 原子地创建并设置两个标志，不存在"创建后到设置前"的窗口，
也不再有被忽略的 `fcntl()` 返回值。

---

## 4. 修正文档：RT callback 仍会 `write()` wake pipe

原文声称 RT callback "touches no file descriptor"，**不准确** —— 它会通过
`sender_wake()` 对非阻塞 pipe 写 1 字节。

已改为：

> Note the one syscall it does make -- `sender_wake()` writes a single byte to a
> non-blocking pipe -- so "touches no file descriptor" would be inaccurate; what is true is
> that the descriptor it pokes is owned by this object, always non-blocking (see `pipe2()`
> in `anland_audio_start`), and **never the transport socket**.

准确表述应该是：

| 表述 | 是否准确 |
|---|---|
| "RT callback 不碰 Android socket" | ✅ 准确（唯一 `sendmsg` 在 sender thread） |
| "RT callback 不打开/关闭任何 fd" | ✅ 准确 |
| "RT callback 不碰任何 fd" | ❌ 不准确（写 wake pipe） |

---

## 5. 丢弃 stale slot 应视为"有进展"

原代码丢弃过期 slot 后返回 `SEND_EMPTY`，会让 sender 进入 `poll()` 睡眠 ——
而此时队列里**可能还有** slot（甚至是更多过期 slot）没处理完。

修复：新增 `SEND_DISCARDED`：

```c
enum send_result {
    SEND_EMPTY = 0,   /* nothing queued: the caller should sleep */
    SEND_SENT,        /* one whole wire message was sent and the slot consumed */
    SEND_DISCARDED,   /* a stale slot was dropped: make progress, keep draining */
    SEND_AGAIN,       /* kernel buffer full: the slot was kept for a later pass */
    SEND_DEAD,        /* consumer gone or framing broken: drop the attachment */
};
```

```c
switch (res) {
case SEND_SENT:
case SEND_DISCARDED:
    continue;   /* more may be queued, or more stale slots to drop */
...
}
```

---

## 6. 保持不变的部分

| 要求 | 状态 |
|---|---|
| wire framing（wire 跨度不含本地 slot 头） | ✅ `wire_off = start + sizeof(slot)`，`wire_len = slot.len - sizeof(slot)` |
| `io_fd` / `sender_fd` 两个独立副本 | ✅ `F_DUPFD_CLOEXEC` 恰好 2 处 |
| `playback_ready` | ✅ 保留 |
| slot epoch | ✅ 保留，且现在是 detach 后丢弃在途 push 的依据 |
| mic 非 RT | ✅ `connect_stream(a->source, ..., false)` |
| sender thread 架构 | ✅ 保留 |
| keep-alive | ✅ 保留 |
| 其他 Niri 功能 | ✅ 仅 `anland_audio.c` 一个文件 |

---

## 7. 编译与结构校验

本机实际执行：

| 检查 | 结果 |
|---|---|
| `gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter -pthread` | ✅ **0 warning / 0 error** |
| `gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w`（cc crate 等价） | ✅ OBJ OK |
| 其余 C 单元（camera / display_producer / socket_utils） | ✅ 全部通过 |

结构化校验（脚本，对生产源码）21 项全部通过：

| 检查项 | 结果 |
|---|---|
| 全文件无 `atomic_store(&a->send.head, ...)` | ✅ 0 处 |
| `atomic_store(&r->head, ...)` 恰好 1 处，位于 `send_ring_push()`（producer） | ✅ |
| detach **读** `head`、**写** `tail = head`、**不**清零 `head` | ✅ |
| 无 `(struct send_slot *)` 数组 cast；push 使用真实 `struct send_slot slot;`；iovec 用 `&slot` | ✅ |
| `pipe2(fds, O_NONBLOCK \| O_CLOEXEC)` 存在；无残留 `pipe(fds)`；无残留 `F_SETFL`/`F_SETFD` | ✅ |
| `SEND_DISCARDED` 定义、stale 返回它、sender 在 `SENT\|DISCARDED` 时 `continue` | ✅ |
| RT 路径注释不再声称 "touches no file descriptor" | ✅ |
| 全文件 `sendmsg` 调用点恰好 1 处 | ✅ |
| `F_DUPFD_CLOEXEC` 恰好 2 处 | ✅ |
| mic 仍 `rt_process = false`；`playback_ready`/slot epoch/`MSG_TRUNC`/`AUDIO_MSG_PCM` 保留 | ✅ |

未完成的验证（不变）：❌ `cargo build`（本机无 Rust 工具链，MSRV 1.87）、
❌ aarch64 交叉编译；🔴 Android 真机出声 / `pw-top` 进入 `R` / consumer 重连 / KGSL。

---

## 8. 提交链

```
bcc887e  backend/anland: keep the send queue head producer-owned and tidy the sender  ← 本修订
578f4c0  docs/anland-audio: document the playback queue framing revision
cab5b98  backend/anland: fix playback queue framing, ownership and gating
1d5eb03  docs/anland-audio: document taking the audio socket off the realtime thread
4d17eff  backend/anland: take the audio socket out of the realtime callback
f54d23d  docs/anland-audio: document the RT-safety revision
89e6ceb  backend/anland: make the RT process callback actually realtime-safe
97f2136  docs/anland-audio: add speaker keep-alive fix report and patch
ec9ef2f  backend/anland: keep PipeWire speaker stream alive
cfd31db  (origin/main)

补丁存档: 0001 (ec9ef2f) / 0002 (89e6ceb) / 0003 (4d17eff) / 0004 (cab5b98) / 0005 (bcc887e)
```

`bcc887e` 仅修改 [`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c)
（+60 / −37），未触碰该文件之外的任何代码。

---

## 9. 真机验证要点（累计）

本次修订中的第 1 项影响 **detach/reconnect 后的队列可用性**，是功能性的，请重点确认：

1. **consumer 断开再重连后仍能出声** —— 这是第 1 项修复的直接目标
   （修复前：一次在途 push 会让队列计数器错位，之后再也推不进数据）；
2. 日志正常序列：`audio transport attached` → `audio playback format ...` →
   `speaker stream paused -> streaming` → `keep-alive started` / `playback streaming (real PCM)`；
3. **不应出现** `short audio write, transport detached`（wire 长度与 `audio_msg.size` 不符）；
4. **不应反复出现** `playback queue full, dropped one period`；
5. 正常播放期间**不应反复出现** `audio consumer went away, detaching transport`。

```bash
journalctl -u niri-anland.service -o cat | grep '^anland:'
```
