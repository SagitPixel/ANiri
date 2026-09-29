# anland-audio 修复资料

本目录是「ANiri Anland 扬声器永久 paused」修复的配套档案。

| 文件 | 说明 |
|---|---|
| `ANLAND_AUDIO_FIX_REPORT.md` | 完整修复报告（根因 / 参考实现 / 修改 / diff / 编译 / 测试 / 风险 / commit / 部署验证命令）。**描述 `ec9ef2f`，其中 RT 安全性的部分已被下一条修订** |
| `RT_SAFETY_REVISION.md` | `89e6ceb` 的并发/RT 安全性修订说明（**部分内容已被下一条取代**） |
| `RT_SOCKET_OFFLOAD_REVISION.md` | `4d17eff` 的修订说明：socket 移出 RT callback、sender thread（**其队列 framing/所有权已被下一条修正**） |
| `QUEUE_FRAMING_REVISION.md` | `cab5b98` 的修订说明：wire framing 修正、`SEND_*` 状态机、两个独立 fd 副本、`playback_ready` 门禁与 epoch 丢弃（**其 `head` 复位与对齐问题已被下一条修正**） |
| `QUEUE_OWNERSHIP_REVISION.md` | `bcc887e` 的修订说明：`head` 单写者契约、未对齐 struct 指针、`pipe2()`、RT 路径 fd 表述纠正、`SEND_DISCARDED` |
| `0001-*.patch` | `ec9ef2f` 补丁 |
| `0002-*.patch` | `89e6ceb` 补丁 |
| `0003-*.patch` | `4d17eff` 补丁 |
| `0004-*.patch` | `cab5b98` 补丁 |
| `0005-*.patch` | `bcc887e` 补丁 |
| `COMMIT.txt` | 对应的提交哈希与标题 |

## 快速定位

- 代码改动提交：`ec9ef2f` — `backend/anland: keep PipeWire speaker stream alive`
- RT 安全性修订 1：`89e6ceb` — `backend/anland: make the RT process callback actually realtime-safe`
- RT 安全性修订 2：`4d17eff` — `backend/anland: take the audio socket out of the realtime callback`
- 队列/framing 修订 3：`cab5b98` — `backend/anland: fix playback queue framing, ownership and gating`
- 队列所有权修订 4：`bcc887e` — `backend/anland: keep the send queue head producer-owned and tidy the sender`
- **当前 HEAD 的线程模型/fd 生命周期说明以 `RT_SOCKET_OFFLOAD_REVISION.md` + `QUEUE_FRAMING_REVISION.md` + `QUEUE_OWNERSHIP_REVISION.md` 为准**
- 直接看 diff：`git show ec9ef2f`
- 应用补丁到其它 checkout：`git am < 0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch`

## 重要提示

修复报告中的 Android 真机验证项（实际出声、`pw-top` 进入 `R`、consumer
disconnect/reconnect）**尚未在目标 ARM64 DroidSpaces 设备上验证**，报告 §0
与附录 B 已逐条标注。部署验证命令见报告 §10。
