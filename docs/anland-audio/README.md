# anland-audio 修复资料

本目录是「ANiri Anland 扬声器永久 paused」修复的配套档案。

| 文件 | 说明 |
|---|---|
| `ANLAND_AUDIO_FIX_REPORT.md` | 完整修复报告（根因 / 参考实现 / 修改 / diff / 编译 / 测试 / 风险 / commit / 部署验证命令） |
| `0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch` | 代码修复补丁（`git format-patch -1 ec9ef2f`），可用 `git am` 直接应用 |
| `COMMIT.txt` | 对应的提交哈希与标题 |

## 快速定位

- 代码改动提交：`ec9ef2f` — `backend/anland: keep PipeWire speaker stream alive`
- 直接看 diff：`git show ec9ef2f`
- 应用补丁到其它 checkout：`git am < 0001-backend-anland-keep-PipeWire-speaker-stream-alive.patch`

## 重要提示

修复报告中的 Android 真机验证项（实际出声、`pw-top` 进入 `R`、consumer
disconnect/reconnect）**尚未在目标 ARM64 DroidSpaces 设备上验证**，报告 §0
与附录 B 已逐条标注。部署验证命令见报告 §10。
