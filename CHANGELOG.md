# 变更记录

## 未发布

### 修复

- 修复 Spout2/OBS 把独立 Bloom 做 sRGB 提亮后直接叠加到编码游戏画面，导致光晕暗部被放大、
  中心高光过早削顶的问题。v5 输出改用保色相 SDR rolloff，并保留 v4 的扩展预乘 Alpha 合同。

## 0.2.2 - 2026-08-21

### 修复

- 修复 Host 启动前已经连接 HDMI 采集卡时，采集端的活动 DisplayConfig source 没有
  `HMONITOR`，导致全局拓扑为 `incomplete / ERROR_NOT_FOUND`，内置主屏的 SDR white
  查询被一并阻断，WGC 虽持续取得帧但背景无法参与合成的问题。
- 新增逐显示器的颜色路径完整性门。采集端等无法映射到桌面显示器的独立路径仍保留全局
  `incomplete` 诊断，但不再污染证据完整的内置屏颜色状态。
- 无法归属到具体 source、source 身份不完整或缺少物理 target 时继续 fail-closed，避免在
  HDR、克隆显示或真实不完整路径中误用颜色状态。

### 验证与支持边界

- 已在笔记本内置屏加 HDMI 采集卡的真实冷启动场景确认：全局拓扑保持
  `incomplete / ERROR_NOT_FOUND` 时，内屏仍取得 fresh SDR 颜色快照，WGC 帧和背景合成帧
  持续推进，无需重新热插拔采集卡。
- 本补丁不扩大单主屏 SDR 支持合同。多显示器、HDR/Advanced Color、混合 DPI/刷新率、
  跨适配器和完整热插拔矩阵仍为实验项或 `Not Run`。

## 0.2.1 - 2026-08-21

### 修复

- 修复笔记本内置屏幕使用 `background-aware` 时，HDMI 采集卡引起显示拓扑变化后
  WGC 会话停止但背景捕获没有重启，画面长期回退为 FX-only 的问题。
- 新增一次性、5 秒有界的显示拓扑恢复门，兼容 Win32 拓扑通知与 WGC
  `session-stopped` 的两种到达顺序。恢复只在硬件渲染、配置仍请求背景捕获、
  WGC 允许重启且显示器供电可用时发生。
- 新增 `WGC.DisplayTopologyRecovery.Requested` 结构化日志，记录恢复原因、重试令牌、
  拓扑状态与恢复窗口，便于后续硬件验收。

### 支持边界

- 本补丁仅恢复 HDMI 拓扑变化后的内置主屏 WGC 背景捕获，不扩大 0.2.0 的
  单主屏 SDR 支持合同。多显示器、混合 DPI/刷新率、跨适配器和完整热插拔矩阵
  仍为实验项或 `Not Run`。

## 0.2.0 - 2026-08-21

0.2.0 是首个不带 prerelease 后缀的正式测试版。公共版本号从本版起严格使用
`MAJOR.MINOR.PATCH`；Windows 文件和 Package Identity 版本使用对应的
`MAJOR.MINOR.PATCH.0`。

### 当前能力

- Windows 10/11 x64、单主屏 SDR 下的原生点击、拖尾、圆环、碎片和 Bloom 特效。
- `background-aware`、`recording-compatible`、`light-background` 三种渲染模式，以及低配 `core` 模式。
- Host 与原生 Win32 Control Center 的本地配置、暂停/恢复、主题色、逐屏状态、帧率策略和日志清理。
- 标准版 Spout2/OBS v4 透明扩展预乘输出；Slim 版移除 Spout2 依赖和控制项，保留核心特效。
- 便携测试 ZIP 和目标机自签名单文件安装器；安装器支持安装、升级、卸载和保留 `data` 配置目录。

### 支持边界

- 正式测试范围是 Windows 10/11 x64、单主屏、SDR。便携版没有 Package Identity，不能承诺无边框 WGC。
- HDR、Advanced Color、多显示器、混合 DPI/刷新率、跨适配器、真实 device lost、ROI、外部录屏兼容性和
  Session-local WGC 仍是实验项或 `Not Run`，不属于 0.2.0 支持声明。
- `recording-compatible` 是用户主动选择的测试入口。Windows build `>=28000` 才允许尝试，失败时按
  `SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly` 回退；这不等于 Session-local WGC 已通过验收。

### 安装与升级

- 从 Alpha 版本升级时保留现有配置和 `data` 目录；配置 schema 不匹配继续采用当前安全回退策略，不覆盖原文件。
- 安装器使用目标机自签名证书，Windows SmartScreen 可能显示 `Unknown Publisher`。这是预期行为，Release 不提供
  可单独安装的证书、MSIX、私钥或 SDK 工具。
