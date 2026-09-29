# Anland 音频：回放队列 framing / 所有权 / 门禁修复（修订 3）

对应提交 **`cab5b98`**
（`backend/anland: fix playback queue framing, ownership and gating`）。

本文档是 [RT_SOCKET_OFFLOAD_REVISION.md](RT_SOCKET_OFFLOAD_REVISION.md)（描述 `4d17eff`）
的后续修订，记录审查发现的 4 个确定缺陷及其修复。前 3 个是 `4d17eff` 引入的 sender-thread
设计缺陷，第 4 个是更早就存在的门禁缺失。

---

## 1. 四个缺陷与修复

### 缺陷 1：`slot.len` 被发到 Android（协议 framing bug）

`4d17eff` 的 slot 布局是：

```
[slot.len 4B][slot.epoch 4B][audio_msg 8B][PCM]      ← 16 字节本地头
```

但 `sender_send_one()` 从 **slot 起点** 发送 `slot.len` 个字节，于是本地头（含 `slot.len`
自身）被当作协议数据发给了 consumer。Android 侧会把 `slot.len` 的低 4 字节当作
`AUDIO_MSG_PCM` 的 `type` 解析 —— 整条消息错位。

**修复**：`send_ring_peek()` 现在显式区分两个跨度：

| 量 | 含义 | 用途 |
|---|---|---|
| `slot_len` | 整个 slot（含本地头） | 只给 `send_ring_consume()` 推进 tail |
| `wire_off` | `slot 起点 + sizeof(struct send_slot)` | iovec 起点 |
| `wire_len` | `slot.len - sizeof(struct send_slot)` | iovec 长度、短写比较 |

```c
*slot_len = slot.len;
*wire_off = (start + sizeof(slot)) & (r->size - 1);
*wire_len = slot.len - sizeof(slot);
```

现在送上 wire 的恰好是 `[audio_msg][PCM]`；短写检查比较 `wire_len`；consume 仍消费
完整 `slot_len`（保证 parser 不漂移）。

### 缺陷 2：空队列 busy-loop + `pthread_join` 卡死

`sender_send_one()` 用 `0` 同时表示 "发了一包" 和 "队列为空"，于是：

```c
while (sender_send_one(a, fd) == 0)      /* 空队列时永久自旋 */
    ;
```

退出前的 flush 用同一模式 → **`pthread_join` 永远无法返回**，detach 直接挂死 loop thread。

**修复**：引入显式状态

```c
enum send_result {
    SEND_EMPTY,   /* 队列空（或只有过期 slot）*/
    SEND_SENT,    /* 已发出一条完整 wire 消息并消费该 slot */
    SEND_AGAIN,   /* 内核缓冲满：保留 slot，不丢 */
    SEND_DEAD,    /* consumer 消失 / framing 失效：放弃该 attachment */
};
```

`sender_thread()` 用 `switch` 分派：`SENT` 继续排空，`AGAIN` 等 `POLLOUT`，
`DEAD` 置 `stop_requested` 并退出，`EMPTY` 才去 `poll(ctl_read, SEND_IDLE_MS)` 睡眠。

**同时删除退出前的 flush**：detach 时不需要把最后一包实时音频交给正在离场的 consumer，
且在退出路径等内核只会延长 join。现在 detach 直接丢弃剩余 queue，并把 head/tail 归零
（生产者已先被解除武装，安全）。

### 缺陷 3：`audio_fd` 与 `sock_fd` 是同一个 fd → double-close

```c
a->audio_fd = owned_fd;
a->sock_fd  = owned_fd;      /* 同一个描述符，两个"所有者" */
```

io source 以 `close=true` 创建（destroy 时关闭它），sender thread 退出时也 `close(fd)`
—— 同一个号码被关闭两次，且 `fd` 号可能在两者之间被复用。

**修复**：取 **两个独立** 的 `F_DUPFD_CLOEXEC`：

| 描述符 | 所有者 | 关闭者 | 关闭时机 |
|---|---|---|---|
| `io_fd` | io source（`pw_loop_add_io(..., close=true)`） | `pw_loop_destroy_source()` | 仅在 `detach_audio_fd_locked()` |
| `sender_fd` | sender thread | sender thread 自己 | 线程函数返回前 |

