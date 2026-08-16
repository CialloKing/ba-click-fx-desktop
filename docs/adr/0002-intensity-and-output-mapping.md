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
Unity authored color、粒子、材质、Trail 与 Bloom 在 reference-linear 阶段始终保持艺术相对的线性 FP16
数值；HDR 开关不能改变内部模拟、材质或 Bloom 能量，只能改变最终输出策略。

- `ArtisticRelative`：使用效果专属校准曲线，不显示 nits 标签。
- `ReferenceWhiteRelative`：以当前显示器 SDR white level 为参考。
- `AbsoluteNits`：只允许显式物理意图的参数；HDR scene-referred 编码使用 `nits / 80`。
- 普通 SDR 使用独立 display-referred rolloff，并公开白底饱和的不可避免降级。

WGC 背景输入白点与最终显示输出白点是两个独立合同：

- WGC 在 HDR/Advanced Color 下提供物理 scRGB，背景 reference white 只负责把捕获像素转入 Unity 相对
  工作空间；它不缩放 Unity authored emission。
- 输出 reference white 只在最终呈现阶段把 Unity 相对 FX 映射到目标屏的 SDR/HDR transport；不得反向
  改写中间 FP16 层。
- 需要背景白点但能力查询结果未知时，捕获 producer 可以保持预热，consumer 必须失效旧快照并回退
  FX-only；不得使用输出白点、固定 `80 nits` 或一单位 scRGB 猜测背景尺度。

Bloom threshold 必须与 seed 使用相同语义；不得让绝对 nits threshold 与艺术相对 emission 混用。

## Acceptance

- Spike C 覆盖 SDR、HDR active、Advanced Color SDR、不同 SDR white level、负 FP16 和大于 1 的值。
- 单元测试证明三种语义不会静默互换，且 NaN/Inf/负 AbsoluteNits 有确定处理。
- UI/配置序列化保存语义标签，而不只保存裸浮点值。
- 代码级合同必须分别记录背景与输出 reference white 的 Required/Valid/Nits，并证明未知背景白点不会参与
  合成。真实 HDR、Advanced Color、多显示器和混合 DPI/刷新率硬件矩阵在执行前保持 `Not Run`；本 ADR
  不因生产路径存在而标记为 Accepted。
