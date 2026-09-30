# Anland 音频：sender_fd 退出路径修复（修订 6）

对应提交 **`f9a4af0`**
（`backend/anland: close sender_fd on every sender-thread exit path`）。

这是本轮最后一个确定的生命周期问题。

---

## 1. 缺陷：`SEND_DEAD` 提前 return 导致 `sender_fd` 泄漏

### 原代码（`f337d2d`）

```c
static void *sender_thread(void *data)
{
    const int fd = a->sock_fd;
    ...
    while (!atomic_load_explicit(&a->sender_stop, memory_order_acquire)) {
        enum send_result res = sender_send_one(a, fd);
        switch (res) {
        ...
        case SEND_DEAD:
            atomic_store_explicit(&a->stop_requested, true, memory_order_release);
            return NULL;          /* ← 直接返回，跳过函数尾部的 close(fd) */
        ...
        }
        ...
    }
    close(fd);                    /* ← 只有正常退出循环才执行到 */
    return NULL;
}
```

### 后果

所有走 `SEND_DEAD` 的路径都泄漏 `sender_fd`：

| 触发条件 | 位置 |
|---|---|
| `EPIPE` / `ECONNRESET` / `ENOTCONN` | `sender_send_one()` 的 `sendmsg` 错误分支 |
| 短写（`n != wire_len`） | 同上，framing 失效分支 |
| 未知 send 错误 | 同上，兜底分支 |

**detach 路径不会补 close**：`detach_audio_fd_locked()` 在 `sender_started == true` 时
只做 `pthread_join()`，随后 `sock_fd = -1`；而 loop thread 自己的
`close(sender_fd)` 是为 `pthread_create()` 失败（线程从未存在）预留的。因此
`sender_fd` 确实永久丢失 —— 每次 consumer 异常断开泄漏 1 个 fd。

考虑到 Anland consumer 会反复切后台/重连，这个泄漏是**累积性**的。

### 修复：统一退出路径

```c
        case SEND_DEAD:
            /* Raise the flag for the loop thread and leave through the common exit. Do NOT
             * call sender_wake() here: that pipe is read by this very thread, so it cannot
             * wake the loop -- the loop thread picks stop_requested up on its 1 s periodic
             * tick (service_rt_state) and performs the detach. The sender is already gone by
             * then, which detach_audio_fd_locked() handles through sender_started. */
            atomic_store_explicit(&a->stop_requested, true, memory_order_release);
            goto out;
        ...
    }

out:
    /* The single exit path for every way this thread can end -- normal stop or SEND_DEAD.
     * ...
     * This is the ONLY place sender_fd is closed on the running-thread path, which is what
     * makes "exactly one close" hold: detach_audio_fd_locked() only pthread_join()s when
     * sender_started is set, and the loop thread's own close(sender_fd) is reserved for the
     * pthread_create() failure case where this thread never came into existence. */
    close(fd);
    return NULL;
}
```

`sender_thread()` 现在**只有 1 个 `return`**（在 `out:` 之后），已用脚本核实
（`grep -c return` 于该函数体内 == 1）。

---

## 2. 审计：`sender_fd` 恰好关闭一次

全部 4 处 `close` 调用点及其对应的所有权状态：

| # | 位置 | fd | 条件 | 是否与其它 close 重叠 |
|---|---|---|---|---|
| 1 | `sender_thread()` 的 `out:`（第 511 行） | `fd`（取自 `a->sock_fd`） | 线程的任何退出方式 | ❌ 不重叠：loop 侧只在 `sender_started == false` 时 close |
| 2 | `detach_audio_fd_locked()`（第 540 行） | `a->sock_fd` | `sender_started == false`（含 `pthread_create` 失败） | ❌ 线程不存在，无人关闭 |
| 3 | `anland_audio_set_fd()`（第 1170 行） | `sender_fd` | `pw_loop_add_io()` 注册失败 | ❌ 此时 `pthread_create` 尚未调用 |
| 4 | `anland_audio_set_fd()`（第 1181 行） | `sender_fd` | `pthread_create()` 失败 | ❌ 线程从未存在 |

配套不变式：

- `a->sock_fd` 在 `pthread_create()` **之前**发布（新线程必然看到自己的描述符）；
- `a->sock_fd` 在 detach 的 `pthread_join()` **之后**才置 `-1`，因此「join 完成后
  `sock_fd` 是陈旧值」不会导致二次 close；
- `anland_audio_stop()` 的兜底分支同样先 `sender_stop = true` → `sender_wake()` → `join`，
  再由线程自己 close。

所有权设计（未改）：

```
io_fd     -> 由 PipeWire io source 关闭（pw_loop_add_io(..., close=true) -> pw_loop_destroy_source）
sender_fd -> 由 sender_thread 关闭（统一 out: 路径）
             仅当线程从未创建时，才由 loop thread 关闭
```

---

## 3. 同时删除无调用者的 `send_ring_used()`

`sender` 不再用它清空队列（detach 现在直接把 `tail` 推到 `head`），因此该静态函数
**自 `cab5b98` 起已无任何调用者**，属于死代码。

### 关于"真实 0 warning"的更正

我之前报告的 "0 warning" **不成立**，原因有两层：

