# 架构 Spike 验收规范

四个 P0 Spike 都是架构状态从 Proposed 升级的必要证据。主假设失败不自动阻塞架构接受，但必须有
已接受的 fallback 并同步收窄能力矩阵。执行结果保存在
`artifacts/spikes/<spike>/<machine-and-date>/`；本机临时输出放在被 Git 忽略的 `artifacts/local/`。
每次结果必须包含 OS build、GPU/driver、显示器/色彩模式、应用 commit、步骤、原始输出和结论。

## SPK-001 / Spike A：DirectComposition FP16 扩展预乘合成

### 要回答的问题

DXGI premultiplied alpha 的 FP16 surface 经 DirectComposition/DWM 后，`A=0, RGB>0` 与 `RGB>A`
是否按预期形成加法 source-over，且没有被项目自身 pass canonicalize。

### 矩阵

- 普通 SDR；HDR active（若设备支持）。
- 黑、18% 灰、彩色和白背景。
- `(rgb,a)` 至少包含 `(0,0)`、`(0.25,0)`、`(1,0.25)`、`(4,0.5)`。

### Pass

- GPU readback 与最终 shader 期望值在 FP16 容差内。
- 桌面捕获/显示测量与 source-over 方向一致，`A=0, RGB>0` 不被 overlay 内部清零。
- 普通 SDR 白底饱和被记录为产品降级，而不是测试失败或偷偷压暗桌面。

## SPK-002 / Spike B：WGC 生命周期、自排除与录屏

### 场景

- capture 授权成功与拒绝；borderless 请求成功与失败。
- cursor inclusion/exclusion、ContentSize 改变、session restart、窗口/显示器关闭。
- `BackgroundAware` 与 `RecordingCompatible` 模式切换。
- 至少一种桌面捕获与一种窗口捕获录屏路径。

### Pass

- 回调只通知，Render Owner 串行 drain/copy/close；无 frame 生命周期泄漏。
- self-exclusion 不产生递归反馈；失败时退回 FX-only 并有诊断。
- 录屏结果按“观察”记录，不将单一录屏器结论泛化。

## SPK-003 / Spike C：Color/HDR 输出

### 场景

- 普通 SDR、HDR active、Advanced Color SDR（设备支持项）。
- 枚举并记录 SDR white level。
- 保存 `Display.ColorMode`、`Display.DxgiColorSpaceValue`、位深和亮度元数据；零亮度元数据按
  `unknown` 处理，不能作为显示器实测亮度。
- FP16 负值、0..1、`>1` 与名义 1000-nit 编码输入。

### Pass

- `ArtisticRelative`、`ReferenceWhiteRelative`、`AbsoluteNits` 的映射路径可在日志和 readback 中区分。
- 没有把 Unity 艺术值标成 nits。
- SDR white saturation、DWM clamp/映射和 HDR 显示观察均明确记录；未知路径保守降级。

## SPK-004 / Spike D：显示拓扑与资源域

### 场景

- 60+144 Hz、144+240 Hz（有硬件才执行）。
- SDR+HDR、iGPU+dGPU、旋转、不同 DPI、热插拔、运行中 HDR toggle。

### Pass

- 每个 monitor 可追溯到 Adapter LUID 和独立资源域。
- 指针跨屏连续，present cadence 不被最慢显示器全局锁定。
- 跨 adapter、热插拔、旋转和 device lost 使用重建路径，无悬挂共享资源。
- 缺少硬件的单元格标为 `Not Run`，不能以模拟结果标成 Passed。

## 状态模板

```text
Spike:
Commit:
Machine/OS:
GPU/driver:
Displays/color modes:
Steps:
Raw evidence:
Result: Passed | Failed | Not Run
Limitations:
```
