# Anland 音频：wire 跨度修正（修订 5）与后续风险

对应提交 **`f337d2d`**
（`backend/anland: bound the wire span by the local header, not the whole slot`）。

本文档记录一个**协议 blocker** 的修复，以及一项**已知但未修复**的后续风险。

---

## 1. 协议 blocker：`AUDIO_MSG_PCM` 头被跳过

### 缺陷

`struct send_slot` 的布局是：

```c
struct send_slot {
    uint32_t         len;    /* 本地 */
    uint32_t         epoch;  /* 本地 */
    struct audio_msg msg;    /* ← 这个必须上 wire */
};
```

`sizeof(struct send_slot)` **包含 `msg`**（16 字节），而 `bcc887e` 的
`send_ring_peek()` 用它算 wire 边界：

```c
*wire_off = (start + sizeof(slot)) & (r->size - 1);   /* 16 */
*wire_len = slot.len - sizeof(slot);                  /* slot.len - 16 */
```

于是实际发出去的是：

```
[ PCM payload ]              ← 只有负载，AUDIO_MSG_PCM 头被跳过
```

Android 侧收到的第一条 4 字节会被当成 `type` 解析 —— **整条消息无头**，无法解析。
这是硬性协议失败，不是音质问题。

### 修复

wire 边界改为基于 `offsetof(struct send_slot, msg)`：

```c
/* Bytes of the slot that are ring bookkeeping only. */
#define ANLAND_SLOT_LOCAL   offsetof(struct send_slot, msg)
```

```c
const size_t local = ANLAND_SLOT_LOCAL;   /* local-only prefix, NOT sizeof(slot) */
...
*wire_off = (start + local) & (r->size - 1);
*wire_len = slot.len - local;
```

并新增显式长度校验（slot 至少要能装下本地头 + 一个完整 wire 头）：

```c
if (slot.len < local + sizeof(struct audio_msg) ||
    (size_t)(head - tail) < slot.len)
    return false;
```

局部头读取也只复制 `local` 字节，先 `memset` 清空 struct，因此未发布的 `msg` 字段
永远不会被当作已发布数据：

```c
uint8_t header[ANLAND_SLOT_LOCAL];
...
struct send_slot slot;
memset(&slot, 0, sizeof(slot));   /* msg is not part of the local header */
memcpy(&slot, header, local);
```

`<stddef.h>` 已加入 include（`offsetof`）。

结构体注释现在明确写出为什么不能用 `sizeof()`：

> The local-only prefix is `offsetof(struct send_slot, msg)`, NOT
> `sizeof(struct send_slot)`: the struct also contains the audio_msg that must go on the
> wire, so `sizeof()` would skip the wire header as well and transmit a headerless message.

### 修复前后对照（同一 harness）

从生产源码中**逐字提取** `send_ring_push` / `send_ring_peek` / `send_ring_consume`
生成独立 harness，断言 `wire_len == sizeof(audio_msg) + payload_len` 及
`[audio_msg][PCM]` 字节布局：

| 被测代码 | 结果 |
|---|---|
| `bcc887e`（修复前） | ❌ `FAIL: wire_len == sizeof(audio_msg) + payload_len`、`FAIL: wire[0..4) is audio_msg.type`、`FAIL: wire payload intact` 等，每次迭代必失败 |
| `f337d2d`（修复后） | ✅ **ALL FRAMING INVARIANTS PASSED**（400 轮含跨环、stale-epoch、空队列、超额拒绝、灌满/排空） |

实测常量：`ANLAND_SLOT_LOCAL=8`、`sizeof(struct send_slot)=16`、`sizeof(audio_msg)=8`。
即修复前 `wire_len` 比正确值**少 8 字节**（正好是 `audio_msg` 的大小），与"头被跳过"一致。

> **关于此前遗漏此 bug 的说明**：`bcc887e` 之前的结构化校验只验证了
> `offsetof(msg)==8` 与 `wire_len = slot.len - sizeof(slot)` 两条**各自成立**的语句，
> 但**没有验证两者的不变式** `wire_len == sizeof(audio_msg) + payload_len`。
> 两个各自"看起来对"的表达式组合起来才是错的 —— 这次改为用不变式驱动校验，
> 并用提取真实函数的方式杜绝手工复制偏差。

---

## 2. `SEND_DEAD` 的无效 wake

### 缺陷

```c
case SEND_DEAD:
    atomic_store_explicit(&a->stop_requested, true, memory_order_release);
    sender_wake(a);   /* ask the loop thread to finish the detach */   ← 无效且注释错误
    return NULL;
```

`sender_wake()` 写的是 `ctl_write`，而 `ctl_read` **只由 `sender_thread()` 自己读取**
（loop thread 从不读它，也没有把它注册成 loop source）。所以这个调用：

