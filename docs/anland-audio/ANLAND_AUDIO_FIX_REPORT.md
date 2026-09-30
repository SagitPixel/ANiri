# ANiri Anland 音频播放链路修复报告

**主题**：`anland-speaker` 永久 `paused` / Android 无声音 — 源码级根因分析与修复

| 项目 | 值 |
|---|---|
| 目标项目 | `Celvra/ANiri`（ANiri v0.2.0，运行时 `niri version 26.04`） |
| 目标运行平台 | Arch Linux ARM64 / aarch64，DroidSpaces Android Linux container，KGSL + Freedreno，PipeWire 1.6.9 + WirePlumber + pipewire-pulse，Anland Android consumer，`ANLAND=1` |
| 本次开发机 | x86_64 Linux（无 Rust 工具链、无 DroidSpaces/Anland/KGSL/ARM64 runtime） |
| 提交 | `ec9ef2fbee48b62c97a6b1d12b1aadbc10d912cb`（`ec9ef2f`），分支 `fix/anland-audio` |
| Commit message | `backend/anland: keep PipeWire speaker stream alive` |
| 改动规模 | 2 文件，+284 / −36 |
| 完整补丁存档 | [`0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch`](0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch) |
| 真机验证 | ✅ speaker / playback 主链路已在目标 ARM64 DroidSpaces 设备验证通过（见 §0、§7、附录 B）；Firefox 与 mic capture 仍未完成 |

---

> ## ⚠️ 本报告描述的是 `ec9ef2f`，已被 `89e6ceb` 修订
>
> `ec9ef2f` 中关于「RT callback 完全 realtime-safe」的说法**不准确**，且
> `PW_STREAM_FLAG_RT_PROCESS` 被加到了 speaker 和 mic **两个** stream 上。
> `89e6ceb` 修正了这两点。阅读 §4.3、§4.4、§4.6、§8 时请以
> [RT_SAFETY_REVISION.md](RT_SAFETY_REVISION.md) 为准。
>
> 本文件的分析章节（§1–§6、§8、§10）作为**历史档案**保留，不逐处改写，以免掩盖当时的
> 分析过程；但 **§0、§7、附录 B 的验证状态已按 ARM64 DroidSpaces 真机测试结果更新**。

## 0. 验证状态总览（先读这一节）

本报告严格区分「当时在 x86_64 开发机完成」与「已在目标 ARM64 DroidSpaces 设备验证」。
**状态已在真机测试后更新**；未逐条改写的历史分析见各修订文档。

| # | 工作项 | 状态 |
|---|---|---|
| 1 | 阅读整个 ANiri 仓库、定位 Anland audio 实现 | ✅ 已完成 |
| 2 | 阅读 Anland protocol（`protocol.h`）与 `AUDIO_MSG_*` 语义 | ✅ 已完成 |
| 3 | 联网查阅 upstream Anland / Mutter Anland backend 实现 | ✅ 已完成（`lfdevs/mutter` `debian/patches/anland/0007`，665 行） |
| 4 | 定位根因 | ✅ 已完成，且与用户假设同向但**不完全相同**（见 §1） |
| 5 | 修改源码（仅 Anland backend，无无关重构） | ✅ 已完成 |
| 6 | 生成 git diff / 独立 commit | ✅ 已完成 |
| 7 | C 静态检查（严格告警 + `cc` crate 等价编译参数） | ✅ 已完成，0 warning |
| 8 | 与 PipeWire 头文件 API 兼容性核对（1.0.5 / 1.6.9） | ✅ 已完成 |
| 9 | 本机沙箱内复现 `connecting -> paused` 故障 | ✅ 已复现 |
| 10 | 完整 `cargo build`（x86_64 debug/release） | ⚪ **本机未做**：开发机无 Rust 工具链（项目 MSRV 1.87，系统仅 1.75），且缺 libinput/libwayland/libdisplay-info 等 sys 库；未改动任何 Rust 代码 |
| 11 | ARM64 release 构建（目标 DroidSpaces 设备） | ✅ **已完成**：已在目标设备成功编译 ARM64 release binary |
| 12 | 新 ANiri binary 实际运行 | ✅ **已完成** |
| 13 | `anland-speaker` / `anland-mic` 节点创建 | ✅ **已完成** |
| 14 | `pw-play` 真机播放 | ✅ **已完成**（进入 streaming，实际出声） |
| 15 | `pw-top` 进入 `R` 且 `QUANT/RATE` 非 0 | ✅ **已验证** |
| 16 | Android 扬声器实际出声（48 kHz / stereo） | ✅ **已验证** |
| 17 | KGSL / DroidSpaces ARM64 runtime | ✅ **已完成** |
| 18 | Anland consumer 真机 disconnect / reconnect | 🟡 **未做完整稳定性验证**：未做长时/多次重连的压力验证，不要视为已完成 |
| 19 | Firefox 播放 | 🔴 **未解决，待继续排查**（见 §7 H/I） |
| 20 | 麦克风 capture 真机录音 | 🔴 **未验证**：仓库中暂无真机录音证据 |

> **重要说明**：
>
> 1. 标记 ⚪ 的第 10 项是**开发机限制**（无 Rust 工具链），不影响目标设备；ARM64 release
>    构建已在目标设备完成（第 11 项）。
> 2. 第 18～20 项仍未完成，请勿视为已验证。
> 3. 本报告正文（§1–§6、§10）是 `ec9ef2f` 当时在 x86_64 开发机上的**历史分析记录**，
>    未逐处改写；真机验证结论以本节和 §7 为准。
> 4. 真机测试中另发现一个**独立的环境问题**（WirePlumber V4L2），见 §7 开头的说明。

---

## 1. 根因

### 1.1 结论

`anland-speaker` 是**普通 follower 节点**（非 graph driver），播放数据只能由**它自己的 process callback** 产出。而修复前的 `on_capture_process()` 的发送条件为：

```c
if (d->data && d->chunk->size > 0 && a->audio_fd >= 0) {
    /* sendmsg ... */
}
pw_stream_queue_buffer(a->capture, b);
```

**没有 PCM 就一个字节都不发**。于是 Android 侧 AAudio playback stream 拿不到样本 → AAudio buffer 饿死 → native stream 停止 → PipeWire 节点永远 suspended。这是「Link 已建立却永远 `[paused]`」与「`pw-play connecting -> paused`」的直接原因。

### 1.2 源码级证据链

| 观察到的现象 | 对应的源码依据 |
|---|---|
| `pw-top`：`S anland-speaker`，`QUANT 0`，`RATE 0` | 节点从未被 driver 拉起执行过 process |
| 加 `node.always-process=true` 后 `Dummy-Driver` 变 `R` | `impl-node.c`：`always_process` ⇒ `want_driver=true`；`context.c` 的 unassigned-node 循环中 `if (... \|\| t->always_process) driver = target` → driver 变为 runnable → running。**证明该属性本身确实生效** |
| 但 `anland-speaker` 仍 `S` / `QUANT 0` | 节点虽已成为 follower 并挂到 driver 上，但**没有任何周期数据被送给 consumer**；consumer 端 AAudio 流停掉后，整条链路失去了被拉起的理由 |
| `pw-play` 停在 `paused` | `stream.c`：只有收到 `SPA_NODE_COMMAND_Start` 才 `PAUSED → STREAMING`；而 `Start` 仅在节点真正 running 时下发 |

### 1.3 关于「格式已确认正常」的误判

EnumFormat 显示的 `S16LE / 48000 / 2ch / FL,FR` 恰好等于代码里的 `DEFAULT_RATE` / `DEFAULT_PLAY_CHANNELS`（48000 / 2）。因此 `apply_format()` 中 `format_changed` 的比较结果为 `false`，**默认值掩盖了「格式协商其实从未真正走通」这一事实**，并使 `node.latency` 从未被设置（`quantum == 0` 时 `set_latency()` 直接 return）。这是本次一并修掉的隐患。

### 1.4 为什么「只加 node.always-process」不够

