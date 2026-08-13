# ADR-005: Golden Oracle 层级

- Status: Proposed
- Decision owner: Validation/Rendering

## Context

Web 实现与 Unity 在早期时序和 Bloom 上已有可观测差异，因此 Web 像素不能作为原生实现真值。
同时，FP16 GPU 结果受驱动、滤波和显示映射影响，单纯文件 hash 既脆弱也不足以证明视觉一致。

## Proposed decision

验证证据按优先级分三层：

1. Unity/游戏提取资源与受控捕获：视觉真值；
2. `ba-click-fx`：交互行为、命名与参数兼容参考；
3. 原生中间 buffer：实现定位与回归证据。

每个 Golden case 至少导出 Coverage、DirectEmission、BloomSeed、DifferentialPrefilter、BloomMipN、
BloomResult、FinalOverlay。FP16 使用绝对/相对误差与感知指标；只有离散元数据使用 hash。

## Acceptance

- 固定 QPC、随机种子、viewport、DPI、色彩编码和输入事件。
- 关键时间点覆盖 0/50/100/200/450 ms 及 release 后衰减。
- Tri2 三角碎片的 BloomSeed 在点击和拖拽 case 中均应为非零（在可见像素存在时），并且其
  Down/Up mip 与 FinalOverlay 能观察到对应的 Bloom 传播；非 Bloom 材质仍须保持零种子。
- 失败报告能定位到第一个偏离的中间 buffer，而不只给最终截图。