- **不能**唤醒 PipeWire loop；
- 只是线程退出前的一次无用 write；
- 注释 "ask the loop thread" 是错的。

### 修复

删除该调用，并把真实机制写进注释：

```c
case SEND_DEAD:
    /* Raise the flag for the loop thread and return. Do NOT call sender_wake()
     * here: that pipe is read by this very thread, so it cannot wake the loop.
     * The loop thread picks stop_requested up on its 1 s periodic tick
     * (service_rt_state) and performs the detach. The sender is already gone by
     * then, which detach_audio_fd_locked() handles through sender_started. */
    atomic_store_explicit(&a->stop_requested, true, memory_order_release);
    return NULL;
```

同时把字段注释改为准确表述：

```c
/* Wake-up pipe for the SENDER thread: it is the only reader, and the RT callback (via
 * sender_wake) or the loop thread pokes it to ask for a flush. It deliberately does NOT
 * reach the PipeWire loop -- the loop thread has its own 1 s periodic tick (see
 * on_tick), which is what services stop_requested. */
int ctl_read;    /* read by sender_thread only */
int ctl_write;   /* poked to wake sender_thread */
```

`sender_wake()` 的函数注释也补充了同样的边界说明。

**其余 3 处 `sender_wake()` 调用均指向 sender thread，语义正确、保持不变**：

| 位置 | 调用者 | 目的 | 正确性 |
|---|---|---|---|
| `detach_audio_fd_locked()` | loop thread | 置 `sender_stop` 后唤醒 sender 让它退出 | ✅ 目标是 sender |
| `on_capture_process()` push 成功后 | RT thread | 让 sender 立刻排空（而非等 100 ms） | ✅ 目标是 sender |
| `anland_audio_stop()` 兜底分支 | loop thread | 同上，唤醒以便 join | ✅ 目标是 sender |

**选择"删除无效 wake"而非"新增 sender→loop 的 eventfd"**：loop 侧已有 1 秒 periodic tick
在跑（`service_rt_state` 就在其中），stop 请求的处理延迟上界是 1 秒，完全可接受；
新增一条反向唤醒通道会增加一处 fd 与其生命周期管理，不符合"最小修改"。

---

## 3. 后续风险（本次未修，按要求标记）

### 3.1 FORMAT 变更时 `ready`/`epoch` 早于 `pw_stream_update_params`

`apply_format()` 的当前顺序（loop thread）：

```c
*cur_rate = rate; *cur_channels = channels; *cur_quantum = f->quantum;
if (playback) {
    a->play_format_known = true;
    arm_keepalive(a);           /* ← 这里就 keepalive_len=..., playback_ready=true, epoch++ */
}
if (!format_changed && !quantum_changed) return;
fprintf(...);
if (format_changed) {
    set_latency(stream, f->quantum, rate);
    pw_stream_update_params(stream, params, 1);   /* ← 端口格式到这里才真正切换 */
}
```

**风险窗口**：从 `arm_keepalive()` 到 `pw_stream_update_params()` 真正生效之间存在一个窗口。
在此期间：

- `playback_ready` 已是 `true`、`keepalive_len` 已按**新**格式计算、epoch 已递增；
- 但 stream 的端口格式**仍是旧格式**；
- 因此 RT callback 可能按**新格式**的长度送出 silence（或在 real PCM 分支送出仍在
  按旧格式协商的 PCM），使这一两个周期与 consumer 当前期望的格式不一致。

**影响评估**：窗口极短（同一 loop 线程内几条语句，`update_params` 是异步 renegotiate），
且只在 consumer **改变格式**时出现（常见情形 48000/2 不变，走早返回，无此窗口）；
最坏情况是 consumer 侧一两个周期格式不匹配。

**可选修复方向**（留待后续，避免本次扩大改动）：

1. 把 `arm_keepalive()` 拆开：先发布 `playback_ready`（gate real PCM），
   待 `pw_stream_update_params()` 之后再 bump epoch 并发布 `keepalive_len`；
   在窗口内 RT callback 因 `keepalive_len` 仍为旧值/0 而不发 silence；
2. 或引入两个 epoch（"格式已公告"与"格式已生效"），sender 只发送与**生效** epoch
   匹配的 slot；
3. 或在 `format_changed` 时先暂停 keep-alive，renegotiate 完成后再 arm。

### 3.2 其他既有风险（不变，供汇总参考）

| 风险 | 说明 |
|---|---|
| RT 线程的 wake `write()` | 非阻塞 pipe 写，不睡眠，但仍是 syscall；已在文档中如实描述 |
| keep-alive 空闲 CPU | periodic tick 1 s/次 + 静音周期发送；上界已在报告中评估 |
| 队列满丢弃 | `AUDIO_EV_QUEUE_FULL` 计数并打日志，丢弃整条不拆分 |
| 真机验证 | ✅ speaker / playback 主链路已真机验证通过；Firefox 与 mic capture 仍未验证（见 [README.md](README.md)） |