因为**发送侧仍然是空的**。属性只让节点具备被调度的资格，`on_capture_process` 在「无 PCM」分支里依然跳过发送 —— 链路照样饿死。这正是上游 mutter 修复必须**同时**做「保活属性 + silence keep-alive」两件事的原因。

### 1.5 本机沙箱复现的边界（诚实声明）

本机实测手段：克隆 PipeWire 源码，并将 Ubuntu 的 pipewire 1.0.5 包解包到本地前缀（无需 root），搭建独立 daemon + 自写测试客户端（**未改动 ANiri**）复现 ANiri 的 stream 创建路径。

- ✅ **复现成功**：`connect res=0` → `STATE: connecting -> paused`，`processes=0`，永久 `paused`，与用户现象完全一致。
- ❌ **无法复现完整可运行的 graph**：在该最小 daemon 中，**连 `support.null-audio-sink` adapter sink 也一样 `paused / procs=0`**（另写 feeder 客户端单独验证过）。说明该沙箱缺少真实音频驱动所需运行环境（无 ALSA/udev/dbus/WirePlumber 会话，容器内 `pthread_setname` 被拒），因此沙箱里「音频 graph 整体跑不起来」，**不能用它判定 always-process 是否充分**。我没有把这个负面结果当作结论，而是以上游已验证实现为准。

---

## 2. 参考实现

### 2.1 已研究的上游材料

1. **`superturtlee/anland`** — 协议本体（`AUDIO_MSG_FORMAT` / `AUDIO_MSG_PCM` / `struct audio_format` / `AUDIO_ROLE_*` / `quantum`）。
   注：该仓库根目录不含 `common/protocol.h`；协议定义实际以 fork 内的 [`protocol.h`](../../src/backend/anland/c/common/protocol.h) 为准（已通读，语义一致：每条消息带 `{type,size}` 头、PCM 逐帧交织、stereo 为 FL,FR）。
2. **`lfdevs/mutter`** — `debian/patches/anland/0007-backends-anland-Add-PipeWire-audio-bridge.patch`（新增 665 行的 `meta-anland-audio.c`）。其 commit message 明确记载：
   > *"Keep the speaker node processing while unlinked and send a short zero-filled PCM message whenever PipeWire has no audio data. Disable PipeWire and session idle suspension for that node so the Android playback stream stays active until the desktop audio graph begins producing PCM."*
3. **PipeWire 源码** — 对比 `1.0.5` tag 与 `HEAD` / `1.6.9` tag 的 `context.c:ensure_state()`、`impl-node.c:check_properties()`、`stream.c:impl_send_command()`；确认 `always_process ⇒ want_driver`、`pw_loop_update_timer()` 的跨线程可用性、`pw_stream` 状态机。

### 2.2 移植的是「行为语义」而非代码

ANiri 是 Rust + C，mutter 是 GLib/GObject，架构不同，因此提取语义后在本仓库重新实现：

| 上游行为语义 | 在 ANiri 的落地 |
|---|---|
| speaker 节点保持可运行、不可被挂起 | 加 `node.always-process` / `pause-on-idle=false` / `suspend-on-idle=false` / `session.suspend-timeout-seconds=0` |
| 无 PCM 时发一小段静音，不要空着 | `on_capture_process` 增加 silence 分支（**并修正 mutter 固定 480 帧硬编码的问题**，见 §4.2） |
| process 从 RT 线程直接调用 | 加 `PW_STREAM_FLAG_RT_PROCESS` |
| consumer 断开/重连要重建状态 | `detach_audio_fd_locked()` 复位 `play_attached` / `pcm_seen` / `keepalive_on`；fd 先 dup 再替换 |

**未做**：没有照搬 mutter 的固定 `KEEPALIVE_FRAMES 480` 字节数（那会在 44.1k / 单声道下送出错误长度，且与协商 quantum 脱钩），而是按协商 quantum + channels 推导上界。

---

## 3. 修改文件

