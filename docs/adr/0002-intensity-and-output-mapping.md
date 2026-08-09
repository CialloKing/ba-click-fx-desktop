# ADR-002: Authoring 单位与 HDR/SDR 输出映射

- Status: Proposed
- Decision owner: Color/Rendering

## Context

Unity emission 数值是艺术/参考白相对值，不能自动解释为 nits。scene-referred HDR、Advanced Color SDR
和普通 SDR 对 FP16 scRGB 的显示处理不同；普通 SDR 的超范围值可能被 DWM 裁剪，因此白色背景没有
可供正 emission 使用的余量。

## Proposed decision

公共参数必须标记下列语义之一：`ArtisticRelative`、`ReferenceWhiteRelative`、`AbsoluteNits`。
转换管线固定分为 authoring、calibration、reference-linear、output policy、monitor encoding 五段。

- `ArtisticRelative`：使用效果专属校准曲线，不显示 nits 标签。
- `ReferenceWhiteRelative`：以当前显示器 SDR white level 为参考。
- `AbsoluteNits`：只允许显式物理意图的参数；HDR scene-referred 编码使用 `nits / 80`。
- 普通 SDR 使用独立 display-referred rolloff，并公开白底饱和的不可避免降级。

Bloom threshold 必须与 seed 使用相同语义；不得让绝对 nits threshold 与艺术相对 emission 混用。

## Acceptance

- Spike C 覆盖 SDR、HDR active、Advanced Color SDR、不同 SDR white level、负 FP16 和大于 1 的值。
- 单元测试证明三种语义不会静默互换，且 NaN/Inf/负 AbsoluteNits 有确定处理。
- UI/配置序列化保存语义标签，而不只保存裸浮点值。

