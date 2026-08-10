# 首个 Alpha 支持范围

## 可以测试的范围

- Windows 10/11 x64，单个主显示器。
- FX-only 模式下的点击与拖拽特效，以及基于 Unity/游戏资源的当前 D3D11/Bloom 渲染路径。
- `BAFX.ControlCenter.exe` 的 WinUI 3 控制面：启用状态、点击特效、鼠标拖尾、效果大小、拖尾长度、
  拖尾宽度、Bloom 强度和 Bloom 质量会通过本地 Named Pipe 在下一帧应用到正在运行的 Host。
- D3D11 硬件设备；硬件设备创建失败时尝试 WARP 软件设备。
- 当前验证范围为普通 SDR 桌面合成路径。
- 首次生成的 schema 3 配置默认为 `background.mode=fx-only`；背景捕获必须显式启用，
  授权、排除或会话失败时继续使用 FX-only。
- WGC 是可选的背景差分输入，不是点击特效依赖。portable EXE 没有 package identity，
  也不会自行声明 `graphicsCaptureWithoutBorder` capability；只有运行时同时取得
  `GraphicsCaptureSession3`（无系统边框）和 `GraphicsCaptureSession2`（光标排除）时才报告
  `Support.WGC=active`。
- Release 可执行文件静态链接 Visual C++ 运行库；仍使用 Windows 自带的 D3D11、DirectComposition、
  WIC 和 D3DCompiler 系统组件。

直接运行 `ba-click-fx-desktop.exe` 后，窗口保持鼠标穿透。右键通知区域中的程序图标并选择
`Exit` 可退出；也可按 `Ctrl+Alt+F12` 或备用的 `Ctrl+Shift+F12`。即使系统热键注册被占用，
程序仍会轮询同一组合键作为兜底。需要调整效果时，先启动 Host，再从同一目录启动
`BAFX.ControlCenter.exe`；Control Center 与 Host 是独立进程，关闭控制窗口不会停止 Host。

可用下列命令生成完整测试包。脚本会构建 Release Host 与 Control Center，并验证 ZIP 中的文件清单、
校验和、可执行文件依赖和 Control Center 启动；输出包位于
`artifacts\local\ba-click-fx-desktop-<version>-test-windows-x64.zip`：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1
```

解压测试包时必须保留其完整目录结构。`BAFX.ControlCenter.exe` 依赖旁置的 Windows App SDK 资源和 DLL，
不能脱离该目录单独运行。

## 尚未支持或尚未验证

- HDR、Advanced Color 和物理 nits 输出声明。
- WGC 背景感知的边框策略、外部录屏兼容性、会话长时间压力与权限拒绝矩阵。Control Center 中的
  `background-aware` 与 `recording-compatible` 选项仍是显式实验入口；本 Alpha 不将它们作为
  可依赖的效果路径，出现失败或帧过期时应维持或回退 FX-only。`recording-compatible` 始终关闭
  WGC 并撤销窗口捕获排除，以便录屏器有机会看到 overlay，但不保证任意录屏器都能捕获。
  `background-aware` 启动或会话中止后也会撤销窗口捕获排除，避免 FX-only 回退被录屏器隐藏。
- 当前 portable 包在 Windows 10 19045 的实测结果为 `Support.WGC=fallback-fx-only`，原因是
  `GraphicsCaptureSession::QueryInterface(IGraphicsCaptureSession3)` 返回 `0x80004002`
  (`E_NOINTERFACE`)。这表示本机缺少无边框接口，不表示 D3D11 或 FX-only 渲染失败。需要
  package identity/capability 的 MSIX 实验和 Windows 11/Server 矩阵验证后，才能放宽该策略。
- 多显示器、跨显示器输入、多适配器和混合刷新率。
- device removed/reset 后的原地恢复。
- 开机启动、自动更新、安装程序和代码签名。

这些能力即使存在实验代码或架构文档，也不属于本 Alpha 的支持合同。

## 测试入口

- `ba-click-fx-desktop.exe --demo-click`：在主屏中心生成一次可见点击后继续运行。
- `ba-click-fx-desktop.exe --smoke-test`：执行有界的 D3D11/DirectComposition 中心像素检查并退出；
  成功退出码为 `0`。
- `ba-click-fx-desktop.exe --quit-after-ms=1000`：运行正常消息/渲染循环并在约一秒后退出，用于验证
  退出清理路径。
- `BAFX.ControlCenter.exe`：在 Host 已运行时打开 WinUI 3 设置窗口，通过本地 IPC 读取并调整当前
  FX-only 特效参数；它不是独立渲染器。

smoke 只证明当前 Windows 会话中的基本渲染链路可用。运行日志中的
`Support.WGC=active` 只表示本次会话成功创建了 WGC 路径；`fallback-fx-only` 表示已安全降级。
两者都不替代 FX-only 人工视觉审核，更不替代 HDR、多显示器、录屏兼容性或其他硬件矩阵验证。