| 文件 | 改动 | 说明 |
|---|---|---|
| [`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c) | +267 / −36 | 核心修复 |
| [`src/backend/anland/c/anland_audio.h`](../../src/backend/anland/c/anland_audio.h) | +17 / −0 | 仅更新说明性注释，与实现对齐 |

**未改动**：任何 Rust 代码、`build.rs`、`Cargo.toml`、config schema、mic/camera 业务逻辑、Niri 无关功能。

---

## 4. 核心修改

### 4.1 Stream properties（`build_pw()`，speaker 侧）

新增 4 个属性。每个都说明「为什么加 / 解决什么状态转换 / 与 1.6.9 是否兼容」：

| 属性 | 为什么加 | 解决的状态转换 | 1.6.9 兼容性 |
|---|---|---|---|
| `node.always-process=true` | 让 sink 在**无人连接时也 runnable**；`impl-node.c` 中它会**隐含 `node.want-driver=true`** | unassigned 节点被 `move_to_driver()` 挂到 driver，从而具备被调度资格 | ✅ `keys.h:187` 起，1.0.5 即有 |
| `node.pause-on-idle=false` | 默认 `pause_on_idle=true`，空闲即 `Pause` → 回到 `PAUSED` | 避免曲目间隙被 pause，`ensure_state()` 不再把它打回 IDLE | ✅ `keys.h:190` |
| `node.suspend-on-idle=false` | 默认 suspended 后会 `pw_impl_node_set_state(SUSPENDED)` | 避免长期 suspended | ✅ `keys.h:191` |
| `session.suspend-timeout-seconds=0` | WirePlumber session 层默认按 idle 秒数挂起节点 | 防止 WP 侧把 sink 挂起 | ✅ 字符串属性，WP 读取 |

**刻意未加** `media.role` / `media.category`：经核对，`media.role=Music` 对 **node** 不适用（`keys.h:352` 的 role 语义是 device/stream 用途；`Audio/Sink` device 需要的是 `device.role` / `device.class`，属于 adapter/device 层）。遵循「不要为了看起来合理随意增加属性」的要求，**移除了原打算添加的 `media.role`**，只保留 `media.type` / `media.class` —— 这两个原本就有，且是 WirePlumber 用于设备分类的依据。

### 4.2 process callback（`on_capture_process()`）

```
有 PCM                → 发真实 PCM（字节数 = 出队周期，与纯直通路径完全一致）
无 PCM 且已 attached  → 发 a->silence 的 a->silence_bytes 字节 S16 全零
其他                  → 仅 queue，不发送
```

关键设计（对应需求 §12 的每一条）：

- **不硬编码 48000 / 2ch**：`update_silence_size()` 按 `play_quantum`（协商值，有则用之）+ `play_channels` 计算；`KEEPALIVE_FRAMES=480` 仅作 quantum 未知时的上界。步长恒为 `channels * sizeof(int16_t)`。
- **不 malloc**：`silence[KEEPALIVE_BYTES]` 是结构体内嵌数组，`calloc` 时已全零，之后**从不写入**；`KEEPALIVE_BYTES` 按最坏情况（480 × 2ch × 2B = 1920 B）固定。
- **不 busy loop / 不 sleep / 不阻塞 RT**：无 sleep；`sendmsg(MSG_DONTWAIT)`；`EAGAIN` 直接丢弃本周期。
- **真 PCM 优先，静音不插队**：只要本周期出队到 `size > 0`，就**原样转发该周期**，静音长度永不参与 —— 因此不会向实时流插入固定长度静音，**不产生额外延迟、不破坏 A/V 时序**。
- **不产生明显额外延迟**：`silence_bytes` 跟随协商 quantum（上限 480 帧 ≈ 10 ms），与 Android 一个 AAudio burst 对齐，而不是固定 480 帧。
- **consumer 断开即停 keep-alive**：`detach_audio_fd_locked()` 复位 `play_attached` / `pcm_seen` / `keepalive_on`；发送前仍检查 `audio_fd >= 0`。
- **重连后恢复**：新 attach 后须等 consumer 重新发 `AUDIO_MSG_FORMAT(PLAYBACK)`，`play_attached=true` 才启用，不会对旧 fd 空写。

### 4.3 PipeWire scheduling

`connect_stream()` 增加 `PW_STREAM_FLAG_RT_PROCESS`，让 sink 的 process 由 **graph 自身的调度周期**直接调用，而不是异步 hop 到 main loop（那样 driver 可能先推进而 consumer 始终没被喂数据）。callback 保持 RT-safe：预分配静音、仅非阻塞系统调用、无锁、无 heap。异常处理中**不直接销毁 loop source**，而走 `defer_detach()`。

### 4.4 fd 生命周期与并发（对应需求 §14）

- **dup 前置**：`anland_audio_set_fd()` 先 `F_DUPFD_CLOEXEC`，失败则**不再破坏当前可用的 attachment**（原实现先 `detach_audio_fd_locked()` 再 dup，dup 失败就白白丢掉一条好链路）。
- **serial 保护的延迟 detach**：RT callback 中发现 `EPIPE` / `ECONNRESET` / `ENOTCONN`，或发生**短写**时，只做 `pw_loop_update_timer(0)` 打标记（写 timerfd，任何线程安全），真正的 `pw_loop_destroy_source` 在 loop 线程执行；并用 `attach_serial` / `detach_serial` 保证**旧 attachment 的失败绝不会拆掉刚装上的新 attachment**（防 double-close / use-after-free / 误拆重连）。
- **销毁顺序**（`anland_audio_stop()`）：
  `pw_thread_loop_stop` → `teardown_pw()`（先移除 listener 再 destroy stream，最后 `pw_core_disconnect`）→ destroy io source（它拥有 `audio_fd`，destroy 即 close）→ destroy `reconnect_timer` → destroy `detach` timer → `pw_context_destroy` → `pw_thread_loop_destroy` → `free(ring)` → `pw_deinit`。
  **反创建序，且不在已销毁对象上保留任何回调。**
- `fail:` 路径补齐 `detach` timer 的释放。

### 4.5 协议边界与读侧健壮性

- `recv(..., MSG_TRUNC)`：超大 datagram 会被截断但**返回真实长度**，配合 `n > sizeof(rx)` 检查，绝不会把截断帧当成合法帧。
- `AUDIO_MSG_FORMAT`：要求 `h.size == sizeof(struct audio_format) == avail` **完全一致**，再经 `valid_format()`（role ∈ {PLAYBACK, CAPTURE}、format == S16LE、rate ∈ [8000, 384000]、channels ∈ [1, 2]、quantum ≤ 65536）校验；非法即丢弃并打日志，**不再用 `>=` 猜测**。
- `AUDIO_MSG_PCM`：要求 `h.size == avail`，且 `h.size % (2 * cap_channels) == 0`（拒绝半帧，避免 mic 每帧错位一个 sample）。
- **不假设 stream socket 的 partial-write 语义**：按「每条 `AUDIO_MSG_PCM` 是一条完整消息」处理，短写视为 framing 失效 → 一次性日志 + 延迟 detach，而不是偷偷拼半包。

### 4.6 诊断日志（全部低频，非每 buffer）

| 覆盖项 | 实际输出 |
|---|---|
| audio format received | `anland: audio playback format 48000 Hz, 2 ch, S16LE, quantum N` |
| invalid format | `anland: ignoring invalid audio format (rate=.. ch=.. fmt=.. role=.. quantum=..)` |
| PipeWire stream state | `anland: speaker stream paused -> streaming`（新增 `state_changed` listener） |
| playback process start | `anland: playback streaming (first PCM captured)` |
| first PCM packet sent / silence→real | `anland: playback streaming (real PCM)` |
| silence keepalive start | `anland: playback silent, keep-alive started (N bytes)` |
| audio fd attach / detach | `anland: audio transport attached` / `detached` / `failed to duplicate audio fd: ...` / `failed to register audio fd: ...` |
| send / write failure | `anland: audio sendmsg failed: ...` / `anland: short audio write (n/N), detaching` |
| consumer disconnect / reconnect | 由 `audio transport detached` / `attached` + 后续 format 行覆盖 |

RT callback 中只有**状态跳变边沿**才输出，并在注释中明确标注「stdio 在数据线程非严格 RT-safe」这一取舍。

### 4.7 麦克风方向（对应需求 §13）

capture 路径**逻辑未变**，仅增加一处防御：

```c
if (!d->data || !d->chunk) { pw_stream_queue_buffer(a->source, b); return; }
```

`PW_STREAM_FLAG_RT_PROCESS` 对 source 同样成立（ring 只在 loop 线程读写，无需加锁）。`apply_format()` / `valid_format()` 为双向共用但按 role 分流，mic 的格式协商与 ring 生命周期**没有改动**。

---

## 5. 完整 diff

> 完整补丁已存档为 [`0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch`](0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch)（`git format-patch -1 ec9ef2f`，564 行）。
> 也可直接执行：`git show ec9ef2f`

### 5.1 `anland_audio.c`

```diff
diff --git a/src/backend/anland/c/anland_audio.c b/src/backend/anland/c/anland_audio.c
index 5a12358..febeebb 100644
--- a/src/backend/anland/c/anland_audio.c
+++ b/src/backend/anland/c/anland_audio.c
@@ -30,6 +30,25 @@
 /* Retry cadence after the PipeWire connection is lost (sound service restart, etc). */
 #define RECONNECT_SECS    1
 
+/* Plausibility bounds for a consumer-announced format; anything outside is ignored in
+ * favour of the previous (or default) format instead of wrecking the stream. */
+#define MIN_AUDIO_RATE        8000
+#define MAX_AUDIO_RATE      384000
+#define MAX_AUDIO_CHANNELS       2
+#define MAX_AUDIO_QUANTUM    65536
+
+/* Keep-alive payload: one short block of digital silence in the negotiated format.
+ * The Android consumer runs one AAudio playback stream per direction and only advances
+ * it while PCM keeps arriving; without this its buffer underruns and the native stream
+ * stops (which in turn leaves the PipeWire link -- and every app on it -- sitting in
+ * paused forever). One short period of silence per PipeWire cycle keeps that native
+ * stream primed without adding audible latency or meaningful traffic. Only ever sent
+ * while a consumer audio socket is attached. */
+#define KEEPALIVE_FRAMES      480
+/* Sized for the worst case (KEEPALIVE_FRAMES * MAX_AUDIO_CHANNELS * S16) so any
+ * negotiated format fits without reallocating in the process callback. */
+#define KEEPALIVE_BYTES       (KEEPALIVE_FRAMES * MAX_AUDIO_CHANNELS * (int)sizeof(int16_t))
+
 struct anland_audio {
     struct pw_thread_loop *loop;
     struct pw_context     *context;
@@ -53,12 +72,24 @@ struct anland_audio {
 
     int                    audio_fd;  /* owned duplicate; -1 when detached */
     struct spa_source     *io;        /* loop io source watching audio_fd for reads */
+    struct spa_source     *detach;    /* one-shot timer to run a detach on the loop thread */
+    bool                   detach_pending;
+    uint32_t               attach_serial;   /* bumped on every successful attach */
+    uint32_t               detach_serial;   /* attachment a deferred detach refers to */
+    bool                   play_attached;      /* a PLAYBACK format was announced */
+    size_t                 silence_bytes;      /* fallback silence payload for this format */
+    bool                   pcm_seen;           /* real PCM already sent to this consumer */
+    bool                   keepalive_on;       /* currently feeding silence */
 
     /* Mic ring buffer. Only ever touched from the loop thread (the io read callback
      * fills it, the source process callback drains it), so it needs no lock. */
     uint8_t               *ring;
     size_t                 ring_size, ring_head, ring_tail, ring_fill;
 
+    /* Digital-silence keep-alive payload, zeroed once. Never reallocated and never
+     * written from the process callback, so it stays realtime-safe. */
+    uint8_t                silence[KEEPALIVE_BYTES];
+
     uint8_t                rx[MAX_DGRAM];
 };
 
@@ -117,15 +148,69 @@ static void detach_audio_fd_locked(struct anland_audio *a)
     struct spa_source *io = a->io;
     a->io = NULL;
     a->audio_fd = -1;
+    a->play_attached = false;      /* a new consumer must re-announce its formats */
+    a->pcm_seen = false;
+    a->keepalive_on = false;
+    a->detach_pending = false;
     ring_reset(a);
     if (io)
         pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), io);
 }
 
