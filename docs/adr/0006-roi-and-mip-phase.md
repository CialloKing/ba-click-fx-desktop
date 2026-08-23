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

## 当前生产切片

`bafx::core::planUnityBloomRoi` 已按当前 `FourTap` prefilter/downsample/upsample/resolve
链生成保守 receptive-field、guard 和 phase。以 Unity 默认 `1950x1097`、diffusion `7`
为例，计划得到 `378px` guard 和 `64px` phase。

schema 19 增加默认关闭的 `performance.activeFxRoiEnabled`。启用后，生产渲染器只把对齐后的工作区
用于纯特效 Bloom 首级预滤波的 D3D11 scissor；首级目标先做全纹理清零，因此区域外不会复用上一帧
内容。所有后续 down/up、Bloom resolve、最终合成、WGC 拷贝与 Present 仍按原全屏尺寸、UV、mip
和 pixel-center phase 执行。该切片不会改变资源尺寸，也不声称完成全链路 ROI。

工作区触及屏幕边缘、达到全屏面积的 80%、Bloom 被关闭、core 模式、计划不可用，或背景差分路径
无法保持纯特效首级输入时，当前帧回退全屏预滤波。性能日志区分请求帧、实际应用帧、预滤波像素，
并明确最终合成与 WGC 拷贝仍为全屏。WARP 回归已锁定一个内部工作区在 FP16 最终输出上与全屏路径
逐元素相同；随机边缘、奇数尺寸、负 scRGB、HDR 极值和真实硬件收益仍属于 Acceptance，故本 ADR
继续保持 Proposed，开关继续默认关闭。

## Acceptance

- randomized full-screen-vs-ROI pixel diff，覆盖屏幕边缘、奇数尺寸、负 scRGB、HDR 极值和移动 dirty rect。
- 测试 oracle 独立计算 alignment/guard，避免与生产函数同错。
- 文档记录每个 Bloom preset 的 mip、footprint、guard 和容许误差。