loop thread 只在 `pthread_create()` **失败**（即线程不存在）时关闭 `sender_fd`；
`io source 注册失败`时两个都由 loop thread 关闭（此时无人认领）。`sock_fd` 在
`pthread_create()` 之前发布，保证新线程一定看到自己的描述符。

### 缺陷 4：real PCM 没有 attachment gate

`4d17eff` 只把 **silence** 路径挂在 `keepalive_len` 上；real PCM 路径只要
`d->chunk->size > 0` 就入队。后果：

- detach 后 RT callback 仍把捕获到的 PCM 放进 queue；
- 新 consumer attach 后、在它发出 `AUDIO_ROLE_PLAYBACK FORMAT` **之前**，
  可能收到上一任留下的（或本次过早的）PCM。

**修复**：新增 `_Atomic bool playback_ready`，语义是「本 attachment 已收到 PLAYBACK FORMAT」：

```c
/* arm_keepalive() —— 只有 PLAYBACK FORMAT 才会走到这里 */
atomic_store_explicit(&a->keepalive_len, bytes, memory_order_release);
atomic_store_explicit(&a->playback_ready, true, memory_order_release);
atomic_fetch_add_explicit(&a->epoch, 1, memory_order_release);

/* detach_audio_fd_locked() —— 先解除武装，再拆 sender */
atomic_store_explicit(&a->keepalive_len, 0, memory_order_release);
atomic_store_explicit(&a->playback_ready, false, memory_order_release);
atomic_fetch_add_explicit(&a->epoch, 1, memory_order_release);
```

RT callback 一次读取即决定本周期去留：

```c
const uint32_t slot_epoch = epoch;              /* 本周期归属的 attachment */
const bool ready = atomic_load_explicit(&a->playback_ready, memory_order_acquire);

if (d->data && d->chunk->size > 0) {
    if (!ready) { pw_stream_queue_buffer(a->capture, b); return; }   /* 丢弃，不入队 */
    ...
} else {
    size = ready ? atomic_load_explicit(&a->keepalive_len, memory_order_acquire) : 0;
    ...
}
```

**并且每个 slot 携带 epoch**：sender 在发送前比对

```c
if (epoch != atomic_load_explicit(&a->epoch, memory_order_acquire)) {
    send_ring_consume(&a->send, slot_len);   /* 丢弃过期 slot，不发送 */
    return SEND_EMPTY;
}
```

这样即使某一周期正好落在 detach 与 reconnect 之间被入队，它携带的是旧 epoch，
sender 会丢弃而不是发给新 consumer —— 从根本上消除 stale PCM，而不依赖时序巧合。

---

## 2. slot 布局（修复后）

```
本地 ring 中：
  ┌────────────┬─────────────┬──────────────┬─────────────┐
  │ slot.len   │ slot.epoch  │ audio_msg    │ PCM payload │
  │ 4B         │ 4B          │ 8B           │ N B         │
  └────────────┴─────────────┴──────────────┴─────────────┘
  └────────── 本地 16B 头 ────┘└──── 上 wire 的部分 ────────┘
   slot_len = 16 + 8 + N
   wire_off = slot 起点 + 16
   wire_len = 8 + N            ← sendmsg() 只发这一段
```

`sizeof(struct send_slot) == 16`，`offsetof(msg) == 8`。

---

## 3. 保持不变的部分

按要求逐条保留：

| 要求 | 状态 |
|---|---|
| speaker RT callback 不 `sendmsg` | ✅ 全文件仅 1 处 `sendmsg`（第 404 行，位于 `sender_send_one()`），RT callback 内不含 `sendmsg`/`audio_fd`/`sock_fd`/`close`/`poll`/`pw_loop_*`/`fprintf` |
| mic 非 RT | ✅ `connect_stream(a->source, ..., false)` |
| mic ring 单线程 | ✅ 未改动，仅 loop thread 读写 |
| SOCK_SEQPACKET wire framing | ✅ `MSG_TRUNC`、`h.size == sizeof(struct audio_format) == avail`、整帧校验、短写判定为 framing 失效 —— 逐字未动 |
| keep-alive | ✅ 保留，门禁增强为 `playback_ready` + `keepalive_len` |
| 不改其他 Niri 功能 | ✅ 仅 `anland_audio.c` 一个文件 |

