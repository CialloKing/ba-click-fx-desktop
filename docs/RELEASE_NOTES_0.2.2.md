# ba-click-fx-desktop 0.2.2

0.2.2 是针对 HDMI 采集卡已连接时 WGC 背景捕获冷启动失效的正式补丁版
（非 prerelease）。

## 修复内容

部分 HDMI 采集卡会向 Windows 注册活动的 DisplayConfig source，但不会提供可由
`EnumDisplayMonitors` 映射的桌面 `HMONITOR`。Host 启动时，全局显示拓扑因此报告
`incomplete / ERROR_NOT_FOUND`。旧实现只有在全局拓扑完整时才查询颜色状态，导致证据本来
完整的笔记本内置屏也无法取得 SDR white：WGC 帧持续到达，但背景合成为保证颜色合同而
fail-closed，画面只剩 FX-only。重新热插拔采集卡会触发另一轮枚举，所以问题会暂时消失。

本版将全局拓扑质量与逐显示器颜色路径质量分开处理。未映射到桌面显示器的采集端路径继续
保留全局 `incomplete` 状态和错误码，便于诊断；只要内置屏自己的 source 身份和全部物理
target 完整，就允许它独立查询 Advanced Color 与 SDR white 状态。无法归属 source、source
身份不完整或缺少物理 target 时仍会阻断查询，避免在 HDR 或克隆显示场景误用不完整证据。

真实冷启动验证中，全局拓扑保持 `incomplete / ERROR_NOT_FOUND`，内屏颜色快照为 fresh SDR，
WGC 取得 661 帧并完成 419 个背景合成帧，期间没有拓扑恢复重试或会话停止；背景捕获无需
重新热插拔采集卡即可生效。

## 下载哪个包

标准版和 Slim 版都提供便携 ZIP、安装器及各自的 SHA-256 文件：

- `ba-click-fx-desktop-0.2.2-test-windows-x64.zip`
- `ba-click-fx-desktop-0.2.2-test-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.2-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.2-setup-windows-x64.exe.sha256`
- `ba-click-fx-desktop-0.2.2-slim-test-windows-x64.zip`
- `ba-click-fx-desktop-0.2.2-slim-test-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.2-slim-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.2-slim-setup-windows-x64.exe.sha256`

标准版包含 Spout2 发送器，可按 [`OBS_SPOUT2.md`](OBS_SPOUT2.md) 接入 OBS。Slim 版不包含
Spout2 依赖，Control Center 也不显示 Spout2 控件；两者的 Host、配置和基础特效一致。
便携版没有 Package Identity，因此不承诺无边框 WGC。

## 安装、升级和 SmartScreen

从 0.2.0、0.2.1 或早期 Alpha 版本升级会保留现有配置和 `data` 目录。当前产品配置使用
`schemaVersion=17`；不匹配的配置仍保留原文件，Host 使用内存默认值安全运行。

安装器使用目标机生成的自签名证书，Windows SmartScreen 可能显示 `Unknown Publisher`。
这是当前发布渠道的预期提示；Release 不附带可单独安装的证书、MSIX、私钥或 SDK 工具。

## 支持边界

0.2.2 仍只承诺 Windows 10/11 x64、单主屏 SDR 下的现有能力。本补丁只修复采集端的
独立 DisplayConfig 路径阻断内置主屏颜色查询的问题，不表示多显示器或完整热插拔矩阵已经
完成硬件验收。

HDR/Advanced Color、多显示器、混合 DPI/刷新率、跨适配器、ROI、真实 GPU device lost、
外部录屏兼容性、无边框 WGC 的跨版本稳定性和完整热插拔矩阵仍为实验项或 `Not Run`，
不属于本版支持声明。

## 验证说明

自动化测试覆盖全局拓扑不完整但逐屏颜色路径完整，以及逐屏 source、target 或路径证据不完整时
继续拒绝颜色查询。发布前分别执行标准版与 Slim 版的 Release workflow；未完成的真实硬件矩阵
继续记为 `Not Run`。
