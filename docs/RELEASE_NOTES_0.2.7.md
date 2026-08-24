# ba-click-fx-desktop 0.2.7

0.2.7 是默认关闭的 Active-FX ROI 完整 Bloom 金字塔候选。本版把 0.2.6 只覆盖首级预滤波的局部路径
扩展到 prefilter、全部 downsample/upsample，并在全屏最终合成中加入 resolve 有效区掩码。主配置仍为
schema 19，`performance.activeFxRoiEnabled=false`；没有新增第二个开关，也不迁移现有配置。

当前源码已完成实现和确定性 WARP 等价矩阵，但 RTX 4060、4K 170 Hz、SDR 的 0.2.7 正式 ABBA 尚未
执行。因此本文是候选说明，不是已发布公告，也不包含性能通过或硬件支持声明。

## 完整 Bloom 金字塔 ROI

ROI 规划器为每帧生成一份不可变的完整 pass 计划：

- prefilter、每级 downsample、每级 upsample 和 resolve 分别拥有目标本地 half-open 矩形；
- mip 数、全屏 viewport/UV、奇数尺寸规则、border mode 和 pixel-center phase 与规范全屏路径一致；
- 每级矩形由前向非零支持与反向贡献依赖共同约束；resolve 使用前向结果，避免奇数 mip 尺寸下漏掉
  仍有非零 Bloom 权重的像素；
- `basePlan.bloomOutput` 和 `alignedWork` 只保留旧诊断语义，不再充当完整 pass 的正确性边界。

prefilter 和 down/up 在原尺寸目标上使用 scissor，不创建另一套裁剪纹理或改变 UV。resolve 与最终场景
合成仍融合为一个全屏 draw；shader 在 `resolveRect` 外返回精确零 Bloom。诊断中的 resolve
`drawnPixels` 因此是全屏，而 `candidatePixels` 是逻辑有效区，两者表达不同工作量，不能相互替代。

## 清理状态与同帧回退

每个实际 down/up 目标维护初始化状态、上一写入矩形、是否曾被全屏写入、最后 writer 和全屏写入帧号：

- 第一次使用 ROI，或上一次为全屏写入时，先完整清理所有目标并报告 `roi-warmup`；
- 稳态通过 `ID3D11DeviceContext1::ClearView` 清理每个目标的上一写入矩形，再绘制当前矩形；
- resize、设备恢复和 Bloom 资源重建会使物理目标状态重新初始化；
- primary 与 recording-rebuild 分别维护自适应和诊断统计，但共享同一组物理资源状态。若另一条路径在
  同帧完成全屏写入，后续路径必须按共享写入事实预热，不能误报稳定局部收益。

ROI 采用整条 Bloom 全有或全无策略。pass 计划、Context1、资源身份、目标状态或 phase 任一无效时，
本帧 prefilter、down、up 和 resolve 全部使用规范全屏路径，不允许把局部 pass 与全屏 pass 混合。

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
负 scRGB、HDR 极值、Spout2、resize 后奇数尺寸、移动/不相交区域、空帧重启、Context1 缺失及
`D3D11_OPTIONS.ClearView=false`。
同一 WARP 适配器上的确定性 FP16 结果必须精确一致。默认 `background-aware` primary 仍命中
`background-differential-bloom` 并完整走全屏。

这些测试证明当前确定性渲染合同，不证明 RTX 4060 或其他硬件上的性能、功耗和帧稳定性。

## 0.2.6 失败历史

0.2.6 已在 `NVIDIA GeForce RTX 4060 Laptop GPU`、`3840x2160`、`170/1 Hz`、SDR 上完成 5 组
ABBA、20 次正式采集，报告为 `FAIL`：Bloom/final p95 恶化 `15.2%`、FPS 下降 `7.2%`、仅
`6/10` 配对不慢、首级绘制比例 `46.8% > 45%`，且两臂均出现非零 `FramePacing.Timeouts`。

因此阈值没有放宽，0.2.6 没有发布，也没有继续执行发布 CI、打包或上传资产。历史说明继续保留在
[`RELEASE_NOTES_0.2.6.md`](RELEASE_NOTES_0.2.6.md)，原始证据保留在既有 `artifacts/performance` 目录。

## 0.2.7 发布门

0.2.7 必须重新在 RTX 4060、4K 170 Hz、SDR 上执行同一 EXE、同一场景、仅切换 ROI 开关的 5 组
ABBA、20 次采集。除既有门槛外，还要求金字塔聚合 drawn/full 像素比例不超过 45%，Pyramid GPU
p95 至少降低 25%；Bloom/final p95 至少降低 `max(5%, 100 us)`，至少 `8/10` 配对不慢，FPS 降幅不
超过 1%，CPU/Present/p99 恶化不超过 5%，GPU pending 最大值不超过 1，所有错误计数为零。

门槛通过前不打 tag、不创建 GitHub Release，也不发布性能收益声明。全部通过后才执行 Full/Slim
workflow、Windows SDK `19041/22621/26100` CI、打包和远端 SHA-256 复核。官方 Release 仍只发布
Full 版四个资产：

- `ba-click-fx-desktop-0.2.7-Portable-windows-x64.zip`；
- `ba-click-fx-desktop-0.2.7-Portable-windows-x64.zip.sha256`；
- `ba-click-fx-desktop-0.2.7-setup-windows-x64.exe`；
- `ba-click-fx-desktop-0.2.7-setup-windows-x64.exe.sha256`。

Slim 只保留源码构建、测试和本地打包验证，不提供预编译 Release 资产。

## 明确排除项

0.2.7 不局部化默认 `background-aware` 的 Differential Bloom、最终场景合成、WGC copy、Spout2 格式
转换、交换链或 Present，也不实现桌面捕获 ROI 或 dirty Present。Differential Bloom ROI 属于 0.2.8，
必须在 0.2.7 独立交付后另行接入背景代次、白点、输出目标和资源恢复回退合同。

ROI 继续保持实验性和默认关闭。AMD、Intel、HDR、Windows 11、多显示器与跨适配器矩阵在真实执行前
保持 `Not Run`；即使单台 RTX 4060 后续通过，也不会自动把 ROI 改为默认开启。
