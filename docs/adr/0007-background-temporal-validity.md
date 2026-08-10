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
- 默认权重：`age <= 1T` 为 1；`1T < age < 3T` 线性衰减；`age >= 3T` 为 0。
- 时间戳无效、倒退、来自旧 session generation 或尺寸/encoding 不匹配时立即为 0。
- 权重只乘 Differential Bloom；DirectEmission 与 FX-only Bloom 始终继续。
- 最终 source-over 传输与 Differential Bloom 权重独立。合同有效的纹理可在
  `max(12T, 250ms)` 内继续使用背景反解，未来时间戳容差上限为 `3T`；超过窗口后才回退
  FX-only。这样短暂缺帧不会逐帧切换两套 Alpha solver，长期停滞仍会有界退出。

## Acceptance

- 纯函数测试覆盖边界、不同刷新率、QPC 溢出防护、时间倒退和 session generation 切换。
- 集成测试人工冻结 sensor，确认差分 Bloom 平滑退出且点击本体不中断。
- 连续帧测试确认 Differential Bloom 已衰减时仍保持同一最终传输合同，并在有界窗口结束后退出。
- 诊断同时记录 sample age、refresh period、generation 和最终权重。
