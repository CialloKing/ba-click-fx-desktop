# ADR-007: 背景样本的时间有效性

- Status: Proposed
- Decision owner: Rendering/Capture

## Context

捕获与呈现具有独立 cadence。直接使用最新纹理会在 WGC 卡顿、显示器变更或 session 重启后，把旧桌面
内容与新点击混合。纹理 generation 新不代表它在时间上仍适合当前 present。

## Proposed decision

- 使用 WGC `SystemRelativeTime` 映射到 QPC，并保留 generation。
- Render Owner 按目标显示器刷新周期计算 age，而不是按固定毫秒阈值。
- 默认权重：`age <= 1T` 为 1；`1T < age < 3T` 线性衰减；`age >= 3T` 为 0。
- 时间戳无效、倒退、来自旧 session generation 或尺寸/encoding 不匹配时立即为 0。
- 权重只乘 Differential Bloom；DirectEmission 与 FX-only Bloom 始终继续。

## Acceptance

- 纯函数测试覆盖边界、不同刷新率、QPC 溢出防护、时间倒退和 session generation 切换。
- 集成测试人工冻结 sensor，确认差分 Bloom 平滑退出且点击本体不中断。
- 诊断同时记录 sample age、refresh period、generation 和最终权重。