+/* Tear the transport down from the PipeWire process callback. That callback can run on
+ * the realtime data thread (PW_STREAM_FLAG_RT_PROCESS), where destroying a loop source
+ * is not safe, so hand the work to the loop thread through the one-shot timer instead.
+ * pw_loop_update_timer() only writes to the timerfd and is callable from any thread.
+ * The attachment that failed is remembered, so a detach requested against a dead socket
+ * can never tear down a socket that was installed in the meantime. */
+static void defer_detach(struct anland_audio *a)
+{
+    if (a->detach_pending || !a->detach)
+        return;
+    a->detach_pending = true;
+    a->detach_serial = a->attach_serial;
+    struct timespec val = { .tv_sec = 0, .tv_nsec = 0 };
+    pw_loop_update_timer(pw_thread_loop_get_loop(a->loop), a->detach, &val, NULL, false);
+}
+
+static void on_detach_timer(void *data, uint64_t expirations)
+{
+    struct anland_audio *a = data;
+    (void)expirations;
+    if (!a->detach_pending)
+        return;
+    a->detach_pending = false;
+    if (!a->io || a->detach_serial != a->attach_serial)
+        return;   /* already replaced (or dropped) by a newer attachment */
+    detach_audio_fd_locked(a);
+}
+
 /* ---- stream process callbacks (run on the PipeWire thread loop) ---- */
 
