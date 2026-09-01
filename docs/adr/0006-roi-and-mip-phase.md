# ADR-006: ROI、guard band 与 mip 相位

- Status: Proposed
- Decision owner: Rendering/Performance

## Context

ROI Bloom 若改变 mip decimation phase、奇数尺寸规则、UV 或 border mode，即使 guard 足够大也不会与
全屏路径等价。guard 是整个算子图的 receptive field，不是一个 kernel 半径乘 mip 数。

## Proposed decision

- full-screen Bloom 是规范路径，ROI 产品开关在后续版本均保持默认关闭。本 ADR 接受和 VAL-ROI 通过
  只决定手动启用的实验路径是否具备声明条件，不自动改变产品默认值。
- 每个 pass 在原全屏 viewport 上单独规划目标本地 half-open 矩形，不裁剪纹理或重置 UV 原点。
- 矩形同时受前向非零支持和反向贡献依赖约束；旧 guard/aligned work 只保留诊断用途。
- 前后帧 dirty rect 先 union 再扩张。
- 所有 mip 沿用全屏 pixel-center phase、mip 数、border mode 和奇数尺寸规则。
- 精确有限 kernel 可给 exact guard；近似无限 kernel 必须标注误差上界。
- 任何约束不满足时回退全屏，不能输出“近似等价”而不带误差标记。

## 当前生产切片

`bafx::core::planUnityBloomPassRoi` 已按当前 `FourTap`
prefilter/downsample/upsample/resolve 链生成逐 pass 矩形。规划器在原 viewport 和 pixel-center phase 上
传播前向非零支持，再与反向贡献依赖相交；`resolveRect` 取前向结果，避免奇数 mip 尺寸下漏掉仍有
非零 Bloom 权重的像素。`planUnityBloomRoi` 的 guard、phase 和 aligned work 继续用于兼容诊断，不再是
完整金字塔的正确性边界。

schema 19 保留默认关闭的 `performance.activeFxRoiEnabled`。启用后，纯特效路径会在真实 Bloom 目标上
scissor 绘制 prefilter 和每级 down/up。每个目标共享一份初始化、上一写入矩形、全屏写入和最后 writer
状态；首次进入或全屏写入后完整清理，稳态只有在 Context1 和驱动 `ClearView` capability 都有效时才用
`ClearView` 清理发生变化的旧区；同一 writer 连续覆盖相同矩形时直接跳过冗余清理。

交换链表面的局部写入与 dirty Present 受更严格的合同限制：只有 pure-FX primary 的实际路径为
`RoiPyramid`、决策为 `Applied`、帧非 warmup，且 renderer 已将最终目标的清理和 draw 都限定在同一 `resolveRect`
并标记为已验证局部输出时，才允许 `Present1` 声明该 dirty rect。Present 选择器会再检查计划、
矩形边界与 resolve candidate/drawn/cleared 像素一致性。warmup、background-aware/Differential Bloom、
WGC 相关路径、任何 fallback 与普通调用者仍完整清理、完整输出并使用全屏 Present。
recording-rebuild 和 Spout2 输出目标始终建立完整表面，不借用交换链的 dirty-present 保证；
同帧 pure-FX primary 只在独立满足上述合同时可以局部提交交换链目标。

pass 计划、Context1/ClearView、资源身份、相位或状态任一无效时，当前帧整条 Bloom 回退规范全屏路径，
不允许局部与全屏 pass 混用。primary 与 recording-rebuild 分别记账，但按真实物理资源共享写入状态。
一旦最终目标已局部写入，dirty rect 选择失败或 `Present1` 失败都不得再回退全屏 Present；
必须重置 ROI 写入状态，不提交未成功显示帧的可见边界，并让下次尝试通过完整清理、完整输出和
全屏 Present 重建。
WARP 已覆盖随机/边角规划、点击、拖尾、移动 dirty rect、奇数尺寸、负 scRGB、HDR 极值、Spout2
recording target、空帧重启和 Context1/ClearView 缺失，并要求同适配器 FP16 精确一致；它只证明
D3D 输出表面合同。dirty-Present 帧/像素遥测也只证明生产路径选择，两者都不等于 DWM 可见正确性、
真实功耗收益或跨硬件支持。旧 schema 的 RTX 4060、4K 170 Hz ABBA 继续按生成时合同保留为 `FAIL`；
当前 schema 4 将重叠观察同一次同步阻塞的 CPU frame 与 Present percentile 保留为非阻塞 advisory，
但新的同规格正式硬件晋级矩阵仍为 `Not Run`。因此本 ADR 继续保持 Proposed，开关继续默认关闭。

## Acceptance

- RTX 4060、4K 170 Hz、SDR 的预注册 ABBA 性能与稳定性门槛。
- 实机 DWM 的静态与移动 dirty rect 像素验收，包括失败后的完整重建。
- AMD、Intel、HDR、Windows 11、多显示器和跨适配器的真实硬件矩阵。
- 独立 Differential Bloom ROI 实验里程碑的背景代次、白点、输出目标和资源恢复回退合同。
- 测试 oracle 独立计算逐像素前向支持和反向依赖，避免与生产函数同错。
- 文档记录每个 Bloom preset 的 mip、footprint、phase 和容许误差。
