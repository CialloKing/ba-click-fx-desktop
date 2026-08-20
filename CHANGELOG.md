# 变更记录

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