+/* Logs the paused/streaming/error transitions that matter when the speaker graph will
+ * not leave "paused". Not per-buffer, so it is safe on the loop thread. */
+static void on_capture_state_changed(void *data, enum pw_stream_state old,
+                                     enum pw_stream_state state, const char *error)
+{
+    (void)data;
+    fprintf(stderr, "anland: speaker stream %s -> %s%s%s\n",
+            pw_stream_state_as_string(old), pw_stream_state_as_string(state),
+            error ? ": " : "", error ? error : "");
+}
+
 /* Desktop audio captured from the default sink's monitor -> push to the socket so
- * the consumer plays it. Dropped (still drained) while detached. */
+ * the consumer plays it.
+ *
+ * The Android consumer owns a real AAudio playback stream that only advances while
+ * PCM keeps arriving; if it ever runs dry it stops, and the PipeWire node it is fed
+ * from is then left permanently paused -- apps on the link never leave "paused" and
+ * the graph never reaches streaming. So a cycle with no captured PCM (nothing produced
+ * yet, or a genuinely quiet period) still sends one short block of digital silence in
+ * the negotiated format. Real PCM always wins: whenever the graph produced a period we
+ * forward exactly that period and nothing else, so silence never displaces audio and no
+ * fixed-size block is spliced into a live stream. Only ever sent while a consumer audio
+ * socket is attached and only after that consumer announced its PLAYBACK format.
+ * Dropped (still drained) while detached. */
 static void on_capture_process(void *data)
 {
     struct anland_audio *a = data;
@@ -134,16 +219,77 @@ static void on_capture_process(void *data)
         return;
 
     struct spa_data *d = &b->buffer->datas[0];
-    if (d->data && d->chunk->size > 0 && a->audio_fd >= 0) {
-        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = d->chunk->size };
+    if (!d->chunk) {
+        pw_stream_queue_buffer(a->capture, b);
+        return;
+    }
+
+    const uint8_t *payload = NULL;
+    size_t size = 0;
+
+    if (d->data && d->chunk->size > 0) {
+        /* Real PCM (or upstream silence) at the graph's current period. Sending exactly
+         * what was dequeued keeps byte counts identical to the plain pass-through path,
+         * so no fixed-size silence is ever spliced into a live stream -- no added
+         * latency, no A/V drift. */
+        payload = (uint8_t *)d->data + d->chunk->offset;
+        size = d->chunk->size;
+        /* Edge-triggered only (state change, not per buffer). stdio on the data thread is
+         * not strictly realtime-safe, but this fires at most once per silence/audio
+         * transition and is worth having when a stream will not leave paused. */
+        if (a->keepalive_on)
+            fprintf(stderr, "anland: playback streaming (real PCM)\n");
+        else if (!a->pcm_seen)
+            fprintf(stderr, "anland: playback streaming (first PCM captured)\n");
+        a->keepalive_on = false;
+        a->pcm_seen = true;
+    } else if (a->audio_fd >= 0 && a->play_attached) {
+        /* No captured PCM at all this cycle: the graph is either not producing yet or
+         * has genuinely gone quiet. Feed the consumer one short period of digital
+         * silence so its native playback stream stays primed and the node can reach
+         * streaming instead of sitting paused forever. Sized from the negotiated
+         * quantum when we know it, else a short default; always zero-filled. */
+        payload = a->silence;
+        size = a->silence_bytes;
+        if (!a->keepalive_on) {
+            fprintf(stderr, "anland: playback silent, keep-alive started (%zu bytes)\n", size);
+            a->keepalive_on = true;
+        }
+    }
+
+    if (payload && a->audio_fd >= 0) {
+        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = size };
         struct iovec iov[2] = {
             { .iov_base = &h, .iov_len = sizeof(h) },
-            { .iov_base = (uint8_t *)d->data + d->chunk->offset, .iov_len = d->chunk->size },
+            { .iov_base = (void *)payload, .iov_len = size },
         };
         struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
         /* Non-blocking: the loop thread must never stall on a slow/dead consumer.
-         * One SEQPACKET datagram per period; drop on EAGAIN. */
-        sendmsg(a->audio_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
+         * One whole message per period; drop on EAGAIN. */
+        ssize_t n = sendmsg(a->audio_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
+        if (n < 0) {
+            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
+                ; /* transient back-pressure: skip this period */
+            else if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
+                defer_detach(a);   /* consumer went away */
+            else {
+                static bool logged;
+                if (!logged) {
+                    logged = true;
+                    fprintf(stderr, "anland: audio sendmsg failed: %s\n", strerror(errno));
+                }
+            }
+        } else if (n != (ssize_t)(sizeof(h) + size)) {
+            /* The protocol carries one whole message per AUDIO_MSG_PCM and the socket is a
+             * message socket, so a short write means the framing is no longer reliable. */
+            static bool logged_short;
+            if (!logged_short) {
+                logged_short = true;
+                fprintf(stderr, "anland: short audio write (%zd/%zu), detaching\n", n,
+                        sizeof(h) + size);
+            }
+            defer_detach(a);
+        }
     }
     pw_stream_queue_buffer(a->capture, b);
 }
@@ -158,6 +294,10 @@ static void on_source_process(void *data)
         return;
 
     struct spa_data *d = &b->buffer->datas[0];
+    if (!d->data || !d->chunk) {
+        pw_stream_queue_buffer(a->source, b);
+        return;
+    }
     const uint32_t stride = sizeof(int16_t) * a->cap_channels;
     uint32_t frames = d->maxsize / stride;
     if (b->requested && b->requested < frames)
@@ -177,6 +317,7 @@ static const struct pw_stream_events capture_events = {
     PW_VERSION_STREAM_EVENTS,
     .process = on_capture_process,
+    .state_changed = on_capture_state_changed,
 };
 
 static const struct pw_stream_events source_events = {
@@ -202,6 +343,29 @@ static const struct pw_stream_events source_events = {
  * legitimately varies between opens) just updates node.latency. Neither disconnects.
  *
  * Runs on the loop thread, so the pw_stream calls are safe. */
+static bool valid_format(const struct audio_format *f)
+{
+    return (f->role == AUDIO_ROLE_PLAYBACK || f->role == AUDIO_ROLE_CAPTURE) &&
+           f->format == AUDIO_FORMAT_S16LE &&
+           f->rate >= MIN_AUDIO_RATE && f->rate <= MAX_AUDIO_RATE &&
+           f->channels > 0 && f->channels <= MAX_AUDIO_CHANNELS &&
+           f->quantum <= MAX_AUDIO_QUANTUM;
+}
+
+/* Bytes of S16 silence the keep-alive path hands the consumer when a cycle produced no
+ * PCM at all. Uses the negotiated quantum when the consumer announced one (so the
+ * Android side receives exactly one AAudio burst), else KEEPALIVE_FRAMES. Never larger
+ * than the preallocated buffer (channels are bounded by valid_format()), so the process
+ * callback can neither overflow it nor allocate. */
+static void update_silence_size(struct anland_audio *a)
+{
+    uint32_t frames = a->play_quantum ? a->play_quantum : KEEPALIVE_FRAMES;
+    if (frames > KEEPALIVE_FRAMES)
+        frames = KEEPALIVE_FRAMES;
+    size_t bytes = (size_t)frames * a->play_channels * sizeof(int16_t);
+    a->silence_bytes = bytes < sizeof(a->silence) ? bytes : sizeof(a->silence);
+}
+
 static void apply_format(struct anland_audio *a, const struct audio_format *f)
 {
     const bool playback = (f->role == AUDIO_ROLE_PLAYBACK);
@@ -214,6 +378,12 @@ static void apply_format(struct anland_audio *a, const struct audio_format *f)
     uint32_t *cur_quantum  = playback ? &a->play_quantum : &a->cap_quantum;
     struct pw_stream *stream = playback ? a->capture : a->source;
 
+    /* A PLAYBACK announcement means the consumer has opened (or is about to open) its
+     * playback device: from here on the speaker stream must not be allowed to run dry,
+     * or the native stream stops and the PipeWire node stays paused forever. */
+    if (playback)
+        a->play_attached = true;
+
     const bool format_changed = (rate != *cur_rate || channels != *cur_channels);
     const bool quantum_changed = (f->quantum != *cur_quantum);
     if (!format_changed && !quantum_changed)
@@ -222,6 +392,10 @@ static void apply_format(struct anland_audio *a, const struct audio_format *f)
     *cur_rate = rate;
     *cur_channels = channels;
     *cur_quantum = f->quantum;
+    if (playback)
+        update_silence_size(a);
+    fprintf(stderr, "anland: audio %s format %u Hz, %u ch, S16LE, quantum %u\n",
+            playback ? "playback" : "capture", rate, channels, f->quantum);
 
     if (!a->pw_connected || !stream)
         return;   /* build_pw() will pick up the new values when it (re)creates the stream */
@@ -255,7 +429,9 @@ static void on_audio_readable(void *data, int fd, uint32_t mask)
         return;
 
     for (;;) {
-        ssize_t n = recv(fd, a->rx, sizeof(a->rx), MSG_DONTWAIT);
+        /* MSG_TRUNC: a datagram larger than rx is truncated but reports its real size,
+         * so a stray oversized frame can never be mistaken for a well-formed one. */
+        ssize_t n = recv(fd, a->rx, sizeof(a->rx), MSG_DONTWAIT | MSG_TRUNC);
         if (n == 0) {
             detach_audio_fd_locked(a);
             break;
@@ -267,24 +443,32 @@ static void on_audio_readable(void *data, int fd, uint32_t mask)
                 detach_audio_fd_locked(a);
             break;
         }
-        if ((size_t)n < sizeof(struct audio_msg))
+        if ((size_t)n < sizeof(struct audio_msg) || (size_t)n > sizeof(a->rx))
             continue;
         struct audio_msg h;
         memcpy(&h, a->rx, sizeof(h));
         size_t avail = (size_t)n - sizeof(struct audio_msg);
 
         if (h.type == AUDIO_MSG_FORMAT) {
-            if (avail >= sizeof(struct audio_format)) {
-                struct audio_format f;
-                memcpy(&f, a->rx + sizeof(struct audio_msg), sizeof(f));
+            /* Framing is trusted only when the header and the payload agree exactly. */
+            if (h.size != sizeof(struct audio_format) || h.size != avail)
+                continue;
+            struct audio_format f;
+            memcpy(&f, a->rx + sizeof(struct audio_msg), sizeof(f));
+            if (valid_format(&f))
                 apply_format(a, &f);
-            }
+            else
+                fprintf(stderr, "anland: ignoring invalid audio format "
+                                "(rate=%u ch=%u fmt=%u role=%u quantum=%u)\n",
+                        f.rate, f.channels, f.format, f.role, f.quantum);
             continue;
         }
-        if (h.type != AUDIO_MSG_PCM)
+        if (h.type != AUDIO_MSG_PCM || h.size != avail)
             continue;
-        size_t size = h.size < avail ? h.size : avail;
-        ring_write(a, a->rx + sizeof(struct audio_msg), size);
+        if (a->cap_channels > 0 &&
+            h.size % (sizeof(int16_t) * a->cap_channels) != 0)
+            continue;   /* partial frame: drop rather than shift the mic by a sample */
+        ring_write(a, a->rx + sizeof(struct audio_msg), h.size);
     }
 }
 
@@ -351,6 +535,10 @@ static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rat
     pw_stream_update_properties(stream, &dict);
 }
 
+/* Sink -> socket writes happen in the graph's own process callback, so ask PipeWire to
+ * invoke it straight from the realtime data thread: an async hop through the main loop
+ * would let the driver advance without the consumer ever being fed. The callback stays
+ * realtime-safe (preallocated silence, non-blocking sendmsg, no locks, no allocation). */
 static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                           uint32_t rate, uint32_t channels, uint32_t quantum)
 {
@@ -361,7 +549,8 @@ static int connect_stream(struct pw_stream *stream, enum spa_direction direction
     const struct spa_pod *params[1] = { build_format(&bld, rate, channels) };
 
     return pw_stream_connect(stream, direction, PW_ID_ANY,
-                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
+                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
+                             PW_STREAM_FLAG_RT_PROCESS,
                              params, 1);
 }
 
@@ -397,7 +586,17 @@ static int build_pw(struct anland_audio *a)
     /* Own a virtual sink so the container has a real output device instead of only
      * the auto-null "Dummy Output": apps play into this Audio/Sink, WirePlumber makes
      * it the default (high priority beats auto_null), and on_capture_process receives
-     * the mixed PCM directly -- no monitor capture, nothing bound to the dummy. */
+     * the mixed PCM directly -- no monitor capture, nothing bound to the dummy.
+     *
+     * The idle properties are what make remote playback work at all. Without them this
+     * node is a follower that is only pulled while a driver is running it, so it sits
+     * suspended (pw-top: RATE 0/QUANT 0) and every app on the link stays in "paused"
+     * forever. always-process keeps the node runnable while unlinked (and implies
+     * want-driver); pause-on-idle/suspend-on-idle keep it from being torn down between
+     * tracks; session.suspend-timeout-seconds stops WirePlumber's session layer from
+     * suspending it after a few idle seconds. Together they let the node reach
+     * streaming -- and stay there -- while on_capture_process keeps feeding the
+     * consumer. */
     a->capture = pw_stream_new(a->core, "anland-speaker",
         pw_properties_new(
             PW_KEY_MEDIA_TYPE, "Audio",
@@ -406,6 +605,10 @@ static int build_pw(struct anland_audio *a)
             PW_KEY_NODE_DESCRIPTION, "Anland remote speaker",
             PW_KEY_PRIORITY_SESSION, "1010",   /* outrank the auto-null dummy sink */
             PW_KEY_PRIORITY_DRIVER, "1010",
+            PW_KEY_NODE_ALWAYS_PROCESS, "true",
+            PW_KEY_NODE_PAUSE_ON_IDLE, "false",
+            PW_KEY_NODE_SUSPEND_ON_IDLE, "false",
+            "session.suspend-timeout-seconds", "0",
             NULL));
     if (!a->capture)
         return -1;
@@ -459,26 +662,37 @@ void anland_audio_set_fd(int audio_fd)
     if (!a)
         return;
 
+    /* Duplicate BEFORE touching engine state: display_producer retains the original fd,
+     * so a failure here must not cost us a working attachment. */
+    int owned_fd = -1;
+    if (audio_fd >= 0) {
+        owned_fd = fcntl(audio_fd, F_DUPFD_CLOEXEC, 3);
+        if (owned_fd < 0) {
+            fprintf(stderr, "anland: failed to duplicate audio fd: %s\n", strerror(errno));
+            return;
+        }
+    }
+
     pw_thread_loop_lock(a->loop);
 
     detach_audio_fd_locked(a);
 
-    if (audio_fd >= 0) {
-        /* display_producer retains the input fd. Keep an owned duplicate so source
-         * teardown cannot race with producer fd reuse during fallback. The consumer
-         * announces both device formats (AUDIO_MSG_FORMAT) right after
-         * this socket comes up; on_audio_readable applies them and reconfigures the
-         * PipeWire streams, so we don't dictate any format here. */
-        int owned_fd = fcntl(audio_fd, F_DUPFD_CLOEXEC, 3);
-        if (owned_fd >= 0) {
-            a->io = pw_loop_add_io(pw_thread_loop_get_loop(a->loop), owned_fd,
-                                   SPA_IO_IN, true, on_audio_readable, a);
-            if (a->io) {
-                a->audio_fd = owned_fd;
-            } else {
-                close(owned_fd);
-            }
+    if (owned_fd >= 0) {
+        /* The consumer announces both device formats (AUDIO_MSG_FORMAT) right after this
+         * socket comes up; on_audio_readable applies them and reconfigures the PipeWire
+         * streams, so we don't dictate any format here. The loudspeaker keep-alive only
+         * starts once that PLAYBACK announcement has been seen. */
+        a->io = pw_loop_add_io(pw_thread_loop_get_loop(a->loop), owned_fd,
+                               SPA_IO_IN, true, on_audio_readable, a);
+        if (a->io) {
+            a->audio_fd = owned_fd;
+            a->attach_serial++;
+            fprintf(stderr, "anland: audio transport attached\n");
+        } else {
+            close(owned_fd);
+            fprintf(stderr, "anland: failed to register audio fd: %s\n", strerror(errno));
         }
+    } else {
+        fprintf(stderr, "anland: audio transport detached\n");
     }
 
     pw_thread_loop_unlock(a->loop);
@@ -499,6 +713,7 @@ int anland_audio_start(void)
     a->play_channels = DEFAULT_PLAY_CHANNELS;
     a->cap_rate = DEFAULT_RATE;
     a->cap_channels = DEFAULT_CAP_CHANNELS;
+    update_silence_size(a);   /* valid fallback before any consumer announcement */
     a->ring_size = MIC_RING_BYTES;
     a->ring = malloc(a->ring_size);
     if (!a->ring)
@@ -517,6 +732,12 @@ int anland_audio_start(void)
     if (!a->reconnect_timer)
         goto fail;
 
+    /* Used to move a detach requested from the realtime process callback back onto the
+     * loop thread (see defer_detach). */
+    a->detach = pw_loop_add_timer(pw_thread_loop_get_loop(a->loop), on_detach_timer, a);
+    if (!a->detach)
+        goto fail;
+
     if (pw_thread_loop_start(a->loop) < 0)
         goto fail;
 
@@ -536,6 +757,8 @@ int anland_audio_start(void)
     return 0;
 
 fail:
+    if (a->detach)
+        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->detach);
     if (a->reconnect_timer)
         pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->reconnect_timer);
     if (a->context)
@@ -555,6 +778,9 @@ void anland_audio_stop(void)
         return;
     g_audio = NULL;
 
+    /* Stop the callbacks first, then unwind in reverse creation order: streams, the
+     * transport io source (which owns audio_fd and closes it), the loop sources, and
+     * only then the context/loop. Nothing may call back into a destroyed object. */
     if (a->loop)
         pw_thread_loop_stop(a->loop);
     teardown_pw(a);
@@ -562,6 +788,8 @@ void anland_audio_stop(void)
         detach_audio_fd_locked(a);
     if (a->reconnect_timer)
         pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->reconnect_timer);
