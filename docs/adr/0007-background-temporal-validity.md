# ADR-007: 背景样本的时间有效性

- Status: Proposed
- Decision owner: Rendering/Capture

## Context

捕获与呈现具有独立 cadence。直接使用最新纹理会在 WGC 卡顿、显示器变更或 session 重启后，把旧桌面
内容与新点击混合。纹理 generation 新不代表它在时间上仍适合当前 present。

## Proposed decision

- 使用 WGC `SystemRelativeTime` 映射到 QPC，并保留 generation。
- Render Owner 按背景采样 cadence 计算 age；高刷新率显示器至少采用 60 Hz 的周期下限，避免把
  WGC 的正常调度抖动误判为过期。
- age 使用饱和时间差计算，极端 QPC 值分别归类为 Stale 或 FutureTimestamp，不执行可能溢出的
  有符号 duration 减法。
- 背景可用性为二值。普通动画仅在 `-3T <= age < max(6T, 100ms)` 时为新一批可见特效获取
  Background-aware 路径；已经获取的批次可在 `-3T <= age < max(12T, 250ms)` 内继续保留。
- Background-aware 超过保留窗口后只向 FX-only 降级一次；FX-only 在同一可见批次内不因新样本
  恢复而升级。画面完全透明后解除锁存，下一批重新选择。
- 暂停后的所有永久保留帧只接受 `-T <= age < T`，并保持相同的单向降级规则。
- 来自旧 epoch、尺寸/encoding 不匹配或未确认排除自身 overlay 的样本立即不可用。
- 可用样本同时启用完整 Differential Bloom 和 background-aware source-over 反解；不可用样本同时
  回退 FX-only。sample age 不参与 Bloom、Alpha 或其他视觉能量计算，避免正常 WGC cadence 在浅色
  背景上形成周期性亮度脉冲。锁存只保存路径枚举，不跨帧缓存 sensor 所有的裸 SRV。

## Acceptance

- 纯函数测试覆盖开闭边界、路径锁存、不同刷新率、QPC 极值饱和、时间倒退和 session generation
  切换。
- WARP 连续帧测试在浅色/深色背景上覆盖点击与拖尾，确认获取边界、保留边界、样本恢复和新批次
  的逐像素稳定性。
- 集成测试人工冻结 sensor，确认背景路径跨获取边界保持稳定，超过有界保留窗口后只回退一次。
- 诊断同时记录 sample age、refresh period、generation 和最终二值状态。
