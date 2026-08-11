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
  Background-aware 路径；已进入该路径的批次只接受 `-3T <= age < max(12T, 250ms)` 的 live
  generation，已有快照本身不因 age 超界失效。
- 新批次只有在样本通过 acquire freshness 时才可选择 Background-aware；进入该路径后，每个通过
  freshness/epoch/尺寸校验的新 generation 都刷新独立背景快照，短暂丢帧时沿用上一张，不切换到
  FX-only。只有在快照尚未成功复制时才允许按 retain 窗口降级；FX-only 在同一可见批次内不因新
  样本恢复而升级。画面完全透明后解除锁存，下一批重新选择。
- 暂停开始时若尚未复制快照，永久保留帧只接受 `-T <= age < T`；已经复制的快照继续保持原
  Background-aware 路径，并由 WGC 帧事件驱动同一静止特效重新合成，使桌面变化不会留下旧背景。
- 来自旧 epoch、尺寸/encoding 不匹配或未确认排除自身 overlay 的样本立即不可用。
- 可用样本同时启用完整 Differential Bloom 和 background-aware source-over payload；不可用样本同时
  回退 FX-only。sample age 不参与 Bloom、Alpha 或其他视觉能量计算，避免正常 WGC cadence 在浅色
  背景上形成周期性亮度脉冲。Render Owner 把可用帧复制到独立背景快照，使每次 present 的
  Differential Bloom 与最终 payload 读取同一个 generation；WGC 接收边界再以两张
  `R16G16B16A16_FLOAT` 的 RTV/SRV 纹理执行一次 GPU 时域稳定化：首个 generation 直接 seed，
  后续 generation 只在 ping-pong 目标中更新。相邻 FP16 采样的死区随背景亮度从 `0.5/1024`
  到 `2/1024` 连续变化，并取通道峰值的 `2/1024` 作为 HDR 相对下限；超过死区后以平滑响应
  逐步跟随，较大的真实桌面变化仍能通过。稳定结果同时供 Differential Bloom 和最终 payload 使用。
  批次结束、尺寸变化或 session 失效时
  丢弃快照。最终 pass 不做会在浅色渐变上产生阶跃的全域量化；只在 scRGB `1.0` 周围的
  `1/1024` 捕获噪声平台内收敛到参考白，再于 `3/1024` 内连续退出，并且不根据目标/背景差异
  反解或抬高 Alpha。Alpha 始终来自 authored Coverage/Bloom 传输容量，
  DirectEmission/Bloom RGB 保持扩展预乘加法能量，避免拖尾衰减或近白分母把不可见差异放大成
  闪烁。该近似只属于已知背景的传输层，不改变 FX-only 或 Unity source-over 的源颜色。

## Acceptance

- 纯函数测试覆盖开闭边界、路径锁存、不同刷新率、QPC 极值饱和、时间倒退和 session generation
  切换。
- WARP 连续帧测试在浅色/深色背景上覆盖点击与拖尾，确认获取边界、保留边界、样本恢复和新批次
  的逐像素稳定性，并覆盖近白到纯白的连续 FP16 采样步进；交替相邻浅色样本必须保持圆盘与
  拖尾最终帧稳定，同时较大的真实背景变化必须收敛而不能冻结首帧。
- 集成测试人工冻结 sensor，确认已复制快照的背景路径跨获取/保留边界保持稳定；新 generation
  刷新背景内容但不改变路径，尚未复制快照的新批次仍在有界保留窗口外回退一次。
- 诊断同时记录 sample age、refresh period、generation 和最终二值状态。