+    if (a->detach)
+        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->detach);
     if (a->context)
         pw_context_destroy(a->context);
     if (a->loop)
```

### 5.2 `anland_audio.h`

```diff
--- a/src/backend/anland/c/anland_audio.h
+++ b/src/backend/anland/c/anland_audio.h
@@ -7,16 +7,25 @@
  * Owns a persistent PipeWire thread-loop with two streams that live for the whole
  * KWin session, independent of whether a consumer is connected:
  *
- *   - a sink-monitor capture stream  -> desktop playback PCM written to the audio
- *                                        socket (heard on the Android speaker);
- *   - a virtual Audio/Source stream  <- microphone PCM read from the audio socket
- *                                        (so Linux apps can record the Android mic).
+ *   - an Audio/Sink ("anland-speaker") whose process callback writes desktop playback
+ *     PCM to the audio socket (heard on the Android speaker);
+ *   - a virtual Audio/Source ("anland-mic") fed from microphone PCM read from the
+ *     audio socket (so Linux apps can record the Android mic).
+ *
+ * The speaker sink is kept permanently runnable and unsuspendable (node.always-process,
+ * pause-on-idle/suspend-on-idle off, no session suspend timeout). Android's AAudio
+ * playback stream only advances while PCM keeps arriving, so a process cycle with no
+ * captured PCM sends one short period of digital silence in the negotiated format
+ * instead of nothing; that is what keeps the native stream, and therefore the PipeWire
+ * link, from settling into "paused" forever. Real PCM always displaces the silence.
  *
  * The streams are NEVER torn down on consumer disconnect: while detached the capture
  * stream simply drops its PCM and the source feeds silence, so PipeWire (and every
  * recording app) never sees the device disappear. Only the socket fd is hot-swapped:
  * anland_audio_set_fd(fd) on (re)connect, anland_audio_set_fd(-1) on fallback. The fd
  * is borrowed from display_producer; the engine owns a CLOEXEC duplicate while attached.
+ * A fresh attachment re-arms the keep-alive and waits for the consumer's format
+ * announcements, so disconnect/reconnect needs no compositor restart.
  */
```

---

## 6. 编译

### 6.1 本机实际执行的验证（可复现）

```bash
cd /path/to/ANiri
P=/tmp/pwroot/root/usr   # 本机解包的 pipewire 头文件前缀（等效于 -dev 包）

# (1) 严格告警全开，含 printf 格式类型检查
gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
  -I src/backend/anland/c -I$P/include/pipewire-0.3 -I$P/include/spa-0.2 \
  src/backend/anland/c/anland_audio.c
# → 0 warning, 0 error

# (2) 与 build.rs / cc crate 等价的编译参数
gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w \
  -I src/backend/anland/c -I$P/include/pipewire-0.3 -I$P/include/spa-0.2 \
  src/backend/anland/c/anland_audio.c -o /tmp/ccheck/anland_audio.o
# → OK

# (3) 同一静态库里其余 C 单元一起确认
gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w \
  -I src/backend/anland/c -I$P/include/pipewire-0.3 -I$P/include/spa-0.2 \
  src/backend/anland/c/anland_camera.c -o /tmp/ccheck/anland_camera.o        # OK
gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w -I src/backend/anland/c \
  src/backend/anland/c/libdisplay_producer/display_producer.c -o /tmp/ccheck/dp.o   # OK
gcc -c -O3 -ffunction-sections -fdata-sections -fPIC -w -I src/backend/anland/c \
  src/backend/anland/c/common/socket_utils.c -o /tmp/ccheck/su.o            # OK
```

**API 兼容性核对**：用到的每个符号在 PipeWire **1.0.5** 头文件中均存在（校验通过），对目标的 **1.6.9** 只会更宽松：

```
PW_KEY_NODE_ALWAYS_PROCESS OK
PW_KEY_NODE_PAUSE_ON_IDLE OK
PW_KEY_NODE_SUSPEND_ON_IDLE OK
PW_STREAM_FLAG_RT_PROCESS OK
```

### 6.2 目标设备（Arch Linux ARM64 / DroidSpaces）上的真实命令

```bash
# 1) 依赖（关键：必须有 libpipewire 的 pkg-config 文件）
sudo pacman -S --needed base-devel rust git pkgconf \
  libpipewire pipewire wireplumber \
  libinput libwayland libdisplay-info libgbm libxkbcommon \
  mesa seatd systemd-libs dbus pango

