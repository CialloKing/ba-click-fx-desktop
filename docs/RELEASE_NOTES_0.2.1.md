# ba-click-fx-desktop 0.2.1

0.2.1 是针对显示拓扑变化后 WGC 背景捕获恢复的正式补丁版（非 prerelease）。

## 修复内容

笔记本内置屏幕使用 `background-aware` 时，接入或拔出 HDMI 采集卡会改变 Windows
显示拓扑。WGC 可能随后报告 `session-stopped`，原实现会回退到 FX-only，但没有为这类
拓扑故障安排新的重试令牌，因此内置屏幕的背景捕获会持续失效。

本版在渲染所有者线程上增加一次性、5 秒有界的拓扑恢复门，同时处理
Win32 拓扑通知先到和 WGC 会话停止先到两种时序。恢复只在下列条件都满足时发生：

- 使用硬件渲染；
- 当前配置仍请求 WGC 背景捕获；
- 本进程仍允许 WGC 重启；
- 显示器供电可用。

日志会以 `WGC.DisplayTopologyRecovery.Requested` 记录恢复原因、重试令牌、拓扑状态和
恢复窗口。

## 下载哪个包

标准版和 Slim 版都提供便携 ZIP、安装器及各自的 SHA-256 文件：

- `ba-click-fx-desktop-0.2.1-test-windows-x64.zip`
- `ba-click-fx-desktop-0.2.1-test-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.1-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.1-setup-windows-x64.exe.sha256`
- `ba-click-fx-desktop-0.2.1-slim-test-windows-x64.zip`
- `ba-click-fx-desktop-0.2.1-slim-test-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.1-slim-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.1-slim-setup-windows-x64.exe.sha256`

标准版包含 Spout2 发送器，可按 [`OBS_SPOUT2.md`](OBS_SPOUT2.md) 接入 OBS。Slim 版不包含
Spout2 依赖，Control Center 也不显示 Spout2 控件；两者的 Host、配置和基础特效一致。
便携版没有 Package Identity，因此不承诺无边框 WGC。

## 安装、升级和 SmartScreen

从 0.2.0 或早期 Alpha 版本升级会保留现有配置和 `data` 目录。当前产品配置使用
`schemaVersion=17`；不匹配的配置仍保留原文件，Host 使用内存默认值安全运行。

安装器使用目标机生成的自签名证书，Windows SmartScreen 可能显示 `Unknown Publisher`。
这是当前发布渠道的预期提示；Release 不附带可单独安装的证书、MSIX、私钥或 SDK 工具。

## 支持边界

0.2.1 仍只承诺 Windows 10/11 x64、单主屏 SDR 下的现有能力。本补丁只为 HDMI
拓扑变化导致的内置主屏 WGC 会话停止增加有界恢复，不表示多显示器或热插拔矩阵
已经完成硬件验收。

HDR/Advanced Color、多显示器、混合 DPI/刷新率、跨适配器、ROI、真实 GPU device lost、
外部录屏兼容性、无边框 WGC 的跨版本稳定性和完整热插拔矩阵仍为实验项或
`Not Run`，不属于本版支持声明。

## 验证说明

自动化测试覆盖拓扑通知与 `session-stopped` 的时序、恢复窗口过期以及硬件、供电、
捕获请求和 WGC 重启安全门。发布前会分别执行标准版与 Slim 版的 Release workflow；
未完成的真实硬件矩阵继续记为 `Not Run`。