---

## 4. 保持不变的部分

| 要求 | 状态 |
|---|---|
| wire framing（本次修正的就是它） | ✅ `wire_off = start + ANLAND_SLOT_LOCAL`，`wire_len = slot.len - ANLAND_SLOT_LOCAL` |
| `io_fd` / `sender_fd` 两个独立副本 | ✅ 未动 |
| `playback_ready` | ✅ 未动 |
| slot epoch | ✅ 未动 |
| mic 非 RT | ✅ 未动 |
| sender thread 架构 | ✅ 未动 |
| keep-alive | ✅ 未动 |
| 其他 Niri 功能 | ✅ 仅 `anland_audio.c` 一个文件 |

**未对 format-change 窗口做大改**（按要求），仅在此记录为后续风险。

---

## 5. 编译与校验

| 检查 | 结果 |
|---|---|
| `gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter -pthread` | ✅ **0 warning / 0 error** |
| `gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w`（cc crate 等价） | ✅ OBJ OK |
| 其余 C 单元（camera / display_producer / socket_utils） | ✅ 全部通过 |
| 生产函数逐字提取 harness：framing 不变式 | ✅ **ALL FRAMING INVARIANTS PASSED** |
| 同一 harness 跑 `bcc887e` | ❌ 必失败（证明 bug 与修复） |
| 所有 `sizeof(struct send_slot)` 用法审计 | ✅ 仅注释中提及；`sizeof(slot)` 仅用于 push 写入与 iovec（正确）；peek 一律用 `ANLAND_SLOT_LOCAL` |
| 全文件 `sendmsg` 调用点 | ✅ 恰好 1 处 |
| `F_DUPFD_CLOEXEC` | ✅ 恰好 2 处 |
| `sender_wake` 调用点 | ✅ 3 处，全部指向 sender thread |

未完成的验证（不变）：⚪ `cargo build`（本机无 Rust 工具链，MSRV 1.87）、
⚪ aarch64 交叉编译（ARM64 release 构建已在目标设备完成）；✅ Android 真机出声 /
`pw-top` 进入 `R` / KGSL **已真机验证通过**；🟡 consumer 重连未做完整稳定性验证。

---

## 6. 提交链

```
f337d2d  backend/anland: bound the wire span by the local header, not the whole slot   ← 本修订
1ec6e9e  docs/anland-audio: document the queue ownership revision
bcc887e  backend/anland: keep the send queue head producer-owned and tidy the sender
578f4c0  docs/anland-audio: document the playback queue framing revision
cab5b98  backend/anland: fix playback queue framing, ownership and gating
1d5eb03  docs/anland-audio: document taking the audio socket off the realtime thread
4d17eff  backend/anland: take the audio socket out of the realtime callback
f54d23d  docs/anland-audio: document the RT-safety revision
89e6ceb  backend/anland: make the RT process callback actually realtime-safe
97f2136  docs/anland-audio: add speaker keep-alive fix report and patch
ec9ef2f  backend/anland: keep PipeWire speaker stream alive
cfd31db  (origin/main)

补丁存档: 0001 (ec9ef2f) / 0002 (89e6ceb) / 0003 (4d17eff) / 0004 (cab5b98) /
          0005 (bcc887e) / 0006 (f337d2d)
```

`f337d2d` 仅修改 [`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c)
（+41 / −14），未触碰该文件之外的任何代码。

---

## 7. 真机验证要点

本次是**协议层**修复，之前即使其它环节全部正确也必然听不到声音（消息无头）。
部署后请务必确认：

1. **有声音** —— 这是 wire 带上 `AUDIO_MSG_PCM` 头之后的首次真实验证；
2. 日志**不应出现** `anland: short audio write, transport detached`；
3. 日志**不应反复出现** `anland: playback queue full, dropped one period`；
4. 正常序列：`audio transport attached` → `audio playback format ...` →
   `speaker stream paused -> streaming` → `keep-alive started` / `playback streaming (real PCM)`；
5. consumer 断开重连后仍能出声。

```bash
journalctl -u niri-anland.service -o cat | grep '^anland:'
```

> **真机验证状态更新（后续）**：上述「未完成的验证」是本次修订**当时**的快照。
> speaker / playback 主链路已在目标 ARM64 DroidSpaces 设备验证通过（ARM64 release 构建、
> 新 binary 运行、节点创建、`pw-play` 进入 `streaming`、`pw-top` `R`/`RATE`/`QUANT` 非 0、
> Android 扬声器实际出声）。**Firefox 播放与 mic capture 仍未完成**，
> disconnect/reconnect 未做稳定性验证。当前状态以
> [README.md](README.md)「真机验证状态」一节为准。