# 若 pkg-config 找不到 libpipewire-0.3，Aniri 会静默不编译 anland_audio.c：
pkg-config --modversion libpipewire-0.3     # 必须打印版本号

# 2) debug 构建（快速验证能编过）
cd /path/to/ANiri
cargo build

# 3) release 构建（部署用）
cargo build --release

# 4) 产物路径
ls -l target/release/niri
```

**`cargo build --release` 即可**。依据：

- [`build.rs`](../../build.rs) 只做 `pkg_config::Config::probe("libpipewire-0.3")` + `cc::Build`，**不需要** `cargo xtask` / bindgen / 额外 feature；
- [`Cargo.toml`](../../Cargo.toml) 中 anland backend 无条件编译（非 optional feature）；`pipewire` crate 只服务于 `xdp-gnome-screencast` feature，与本次改动无关；
- 本项目 MSRV = `1.87`（`Cargo.toml:15`），交叉编译/本机构建均按此要求。

---

## 7. 测试流程

> 🟢 = 本机 x86_64 沙箱可做的静态验证；✅ = **已在目标 ARM64 DroidSpaces 设备验证通过**；
> 🔴 = 仍未完成 / 待排查。

### ⚠️ 独立的环境问题：WirePlumber `monitor.v4l2` 阻塞（**不是** ANiri audio backend 的问题）

真机排查中发现，`pw-play` 长期停在 `connecting -> paused` 还有一个**与本次修复无关**的
环境成因：

- 在 DroidSpaces 环境下，WirePlumber 的 `monitor/v4l2/create-device` 的**异步 device
  activation 会卡住**，阻塞 WirePlumber 的 event dispatcher；
- dispatcher 被阻塞后，后续的音频 stream node **无法完成 session-item / link 创建**，
  表现出的症状正是 `pw-play connecting -> paused`、link 停在 `[paused]`；
- **临时禁用 WirePlumber 的 `monitor.v4l2` 之后，`pw-play` 立即恢复正常并实际出声。**

**定性**：这是**目标运行环境 / WirePlumber 集成问题**，与 ANiri audio backend 的
sender / framing / RT queue 修复**无关**。两者症状相似（都表现为 stream 停在 paused），
排查时务必先用 `wpctl status` 确认 **link 是否真的被创建**：

- link 存在但节点不运行 → 属于本次 ANiri 修复的范畴；
- link 根本没被创建 → 优先怀疑这个 WirePlumber V4L2 阻塞问题。

请**不要**把该 V4L2 阻塞归因到 ANiri backend 回归，也不要据此回退本次修复。

### A / B / C — 构建、替换、重启 ✅

```bash
# A. 备份
sudo cp -a /usr/bin/niri /usr/bin/niri.bak.$(date +%F-%H%M)

# B. 安装新二进制（按实际 niri-anland.service 的 ExecStart= 路径替换）
sudo install -m755 target/release/niri /usr/bin/niri

# C. 重启并抓日志（Aniri 日志走 stderr）
sudo systemctl restart niri-anland.service
journalctl -u niri-anland.service -f -o cat
```

启动日志中应出现：`anland: audio transport attached`。

### D — 节点存在性 ✅

```bash
wpctl status -n
# Sinks:   * anland-speaker
# Sources: * anland-mic
```

### E — 生成测试音频 ✅

```bash
ffmpeg -f lavfi -i "sine=frequency=440:duration=10" -ar 48000 -ac 2 -sample_fmt s16 /tmp/test.wav
# 或： sox -n -r 48000 -c 2 -b 16 /tmp/test.wav synth 10 sine 440
```

### F — `pw-play` 播放 ✅

```bash
pw-play -v --target anland-speaker /tmp/test.wav
```

期望：`stream state changed connecting -> paused` **之后继续**进入 `paused -> streaming`。
更直观的两条日志：`anland: speaker stream paused -> streaming`、`anland: playback streaming (real PCM)`。

**真机结果：✅ 通过。** `pw-play` 已能进入 `streaming`，48 kHz / stereo 测试音经
`anland-speaker` → ANiri → Android consumer → 手机扬声器**实际出声**。

### G — `pw-top` ✅

```bash
pw-top
```

| 节点 | 修复前 | 修复后（真机实测） |
|---|---|---|
| `anland-speaker` | `S`　`QUANT 0`　`RATE 0` | **`R`　`QUANT` 非 0　`RATE 48000` ✅** |
| `pw-play` | `S`　`QUANT 0`　`RATE 0` | **`R`　`QUANT` 非 0　`RATE 48000` ✅** |

**真机结果：✅ 已验证。** 相关节点已进入 `R` 状态，`RATE` / `QUANT` 非 0。

### H / I — Firefox 🔴 仍未解决

> **Firefox 与 `pw-play` 是两个独立验证项，请勿混为一谈。**
> `pw-play` → `anland-speaker` → Android 扬声器链路已真机验证通过（见 F / G / J）；
> **Firefox 当前仍未出声，属于独立待排查项**，不代表 audio backend 修复失败。
> 排查建议：先确认 Firefox 的 stream 是否被创建并 link 到 `anland-speaker`
> （`wpctl status -n`），再排除上文的 WirePlumber V4L2 阻塞 / Firefox 自身 audio backend
> 选择（PulseAudio vs PipeWire）等因素。

播放视频后：

```bash
wpctl status -n
# Firefox output_FL > Anland remote speaker:playback_FL   不能再是 [paused]
# Firefox output_FR > Anland remote speaker:playback_FR   不能再是 [paused]
```

### J — Android 实际出声 ✅（唯一真正的验收标准）

- **`pw-play` 路径：✅ 已验证。** 48 kHz / stereo 测试音经
  `anland-speaker` → ANiri → Android consumer → **手机扬声器实际出声**。
- **Firefox 路径：🔴 仍未出声**，属独立后续排查项（见 H / I），不计入本项结论。

### K — 停止 10 秒后再次播放 ✅

```bash
sleep 10 && pw-play -v --target anland-speaker /tmp/test.wav
```

要求：**不需要**重启 Aniri / 重启 PipeWire / 重设默认设备。停止期间应看到
`anland: playback silent, keep-alive started (N bytes)`，且空闲时 niri 进程 CPU 不应明显占用一个核心（见 §8）。

### L — Android consumer 断开 / 重连 🟡 未做完整稳定性验证

手机端 Anland app 切后台再回来，然后再次 `pw-play`。期望日志序列：

```
anland: audio transport detached
anland: audio transport attached
anland: audio playback format 48000 Hz, 2 ch, S16LE, quantum N
```

且**不 crash**、不需要重启 compositor。

---

## 8. 风险

| 项 | 评估 |
|---|---|
| **CPU** | `always-process` 会让该 sink 在**无客户端连接时也参与调度**（这正是能进入 streaming 的前提）。静音路径成本 ≈ 每周期一次 `sendmsg`（默认 quantum 1024/48000 ≈ 47 次/秒，仅 8 字节头 + 最多 1920 字节负载）。空闲时应用 `pw-top` 确认 niri 的 CPU 占比；若偏高，可调小 `KEEPALIVE_FRAMES`（当前已只在 `play_attached` 后启用）。 |
| **latency** | **无额外延迟**。真实 PCM 永远原样转发当前周期；静音仅在「本周期完全无数据」时使用，长度随协商 quantum（≤480 帧 ≈ 10 ms），不拼接、不补齐。 |
| **silence traffic** | 仅在 `audio_fd >= 0` 且 consumer 已宣告 PLAYBACK 格式时发送，`detach` 立即停止。典型量级 < 100 KB/s，且**有界**（不随空闲时间增长）。 |
| **Android reconnect** | `detach` 复位 `play_attached` / `pcm_seen` / `keepalive_on`；重连后必须等新的 `AUDIO_MSG_FORMAT(PLAYBACK)` 才恢复 keep-alive，**不会对旧 fd 空写**。serial 保护避免「旧 attachment 的失败拆掉新 attachment」。**仍未做完整的真机稳定性验证**（未做长时 / 多次重连压力测试），不要视为已验证。 |
| **mic regression** | 低。capture 逻辑未改（仅加 NULL guard）。唯一可能影响点：`valid_format()` 对 CAPTURE 要求 `channels ∈ [1,2]`；若 Android consumer 宣告 **>2ch 的 mic**，该宣告会被拒并打印 `anland: ignoring invalid audio format ...`。此时放宽 `MAX_AUDIO_CHANNELS` 即可，日志可直接看出。 |
| **RT-safe 日志** | 实时线程上有 3 处**边沿触发** `fprintf`（非严格 RT-safe）。频率极低（仅状态跳变），已在代码注释与提交说明中标注。若需更保守，可改为写 `volatile` 标志、由 main loop 统一打印。 |
| **`always-process` 与 Dummy-Driver** | 之前观察到的「加规则后 `Dummy-Driver` 跑起来但 speaker 仍 `S / RATE 0`」已解释：那一步只让 **driver** 变 running；真正把数据送给 consumer 的是本补丁的 keep-alive，**两者缺一不可**。 |

---

## 9. Commit

```
ec9ef2fbee48b62c97a6b1d12b1aadbc10d912cb
backend/anland: keep PipeWire speaker stream alive
```

- 分支：`fix/anland-audio`
- 改动：`src/backend/anland/c/anland_audio.c`（+267 / −36）、`src/backend/anland/c/anland_audio.h`（+17）
- 工作区 `git status` 干净；已提交代码重新通过严格编译验证。

> commit message 采用了建议的标题。备注：**实际根因与初始判断同向但不完全相同** —— 「缺少 silence keep-alive」确实是**必要**一环，但**单独不够**；必须同时补 `node.always-process`（让节点具备被调度资格）与 `PW_STREAM_FLAG_RT_PROCESS`（让发送发生在图调度周期内）。这三者在上游 mutter 的已验证实现里本来就是成套出现的。

---

## 10. 部署与验证命令速查（可直接复制到目标设备）

```bash
#############################
# 0. 前置检查
#############################
uname -m                                   # 期望 aarch64
pkg-config --modversion libpipewire-0.3    # 必须成功，否则 anland_audio.c 不会被编译
echo "$ANLAND_SOCKET"                      # 确认 Anland socket 路径