1. `-Wall -Wextra` **不包含** `-Wunused-function`；
2. 我用的是 `gcc -fsyntax-only`，它**不产生代码**，因此 `-Wunused-function`
   即使显式打开也不会触发（该警告在 codegen 阶段发出）。

本轮改为**真实 codegen**（`gcc -c`）+ 显式打开相关警告后重测，确认警告确实存在：

```
当前文件（f9a4af0）：  exit=0          ← 无警告
f337d2d（修订前）：    warning: 'send_ring_used' defined but not used [-Wunused-function]
                       /tmp/.../anland_audio.c:230:15
```

使用的完整命令：

```bash
gcc -c -O2 -Wall -Wextra -Wunused-function -Wunused-variable \
    -Wunused-but-set-variable -Wno-unused-parameter -pthread \
    -I src/backend/anland/c \
    -I$P/include/pipewire-0.3 -I$P/include/spa-0.2 \
    src/backend/anland/c/anland_audio.c -o /tmp/ccheck/cur.o
# → exit=0，无输出
```

---

## 4. 保持不变的部分

| 要求 | 状态 |
|---|---|
| wire framing | ✅ 未动，且用提取生产函数的 harness 复测 **FRAMING INVARIANTS STILL PASS** |
| sender architecture | ✅ 未动（仍为「RT 入队 / sender 独占 socket」） |
| `playback_ready` | ✅ 未动 |
| slot epoch | ✅ 未动 |
| keep-alive | ✅ 未动 |
| mic threading | ✅ 未动（仍非 RT、ring 单线程） |
| format-change 风险 | ✅ 仅保留文档说明，未改代码 |

仅修改 [`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c)
（+14 / −13），未触碰该文件之外的任何代码。

---

## 5. 编译与校验

| 检查 | 命令/方法 | 结果 |
|---|---|---|
| 真实 codegen 全警告 | `gcc -c -O2 -Wall -Wextra -Wunused-function -Wunused-variable -Wunused-but-set-variable -Wno-unused-parameter -pthread` | ✅ **exit=0，无输出** |
| 警告真实性反证 | 同命令跑 `f337d2d` | ✅ 复现 `-Wunused-function` 警告 |
| cc crate 等价参数 | `gcc -c -O3 -ffunction-sections -fdata-sections -fPIC ...` | ✅ OBJ OK |
| 其余 C 单元 | camera / display_producer / socket_utils | ✅ 全部通过 |
| `sender_thread` 内 `return` 数量 | `grep -c return` | ✅ **1**（统一在 `out:`） |
| `close` 调用点审计 | 逐点核对所有权状态 | ✅ 4 处互斥，`sender_fd` 恰好关闭一次 |
| `send_ring_used` 残留 | `grep` | ✅ 已删除 |
| framing 不变式 | 提取生产函数的 harness（400 轮跨环 + stale-epoch + 空/超额/灌满） | ✅ **FRAMING INVARIANTS STILL PASS** |

未完成的验证（不变）：⚪ `cargo build`（本机无 Rust 工具链，MSRV 1.87）、
⚪ aarch64 交叉编译（ARM64 release 构建已在目标设备完成）；✅ Android 真机出声 /
`pw-top` 进入 `R` / KGSL **已真机验证通过**；🟡 consumer 重连未做完整稳定性验证。

---

## 6. 提交链

```
f9a4af0  backend/anland: close sender_fd on every sender-thread exit path            ← 本修订
073335b  docs/anland-audio: document the wire-span fix and the remaining format-change risk
f337d2d  backend/anland: bound the wire span by the local header, not the whole slot
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

补丁存档: 0001 … 0007
```

---

## 7. 累计真机验证清单

| # | 验证点 | 相关修订 |
|---|---|---|
| 1 | 有声音 | 5（wire framing） |
| 2 | 无 `short audio write, transport detached` | 5 |
| 3 | 无反复 `playback queue full, dropped one period` | 3 |
| 4 | 正常播放期间无反复 `audio consumer went away` | 4 |
| 5 | consumer 断开重连后仍能出声 | 4（head 单写者）/ 6（fd 不再泄漏） |
| 6 | 长时间多次重连后 fd 数不增长 | **6** |
| 7 | 正常序列：`audio transport attached` → `audio playback format ...` → `speaker stream paused -> streaming` → `keep-alive started` / `playback streaming (real PCM)` | 全 |

fd 泄漏的现场观测方法（重连若干次后对比）：

```bash
PID=$(systemctl show -p MainPID --value niri-anland.service)
ls /proc/$PID/fd | wc -l        # 反复 disconnect/reconnect 后不应持续增长
ls -l /proc/$PID/fd | grep -c socket
```

```bash
journalctl -u niri-anland.service -o cat | grep '^anland:'
```

> **真机验证状态更新（后续）**：上述「未完成的验证」是本次修订**当时**的快照。
> speaker / playback 主链路已在目标 ARM64 DroidSpaces 设备验证通过（ARM64 release 构建、
> 新 binary 运行、节点创建、`pw-play` 进入 `streaming`、`pw-top` `R`/`RATE`/`QUANT` 非 0、
> Android 扬声器实际出声）。**Firefox 播放与 mic capture 仍未完成**，
> disconnect/reconnect 未做稳定性验证。当前状态以
> [README.md](README.md)「真机验证状态」一节为准。
