# ba-click-fx-desktop 0.2.7

0.2.7 引入默认关闭的 Active-FX ROI 完整 Bloom 金字塔。本版把 0.2.6 只覆盖首级预滤波的局部路径
扩展到 prefilter、全部 downsample/upsample，并为满足严格合同的 steady pure-FX primary 帧加入局部
最终输出和 `Present1` dirty rect。主配置仍为 schema 19，`performance.activeFxRoiEnabled=false`；
没有新增第二个开关，也不迁移现有配置。

本版已完成实现和确定性 WARP 等价矩阵。旧 revision 的 RTX 4060、4K 170 Hz、SDR 正式 ABBA
失败证据继续保留；当前实现尚无满足同一显示合同的正式采集。因此本文不包含整机性能、功耗、输入延迟
或普遍硬件支持声明。

## 完整 Bloom 金字塔 ROI

ROI 规划器为每帧生成一份不可变的完整 pass 计划：

- prefilter、每级 downsample、每级 upsample 和 resolve 分别拥有目标本地 half-open 矩形；
- mip 数、全屏 viewport/UV、奇数尺寸规则、border mode 和 pixel-center phase 与规范全屏路径一致；
- 每级矩形由前向非零支持与反向贡献依赖共同约束；resolve 使用前向结果，避免奇数 mip 尺寸下漏掉
  仍有非零 Bloom 权重的像素；
- `basePlan.bloomOutput` 和 `alignedWork` 只保留旧诊断语义，不再充当完整 pass 的正确性边界。

prefilter 和 down/up 在原尺寸目标上使用 scissor，不创建另一套裁剪纹理或改变 UV。steady pure-FX
primary 只有在实际路径为 `RoiPyramid + Applied`、不处于预热、无背景参与，且最终输出矩形与 resolve
像素逐字段一致时，才只清理并绘制当前与上一帧视觉范围的并集，再以一个 dirty rect 调用 `Present1`。
warmup、background-aware、recording-rebuild 和任一 fallback 仍写出完整结果并使用普通 Present。

## 清理状态与同帧回退

每个实际 down/up 目标维护初始化状态、上一写入矩形、是否曾被全屏写入、最后 writer 和全屏写入帧号：

- 第一次使用 ROI，或上一次为全屏写入时，先完整清理所有目标并报告 `roi-warmup`；
- 稳态矩形移动或 writer 改变时通过 `ID3D11DeviceContext1::ClearView` 清理每个目标的上一写入矩形；
  同一 writer 连续覆盖相同矩形时直接跳过冗余清理，再绘制当前矩形；
- resize、设备恢复和 Bloom 资源重建会使物理目标状态重新初始化；
- primary 与 recording-rebuild 分别维护自适应和诊断统计，但共享同一组物理资源状态。若另一条路径在
  同帧完成全屏写入，后续路径必须按共享写入事实预热，不能误报稳定局部收益。

ROI 采用整条 Bloom 全有或全无策略。pass 计划、Context1、资源身份、目标状态或 phase 任一无效时，
本帧 prefilter、down、up 和 resolve 全部使用规范全屏路径，不允许把局部 pass 与全屏 pass 混合。
局部最终输出开始后不存在安全的 full Present 回退；Present 失败会使 ROI 写入状态失效，下一帧必须完整
重建，避免把矩形外未更新像素发布为新内容。

## 诊断协议

`GetDisplayState` 升级为严格 schema 4，并新增：

- 实际路径 `roi-pyramid`；
- primary 与 recording-rebuild 各自的 prefilter/downsample/upsample/resolve
  `fullPixels`、`candidatePixels`、`drawnPixels` 和 `clearedPixels`；
- 既有路径、决策原因、近 5 秒帧计数、矩形、guard/phase 与 Prefilter/Pyramid/FinalComposite GPU
  p50/p95。

Host 每 500 ms 发布不可变快照，Control Center 只在“显示与性能”页可见时每秒读取，超过 3 秒标记
stale。schema 4 不接受 schema 3；Host 与 Control Center 混用版本时，`GetState.productVersion` 继续
fail-closed 禁止设置写入，启动和关闭入口仍可用于完成升级。

## 正确性验证

纯函数规划器覆盖固定种子随机矩形、移动 dirty rect、四边四角、奇偶分辨率、diffusion
`4/6/7/10`、空区域、非法输入和溢出，并以逐像素 oracle 验证前向支持。

WARP 矩阵比较 ROI 与全屏的 prefilter、全部 Down/Up、BloomResult 和 FinalOverlay，覆盖点击、拖尾、
负 scRGB、HDR 极值、Spout2、resize 后奇数尺寸、移动/不相交区域、空帧重启、Context1 缺失、
`D3D11_OPTIONS.ClearView=false`、FP16/BGRA8 最终输出、MRT/录制完整输出及局部输出外哨兵。
同一 WARP 适配器上的确定性结果必须精确一致。默认 `background-aware` primary 仍命中
`background-differential-bloom` 并完整走全屏；完整 Release CTest 为 `45/45`。