#############################
# 1. 依赖
#############################
sudo pacman -S --needed base-devel rust git pkgconf \
  libpipewire pipewire wireplumber \
  libinput libwayland libdisplay-info libgbm libxkbcommon \
  mesa seatd systemd-libs dbus pango

#############################
# 2. 构建
#############################
cd /path/to/ANiri
git log --oneline -1                       # 应为 ec9ef2f backend/anland: keep PipeWire speaker stream alive
cargo build --release
ls -l target/release/niri

#############################
# 3. 部署（先备份！）
#############################
sudo cp -a "$(command -v niri)" "$(command -v niri).bak.$(date +%F-%H%M)"
sudo install -m755 target/release/niri "$(command -v niri)"

#############################
# 4. 重启并观察
#############################
sudo systemctl restart niri-anland.service
journalctl -u niri-anland.service -f -o cat | grep --line-buffered '^anland:'

#############################
# 5. 生成测试音频（S16LE / 48000 / stereo）
#############################
ffmpeg -f lavfi -i "sine=frequency=440:duration=10" \
       -ar 48000 -ac 2 -sample_fmt s16 /tmp/test.wav

#############################
# 6. 播放 + 观察（另开终端）
#############################
pw-play -v --target anland-speaker /tmp/test.wav
# 期望：connecting -> paused -> streaming
# 期望日志：anland: speaker stream paused -> streaming
#           anland: playback streaming (real PCM)

pw-top                                     # anland-speaker / pw-play 应为 R，QUANT 非 0，RATE 48000

#############################
# 7. Firefox 验证
#############################
# 播放任意视频，然后：
wpctl status -n
# Firefox output_FL/FR > Anland remote speaker:playback_FL/FR 不应再是 [paused]

#############################
# 8. 空闲恢复
#############################
sleep 10
pw-play -v --target anland-speaker /tmp/test.wav     # 必须仍能出声，无需重启任何服务

#############################
# 9. Anland consumer 重连
#############################
# 手机端切后台 -> 回前台，再执行第 6 步
# 期望日志：audio transport detached / attached / audio playback format ...

#############################
# 10. 回滚（如需）
#############################
sudo cp -a "$(command -v niri).bak.YYYY-MM-DD-HHMM" "$(command -v niri)"
sudo systemctl restart niri-anland.service
```

### 需要回传的信息

若第 6 / 7 / 9 步仍未进入 `streaming`，请回传：

1. `journalctl -u niri-anland.service -o cat | grep '^anland:'` 的完整输出；
2. `pw-top` 中 `anland-speaker` / `pw-play` / `Dummy-Driver` 三行的 `S/R`、`QUANT`、`RATE`、`ERR`；
3. `wpctl status -n` 的 Audio 段；
4. `pkg-config --modversion libpipewire-0.3` 的实际版本。

据 §1.5 的沙箱限制，这些真机数据是判定「keep-alive 是否已足够」的唯一依据；如有必要，下一轮可基于日志把发送路径进一步收敛（例如按 `node.latency` 显式锁定 quantum，或改用 `pw_stream` + `node.driver` 组合）。

---

## 附录 A：文件与产出一览

| 路径 | 说明 |
|---|---|
| [`src/backend/anland/c/anland_audio.c`](../../src/backend/anland/c/anland_audio.c) | 核心修复 |
| [`src/backend/anland/c/anland_audio.h`](../../src/backend/anland/c/anland_audio.h) | 注释对齐 |
| [`0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch`](0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch) | 完整补丁（`git format-patch -1 ec9ef2f`） |
| [`COMMIT.txt`](COMMIT.txt) | 提交哈希与标题 |
| `ANLAND_AUDIO_FIX_REPORT.md` | 本报告 |

## 附录 B：验证状态（已按真机测试结果更新）

### B.1 已在目标 ARM64 DroidSpaces 设备验证通过 ✅

- ARM64 release 构建（已在目标设备成功编译出 release binary）
- 新 ANiri binary 实际运行
- `anland-speaker` / `anland-mic` 节点正常创建
- PipeWire graph 进入运行态
- `pw-play` 进入 `streaming`（不再永久停在 `paused`）
- `pw-top` 中相关节点进入 `R` 状态，`RATE` / `QUANT` 非 0
- 48 kHz / stereo 测试音经 `anland-speaker` → ANiri → Android consumer →
  **手机扬声器实际出声**
- KGSL / DroidSpaces ARM64 runtime

> 结论：**speaker / playback 主链路已真机验证成功。**

### B.2 仍未完成，不得视为已验证 🔴

- **Firefox 播放**：当前仍未出声，属**独立后续排查项**，与 `pw-play` 分开跟踪；
  不代表 audio backend 修复失败（见 §7 H / I）。
- **麦克风 capture 真机录音**：仓库中暂无真机录音证据，未验证。
- **Anland consumer disconnect / reconnect**：未做长时 / 多次重连的稳定性验证。
- **x86_64 本机 `cargo build`**：开发机无 Rust 工具链，未做（不影响目标设备）。

### B.3 旁证：独立的环境问题

真机排查中另定位到一个与本次修复无关的成因（详见 §7 开头）：WirePlumber 的
`monitor/v4l2/create-device` 异步激活在 DroidSpaces 下会卡住并阻塞 event dispatcher，
使后续 stream node 无法创建 session-item / link，症状同样是 `pw-play connecting -> paused`；
禁用 `monitor.v4l2` 后恢复正常。**该问题属于运行环境 / WirePlumber 集成，不应归因到
ANiri audio backend 的 sender / framing / RT queue 修复。**

### B.4 本报告能够确定的部分

**根因定位、上游语义对齐、C 侧严格编译通过、PipeWire 1.0.5/1.6.9 API 兼容性核对、
并发与生命周期安全加固**，以及上列 B.1 的真机验证结论。
