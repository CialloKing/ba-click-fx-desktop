# ba-click-fx-desktop 0.2.0

0.2.0 是正式测试版（非 prerelease）。它把当前已经完成并经过 Release candidate 门槛验证的 Windows
桌面能力收敛为一个稳定的三段式版本合同。

## 下载哪个包

标准版和 Slim 版都提供以下资产：

- `ba-click-fx-desktop-0.2.0[-slim]-setup-windows-x64.exe`
- 对应的 `.sha256`
- `ba-click-fx-desktop-0.2.0[-slim]-test-windows-x64.zip`
- 对应的 `.sha256`

标准版包含 Spout2 发送器，可按 [`OBS_SPOUT2.md`](OBS_SPOUT2.md) 接入 OBS。Slim 版不包含 Spout2
依赖，也不会在 Control Center 显示 Spout2 控件；两者的 Host、Control Center、配置和基础特效一致。
没有安装权限时使用 portable ZIP。portable 包没有 Package Identity，因此不承诺无边框 WGC。

## 已交付能力

- Windows 10/11 x64 单主屏 SDR 的原生 D3D11/DirectComposition 点击与拖尾特效。
- `background-aware`、`recording-compatible`、`light-background` 三种渲染模式，及面向低性能机器的
  `performance.effectsMode=core`。
- 主题颜色、尺寸、时间、粒子、圆环、碎片、拖尾、Bloom、逐屏状态、帧率策略、暂停/恢复和诊断日志清理。
- 标准版 Spout2/OBS v4 的 `BGRA8 + sRGB + extended premultiplied alpha + FX-only` 输出合同。
- 安装器的当前用户 Package Identity 注册、升级、卸载、Control Center 快捷方式和 `data` 配置保留。

## 录屏兼容测试模式

这是用户主动选择的测试入口，不是默认模式，也不是“保证可录制”的声明。只有 Windows build `>=28000`
且版本探测成功时才允许尝试；更低或无法探测时拒绝请求并保持安全模式。满足门槛后实际路径固定按
`SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly` 回退，日志会记录最终路径。测试通过只能说明
当前机器的透明覆盖层观察结果，不能把 Session-local WGC 提升为正式支持。

## 不在 0.2.0 支持范围内

HDR/Advanced Color、多显示器、混合 DPI 或刷新率、跨适配器、ROI、真实 GPU device lost、外部录屏器兼容性、
无边框 WGC 的跨版本稳定性和自动更新仍为实验项或 `Not Run`。相关诊断入口和 ADR 保持 `Proposed`，不会因为
代码路径存在而改变支持结论。

## 安装、升级和 SmartScreen

从 Alpha 版本升级会保留已有配置和 `data` 目录。当前产品配置使用 `schemaVersion=17`；不匹配的配置继续由
现有解析器安全回退到内存默认值，并保留原文件。安装器使用目标机生成的自签名证书，SmartScreen 可能显示
`Unknown Publisher`。这是预期提示；Release 不附带可单独安装的证书、MSIX、私钥或 SDK 工具。

## 反馈信息

报告问题时请附上 `--support-info` 输出、当前日志及仍存在的轮转日志，并注明 Windows 版本、显示器数量、
GPU/驱动、所用包（标准版或 Slim）和实际渲染模式。HDR、多显示器、Session-local WGC、跨适配器和真实
device lost 场景请按 `Not Run` 处理，不要作为本版回归结论。