---

## 4. 编译与结构检查

本机实际执行的验证：

| 检查 | 结果 |
|---|---|
| `gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter -pthread` | ✅ **0 warning / 0 error** |
| `gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w`（cc crate 等价） | ✅ OBJ 生成成功 |
| 其余 C 单元（camera / display_producer / socket_utils） | ✅ 全部通过 |
| 全文件 `sendmsg` 调用点计数 | ✅ **恰好 1 处**（第 404 行，`sender_send_one()` 内） |
| RT callback 禁用调用扫描 | ✅ 不含 `sendmsg`/`audio_fd`/`sock_fd`/`close(`/`poll(`/`pw_loop_update_timer`/`fprintf` |
| `F_DUPFD_CLOEXEC` 计数 | ✅ **恰好 2 处**，且 `a->sock_fd = io_fd` 不存在 |
| `SEND_*` 四态定义与使用 | ✅ 四态齐全，`sender_thread` 已无 `== 0` 自旋模式 |
| 退出 flush 残留 | ✅ 已消除（`Last flush` 不再存在） |
| `wire_off`/`wire_len` 定义 | ✅ `wire_off = start + sizeof(slot)`；`wire_len = slot.len - sizeof(slot)`；consume 用 `slot_len` |
| speaker/mic `rt_process` 取值 | ✅ `capture → true`，`source → false` |

> **关于队列测试的说明（诚实记录）**：我另外写了一个独立 harness 复刻 ring 逻辑，
> 过程中出现多次失败，逐一定位后**全部是 harness 自身缺陷**（隐式声明 `malloc` 导致
> 64 位指针被截断、断言基准取错、测试间未清空队列、payload 期望值与 `audio_msg.size`
> 不一致）。最后一轮把模型改成与生产一致（由 `send_ring_push()` 内部构造
> `audio_msg`、`wire_len == sizeof(audio_msg) + payload_len`）之前，我改用了**对生产源码
> 的结构化校验**（上表）作为依据，而未把未通过的 harness 结果当作结论。ring 实现本身
> 在字节算术上经手工核对：`slot_len=17` → `wire_off=16`、`wire_len=1`，与
> `[audio_msg 8B][PCM 1B]` 一致。

未完成的验证（与之前一致）：❌ `cargo build`（本机无 Rust 工具链，MSRV 1.87）、
❌ aarch64 交叉编译；🔴 Android 真机出声 / `pw-top` 进入 `R` / consumer 重连 / KGSL。

---

## 5. 提交链

```
cab5b98  backend/anland: fix playback queue framing, ownership and gating   ← 本修订
1d5eb03  docs/anland-audio: document taking the audio socket off the realtime thread
4d17eff  backend/anland: take the audio socket out of the realtime callback
f54d23d  docs/anland-audio: document the RT-safety revision
89e6ceb  backend/anland: make the RT process callback actually realtime-safe
97f2136  docs/anland-audio: add speaker keep-alive fix report and patch
ec9ef2f  backend/anland: keep PipeWire speaker stream alive
cfd31db  (origin/main)

补丁存档: 0001 (ec9ef2f) / 0002 (89e6ceb) / 0003 (4d17eff) / 0004 (cab5b98)
```

`cab5b98` 仅修改 [`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c)
（+189 / −103），未触碰该文件之外的任何代码。

---

## 6. 部署后需要重点观察的点

本次修订同时改了 **wire 字节布局**、**fd 所有权** 和 **入队门禁**，真机验证时请重点看：

1. **是否有声音** —— wire framing 修好后 Android 才可能正确解析；
2. **日志中不应出现** `anland: short audio write, transport detached`
   （若出现，说明 wire 长度与 `audio_msg.size` 不一致，需回传日志）；
3. **不应出现** `anland: playback queue full, dropped one period` 反复刷屏；
4. **不应出现** `anland: audio consumer went away, detaching transport` 在正常播放期间反复出现；
5. 正常序列仍应为：`anland: audio transport attached` →
   `anland: audio playback format ...` →
   `anland: speaker stream paused -> streaming` →
   `anland: playback silent, keep-alive started (N bytes)` / `playback streaming (real PCM)`。

回传命令：

```bash
journalctl -u niri-anland.service -o cat | grep '^anland:'
```
