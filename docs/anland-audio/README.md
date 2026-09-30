# anland-audio 修复资料

本目录是「ANiri Anland 扬声器永久 paused」修复的配套档案。

| 文件 | 说明 |
|---|---|
| `ANLAND_AUDIO_FIX_REPORT.md` | 完整修复报告（根因 / 参考实现 / 修改 / diff / 编译 / 测试 / 风险 / commit / 部署验证命令）。**描述 `ec9ef2f`，其中 RT 安全性的部分已被下一条修订** |
| `RT_SAFETY_REVISION.md` | `89e6ceb` 的并发/RT 安全性修订说明（**部分内容已被下一条取代**） |
| `RT_SOCKET_OFFLOAD_REVISION.md` | `4d17eff` 的修订说明：socket 移出 RT callback、sender thread（**其队列 framing/所有权已被下一条修正**） |
| `QUEUE_FRAMING_REVISION.md` | `cab5b98` 的修订说明：wire framing 修正、`SEND_*` 状态机、两个独立 fd 副本、`playback_ready` 门禁与 epoch 丢弃（**其 `head` 复位与对齐问题已被下一条修正**） |
| `QUEUE_OWNERSHIP_REVISION.md` | `bcc887e` 的修订说明：`head` 单写者契约、未对齐 struct 指针、`pipe2()`、RT 路径 fd 表述纠正、`SEND_DISCARDED`（**其 wire 跨度计算已被下一条修正**） |
| `WIRE_SPAN_REVISION.md` | `f337d2d` 的修订说明：wire 跨度改用 `offsetof`（修复 `AUDIO_MSG_PCM` 头被跳过的协议 blocker）、删除 `SEND_DEAD` 的无效 wake、**format-change 窗口风险标记** |
| `SENDER_FD_EXIT_REVISION.md` | `f9a4af0` 的修订说明：`SEND_DEAD` 提前 return 导致 `sender_fd` 泄漏、统一退出路径、`close` 调用点互斥审计、删除死代码 `send_ring_used`、**「真实 0 warning」的更正** |
| `0001-*.patch` | `ec9ef2f` 补丁 |
| `0002-*.patch` | `89e6ceb` 补丁 |
| `0003-*.patch` | `4d17eff` 补丁 |
| `0004-*.patch` | `cab5b98` 补丁 |
| `0005-*.patch` | `bcc887e` 补丁 |
| `0006-*.patch` | `f337d2d` 补丁 |
| `0007-*.patch` | `f9a4af0` 补丁 |
| `COMMIT.txt` | 对应的提交哈希与标题 |

## 快速定位

- 代码改动提交：`ec9ef2f` — `backend/anland: keep PipeWire speaker stream alive`
- RT 安全性修订 1：`89e6ceb` — `backend/anland: make the RT process callback actually realtime-safe`
- RT 安全性修订 2：`4d17eff` — `backend/anland: take the audio socket out of the realtime callback`
- 队列/framing 修订 3：`cab5b98` — `backend/anland: fix playback queue framing, ownership and gating`
- 队列所有权修订 4：`bcc887e` — `backend/anland: keep the send queue head producer-owned and tidy the sender`
- wire 跨度修订 5：`f337d2d` — `backend/anland: bound the wire span by the local header, not the whole slot`
- sender fd 修订 6：`f9a4af0` — `backend/anland: close sender_fd on every sender-thread exit path`
- **当前 HEAD 的线程模型/fd 生命周期说明以 `RT_SOCKET_OFFLOAD_REVISION.md` + `QUEUE_FRAMING_REVISION.md` + `QUEUE_OWNERSHIP_REVISION.md` + `WIRE_SPAN_REVISION.md` + `SENDER_FD_EXIT_REVISION.md` 为准**
- **已知未修风险汇总见 `WIRE_SPAN_REVISION.md` §3**（format-change 窗口等）
- 直接看 diff：`git show ec9ef2f`
- 应用补丁到其它 checkout：`git am < 0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch`

## 真机验证状态（目标 ARM64 DroidSpaces 设备）

### ✅ 已验证通过

| 项 | 状态 |
|---|---|
| ARM64 release 构建 | ✅ 已在目标设备成功编译 |
| 新 ANiri binary 实际运行 | ✅ |
| `anland-speaker` / `anland-mic` 节点创建 | ✅ |
| PipeWire graph 进入运行态 | ✅ |
| **`pw-play` 真机播放** | ✅ **进入 `streaming`，不再永久停在 `paused`** |
| **`pw-top` `R` / `RATE` / `QUANT` 非 0** | ✅ |
| **Android 扬声器实际出声**（48 kHz / stereo） | ✅ |
| KGSL / DroidSpaces ARM64 runtime | ✅ |

> **结论：ANiri speaker / `pw-play` / ARM64 DroidSpaces playback 已真机验证成功。**

### 🔴 仍未完成（不要视为已验证）

| 项 | 状态 |
|---|---|
| **Firefox 播放** | 🔴 **仍未出声，独立待排查**（与 `pw-play` 分开跟踪，不代表 audio backend 修复失败） |
| **麦克风 capture 真机录音** | 🔴 未验证（仓库中暂无真机录音证据） |
| Anland consumer disconnect / reconnect | 🟡 未做长时 / 多次重连的稳定性验证 |
| x86_64 本机 `cargo build` | ⚪ 开发机无 Rust 工具链，未做（不影响目标设备） |

### ⚠️ 独立的环境问题：WirePlumber `monitor.v4l2` 阻塞（**不是** ANiri backend 的问题）

真机排查中定位到 `pw-play` 长期 `connecting -> paused` 的另一个**独立**成因：

- DroidSpaces 环境下，WirePlumber 的 `monitor/v4l2/create-device` 的**异步 device
  activation 可能卡住**，阻塞 WirePlumber 的 event dispatcher；
- dispatcher 被阻塞后，后续音频 stream node **无法完成 session-item / link 创建**，
  症状同样是 `pw-play connecting -> paused`、link 停在 `[paused]`；
- **临时禁用 `monitor.v4l2` 后，`pw-play` 立即恢复正常并实际出声。**

**定性**：属于**目标运行环境 / WirePlumber 集成问题**，与 ANiri audio backend 的
sender / framing / RT queue 修复**无关**。排查时请先用 `wpctl status` 区分：

- **link 已创建但节点不运行** → 属于本次 ANiri 修复范畴；
- **link 根本没被创建** → 优先怀疑该 WirePlumber V4L2 阻塞。

请勿据此回退本次修复，也不要把 Firefox 未出声描述成 audio backend 修复失败。

---

## 重要提示

各 revision 文档末尾若写有「未完成的验证」，那是**该次修订当时**记录的快照；
**当前真机验证状态以本节的表格为准**。报告 §0 / §7 / 附录 B 已按真机结果更新；
部署验证命令仍见报告 §10。
