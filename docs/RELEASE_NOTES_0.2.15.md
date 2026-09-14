# ba-click-fx-desktop 0.2.15

使用更新后的 Windows SDK 重新构建的维护版本。

## 构建更新

- 发行包使用 Visual Studio 2026 与 Windows SDK `10.0.28000.0`（补丁版 `28000.2526`）构建。
- Full 与 Slim 已通过新 SDK 下的 C++/WinRT 编译探测、Release 编译和链接验证。
- Full 的 45 项、Slim 的 44 项现有测试全部通过。

## 使用与兼容性

录屏兼容模式继续标为“测试，仅 Windows 11 26H2 及以后”，最低 OS build 为 `26300`。
程序仍会检查实际 WGC Session 能力、窗口排除列表与 configuration iteration，并在不可用时回退。
26H2／OBS 的实机录制组合仍待验收；更新构建 SDK 不代表该组合已验证通过。

本次更新不改变捕获或渲染逻辑，主配置 schema 20、IPC 和特效预设格式保持不变。
用户无需安装开发 SDK；请使用完整安装器或便携包，同时更新 Host 与控制中心。

## English

A maintenance release rebuilt with Visual Studio 2026 and Windows SDK `10.0.28000.0` (servicing version `28000.2526`).

- Full and Slim pass C++/WinRT compilation probes, Release builds and all existing tests: 45/45 for Full and 44/44 for Slim.
- Recording compatibility remains a test mode for Windows 11 26H2 and later (minimum OS build `26300`), with runtime WGC capability checks and fallback behavior.
- The 26H2/OBS recording combination still requires validation on a target system. The SDK rebuild does not establish recording compatibility.
- Capture and rendering logic, configuration schema 20, IPC and effect profile format are unchanged. No development SDK is required on users' computers; update Host and Control Center together.

## 发布资产

提供 Full 版安装器、便携 ZIP 及各自的 SHA-256 校验文件。Slim 保留源码构建与本地验证，不上传预编译资产。
