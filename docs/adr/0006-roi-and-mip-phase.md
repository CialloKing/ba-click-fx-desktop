# ADR-006: ROI、guard band 与 mip 相位

- Status: Proposed
- Decision owner: Rendering/Performance

## Context

ROI Bloom 若改变 mip decimation phase、奇数尺寸规则、UV 或 border mode，即使 guard 足够大也不会与
全屏路径等价。guard 是整个算子图的 receptive field，不是一个 kernel 半径乘 mip 数。

## Proposed decision

- full-screen Bloom 是规范路径，ROI 在本 ADR 接受和 VAL-ROI 通过前默认关闭。
- ROI 原点向外对齐到最粗层 `2^L` 网格，尺寸也扩到覆盖目标 dirty rect。
- guard 按最长 pass 路径把各 mip footprint 换算到 mip0 后求和，并包括 down/up/bilinear footprint。
- 前后帧 dirty rect 先 union 再扩张。
- 所有 mip 沿用全屏 pixel-center phase、mip 数、border mode 和奇数尺寸规则。
- 精确有限 kernel 可给 exact guard；近似无限 kernel 必须标注误差上界。
- 任何约束不满足时回退全屏，不能输出“近似等价”而不带误差标记。

## Acceptance

- randomized full-screen-vs-ROI pixel diff，覆盖屏幕边缘、奇数尺寸、负 scRGB、HDR 极值和移动 dirty rect。
- 测试 oracle 独立计算 alignment/guard，避免与生产函数同错。
- 文档记录每个 Bloom preset 的 mip、footprint、guard 和容许误差。
