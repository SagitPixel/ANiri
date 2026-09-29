# anland-audio 修复资料

本目录是「ANiri Anland 扬声器永久 paused」修复的配套档案。

| 文件 | 说明 |
|---|---|
| `ANLAND_AUDIO_FIX_REPORT.md` | 完整修复报告（根因 / 参考实现 / 修改 / diff / 编译 / 测试 / 风险 / commit / 部署验证命令）。**描述 `ec9ef2f`，其中 RT 安全性的部分已被下一条修订** |
| `RT_SAFETY_REVISION.md` | `89e6ceb` 的并发/RT 安全性修订说明：逐 stream 的线程模型、RT callback 内每个调用的安全性论证、`sendmsg` 的准确表述、keep-alive 门禁漏洞修复 |
| `0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch` | 代码修复补丁（`git format-patch -1 ec9ef2f`），可用 `git am` 直接应用 |
| `COMMIT.txt` | 对应的提交哈希与标题 |

## 快速定位

- 代码改动提交：`ec9ef2f` — `backend/anland: keep PipeWire speaker stream alive`
- RT 安全性修订：`89e6ceb` — `backend/anland: make the RT process callback actually realtime-safe`
- 当前 HEAD 的 RT/线程模型说明以 `RT_SAFETY_REVISION.md` 为准
- 直接看 diff：`git show ec9ef2f`
- 应用补丁到其它 checkout：`git am < 0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch`

## 重要提示

修复报告中的 Android 真机验证项（实际出声、`pw-top` 进入 `R`、consumer
disconnect/reconnect）**尚未在目标 ARM64 DroidSpaces 设备上验证**，报告 §0
与附录 B 已逐条标注。部署验证命令见报告 §10。