这些测试证明当前确定性渲染合同，不证明 RTX 4060 或其他硬件上的性能、功耗和帧稳定性。

## 0.2.6 失败历史

0.2.6 已在 `NVIDIA GeForce RTX 4060 Laptop GPU`、`3840x2160`、`170/1 Hz`、SDR 上完成 5 组
ABBA、20 次正式采集，报告为 `FAIL`：Bloom/final p95 恶化 `15.2%`、FPS 下降 `7.2%`、仅
`6/10` 配对不慢、首级绘制比例 `46.8% > 45%`，且两臂均出现非零 `FramePacing.Timeouts`。

因此阈值没有放宽，0.2.6 没有发布，也没有继续执行发布 CI、打包或上传资产。历史说明继续保留在
[`RELEASE_NOTES_0.2.6.md`](RELEASE_NOTES_0.2.6.md)，原始证据保留在既有 `artifacts/performance` 目录。

## 0.2.7 发布门结果

新的正式合同使用 capture schema 4 和 report schema 4。金字塔 drawn/full、Prefilter/Pyramid/Bloom
收益、相邻 ABBA 配对、FPS、GPU command p99、GPU pending、错误计数以及 dirty Present 实际覆盖继续
作为硬门。CPU FrameTotal 与 PresentCall p95/p99 仍按原阈值报告，但作为非阻塞 advisory；两者包含
同一次同步 Present 墙钟阻塞，固定年龄场景又明确禁用 Raw Input，不能把四个相关 percentile 当成四份
独立的交互延迟证据。

最近一份无已知外部负载污染的旧合同正式证据位于
`artifacts/performance/active-fx-roi-v027-final-scissor-clean-rtx4060-4k170-sdr-center-click-20260829-r1`，
revision 为 `11fba7243faf4dee1eb18a7d740c3c3b6f7a1479`，环境为 `NVIDIA GeForce RTX 4060 Laptop GPU`、
`3840x2160`、`170/1 Hz`、SDR、`conservative-sdr`。结果为 **FAIL**：

- Bloom/final p95 从 `1671 us` 降至 `822 us`，GPU command p99 从 `2085 us` 降至 `1075 us`，
  FPS 为 `170.043 -> 170.036`，`8/10` 相邻 Bloom 配对不慢，GPU pending 最大值为 `1`，错误计数为零；
- CPU FrameTotal p95/p99 为 `432/585.5 -> 1210/1657 us`，PresentCall p95/p99 为
  `382/540 -> 1162/1612.5 us`。原始窗口显示这些值具有明显调度相位分布；按每帧
  `FrameTotal + FramePacing.Wait` 解释性重算为 `5801.52 -> 5802.70 us`，与 FPS 一致，但该固定场景
  不能证明真实输入延迟。

该旧报告保持 **FAIL**，不会用 report schema 4 追溯改判。它证明旧 revision 的局部 GPU 收益，也记录
当时的 Present 阻塞分布；它不再单独阻塞 ROI 默认关闭、无性能声明的 0.2.7 普通发布。

当前实现计划执行 8-run 的 dirty-Present 短诊断，但在 2026-08-31 首个 ROI-off run 后即
fail-closed：实际主输出为 `2560x1440 @ 165.003 Hz`，不满足预注册 `3840x2160 @ 170 Hz` 环境合同，
因此没有生成可用于晋级的新性能结论。

本地 Full `release-verify` 已通过 `45/45`，Slim `slim-release-verify` 已通过 `44/44`。官方 GitHub
Release 只提供 Full 便携 ZIP、ZIP 哈希、安装器和安装器哈希四个资产；Slim 只保留源码构建与验证，
不上传预编译资产。

## 明确排除项

0.2.7 不局部化默认 `background-aware` 的 Differential Bloom、WGC copy、Spout2 格式转换、桌面捕获
或 recording-rebuild 最终输出。受限 dirty Present 只用于验证后的 steady pure-FX primary；交换链
资源尺寸没有缩小，也不能外推为所有 DWM、录屏器或驱动上的局部呈现支持。Differential Bloom ROI 属于
0.2.8，必须在新的性能晋级和背景代次、白点、输出目标、资源恢复合同下独立开发。

ROI 继续保持实验性和默认关闭。AMD、Intel、HDR、Windows 11、多显示器与跨适配器矩阵在真实执行前
保持 `Not Run`；旧 RTX 4060 失败、WARP 或 dirty Present 计数都不会扩大支持范围或改变默认值。
