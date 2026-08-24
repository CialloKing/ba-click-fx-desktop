# ba-click-fx-desktop 0.2.6

0.2.6 是让实验性 Active-FX ROI 在 Bloom 首级产生真实局部工作量，并把实际路径、回退原因和 GPU
阶段耗时公开到 Control Center 的正式补丁候选。本版主配置 schema 保持 19，ROI 仍默认关闭；工程面板
展示的是首级诊断，不把像素减少比例包装成 GPU 或端到端性能节省。

## Active-FX ROI 首级实效化

以前的首级绘制虽然使用 scissor，但 `ClearRenderTargetView` 会忽略 scissor 并清理整张目标纹理，
使局部绘制前仍承担全资源清理。0.2.6 为实际 Bloom 首级目标维护初始化状态和上一写入矩形：

- 第一次进入 ROI，或全屏路径之后重新进入 ROI，先完整清理一次并报告 `roi-warmup`；
- 稳态 ROI 使用 `ID3D11DeviceContext1::ClearView` 清理上一写入矩形，再 scissor 绘制当前矩形，报告
  `roi-prefilter`；
- ROI 转全屏时直接完整覆盖；resize、设备恢复和 Bloom 资源重建会重置清理状态；
- Context1 不可用、矩形非法或渲染状态异常时，同帧使用完整全屏路径，不保留未清理的旧像素。

自适应门按首级实际 scissor 面积判断：未进入 ROI 时不超过全目标 50% 才进入；进入后允许维持到
65%，减少阈值附近的往返抖动。接触边界、Bloom 关闭、core 模式、背景差分、无内容和无有效计划等
正确性门始终先于面积收益判断；接近全屏的候选还会单独报告 `area-too-large`。

## 适用路径与明确排除项

本轮只优化纯特效 Bloom 的首级预滤波：

- FX-only primary 可以使用自适应 ROI；
- 录制兼容或 Spout2 纯特效输出需要重建 Bloom 时，recording-rebuild 可以独立使用并独立计数；
- 若 `background-aware` primary 已在共享 Bloom 目标上执行全屏 Differential Bloom，同帧重建会报告
  共享目标全屏写入/全清预热，不会误报为稳定局部收益。

默认 `background-aware` primary 的 Differential Bloom、Bloom 后续 down/up/resolve、最终合成、WGC、
Spout2 格式转换、交换链和 Present 仍保持全屏。0.2.6 不是完整 Bloom ROI、桌面捕获 ROI 或 dirty Present。

## 工程面板与状态协议

“显示与性能”页的开关改名为“启用自适应 Active-FX ROI（实验）”。选中显示器后，工程面板分别展示
primary 和 recording-rebuild：

- 当前实际路径与人类可读的决策/回退原因；
- 近 5 秒观察、请求、合格、应用、预热及各原因帧数；
- 首级完整、绘制和清理像素累计值与处理比例；
- dirty/aligned rect、guard、phase；
- Prefilter、Pyramid、FinalComposite 的 GPU p50/p95。

`GetDisplayState` 升级为严格 schema 3，每个 session 新增 `activeFxRoi`。Host 每 500 ms 发布一份脱离
渲染器的不可变快照；Control Center 仅在页面可见时每秒读取，离页停止，样本超过 3 秒会标记为 stale。
IPC 失败不会改变 ROI 开关、其他配置或 Host 运行状态。

schema 3 不提供 schema 2 兼容层。混用 0.2.5 Control Center 与 0.2.6 Host（或反向混用）会继续由
`GetState.productVersion` 门禁禁用设置写入；Host 启动/关闭入口仍可用于完成升级。

## 配置、Profile 与升级

主配置保持 `schemaVersion=19`。新安装、从旧 schema 迁移和重置默认时，
`performance.activeFxRoiEnabled=false`；已有 schema 19 中用户显式保存的值原样保留。本版不修改用户
配置格式、显示器 override、主配置迁移链、effects-only Profile 或渲染协议。Profile 仍不包含
`performance`，因此应用或切换 Profile 不会打开、关闭或覆盖 ROI。

安装版升级继续保留 `data`、主配置和 `fx-profiles`。便携版应整体替换程序文件并保留原数据目录，
不要混用不同版本的 Host 与 Control Center。安装器由用户手动运行，可能触发 UAC；程序不会自动下载
或执行安装器。

## 发布性能门槛

0.2.6 只有在 RTX 4060、4K 170 Hz、SDR 的专用 A/B 验证通过后才能发布性能声明和正式 Release。
采集固定使用同一 EXE、同一场景，schema 19 配置唯一差异为 ROI 开关；每个场景执行 5 个 ABBA 块、
20 次采集，每次预热 5 秒并采样 30 秒。必须同时满足：

- `Applied/Requested >= 95%`，首级绘制像素比例不超过 45%；
- Prefilter GPU p95 至少降低 25%；Bloom/final p95 至少降低 `max(5%, 100 us)`；
- 10 组配对中至少 8 组 ROI 不慢于全屏；FPS 降幅不超过 1%；
- CPU、Present 和 p99 恶化不超过 5%；GPU pending 最大值不超过 1；查询、节流和状态错误为 0；
- 触边、面积回退及无 Spout2 的 background-aware 场景必须 100% 命中预期原因并保持 exact，性能恶化
  不超过 `max(3%, 100 us)`。

若端到端门槛未通过，本版不得通过放宽阈值发布；后续工作直接转入完整 Bloom down/up/resolve ROI。

## 发布验证状态

2026-08-24 已在 `NVIDIA GeForce RTX 4060 Laptop GPU`、`3840x2160`、`170/1 Hz`、SDR、
`conservative-sdr` 上完成 center-click primary 的 5 个 ABBA 块、20 次正式采集。证据目录为
`artifacts/performance/active-fx-roi-v026-rtx4060-4k170-sdr-center-click-20260824-r2`，采集 revision
为 `08c8365693aedaab9dcd79582ba49967c100a3ca`。结果为 **FAIL**：

- `Applied/Requested=100%`、Prefilter GPU p95 降低 `53.5%`、GPU pending 最大值 `1`，这些门通过；
- 绘制像素比例为 `46.8%`，超过 `45%`；Bloom/final GPU p95 从 `33040.5 us` 增至
  `38050.5 us`，恶化 `15.2%`；
- FPS 从 `101.914` 降至 `94.574`，下降 `7.2%`；10 组配对仅 `6` 组不慢；
- CPU frame、Present 与 GPU command p99 均超过 `5%` 恶化门，且两臂都出现非零
  `FramePacing.Timeouts`。

因此不放宽阈值、不发布 0.2.6、不启动三档 SDK 发布 CI，也不生成或上传 0.2.6 Release 资产。
下一候选版本直接进入 0.2.7 完整 Bloom 金字塔 ROI。

Slim 只做源码构建验证，不发布预编译资产。AMD、Intel、HDR、Windows 11、多显示器与跨适配器 ROI
矩阵在取得真实证据前保持 `Not Run`，不属于 0.2.6 的性能支持声明。

## 后续顺序

ROI 后续顺序固定为：完整 Bloom down/up/resolve ROI，然后才是默认 `background-aware` 的
Differential Bloom ROI。WGC、交换链 dirty Present 和桌面捕获 ROI 继续作为相互独立的高风险项目。
