# 首个 Alpha 支持范围

## 可以测试的范围

- Windows 10/11 x64，单个主显示器。
- 点击与拖拽特效；窗口自排除成功时启用 WGC 背景采样和差分 Bloom，
  WGC 不可用、启动失败或背景帧过期时自动回退 FX-only。
- D3D11 硬件设备；硬件设备创建失败时尝试 WARP 软件设备。
- 当前验证范围为普通 SDR 桌面合成路径。
- Release 可执行文件静态链接 Visual C++ 运行库；仍使用 Windows 自带的 D3D11、DirectComposition、
  WIC 和 D3DCompiler 系统组件。

直接运行 `ba-click-fx-desktop.exe` 后，窗口保持鼠标穿透。右键通知区域中的程序图标并选择
`Exit` 可退出；也可按 `Ctrl+Alt+F12` 或备用的 `Ctrl+Shift+F12`。即使系统热键注册被占用，
程序仍会轮询同一组合键作为兜底。

## 尚未支持或尚未验证

- HDR、Advanced Color 和物理 nits 输出声明。
- WGC 边框策略、外部录屏兼容性、会话长时间压力与权限拒绝矩阵。
- 多显示器、跨显示器输入、多适配器和混合刷新率。
- device removed/reset 后的原地恢复。
- 托盘设置、开机启动、配置持久化、自动更新和代码签名。

这些能力即使存在实验代码或架构文档，也不属于本 Alpha 的支持合同。

## 测试入口

- `ba-click-fx-desktop.exe --demo-click`：在主屏中心生成一次可见点击后继续运行。
- `ba-click-fx-desktop.exe --smoke-test`：执行有界的 D3D11/DirectComposition 中心像素检查并退出；
  成功退出码为 `0`。
- `ba-click-fx-desktop.exe --quit-after-ms=1000`：运行正常消息/渲染循环并在约一秒后退出，用于验证
  退出清理路径。

smoke 只证明当前 Windows 会话中的基本渲染链路可用。运行日志中的
`Support.WGC=active` 表示本次会话成功创建 WGC 路径；`fallback-fx-only` 表示已安全降级，
两者都不替代 HDR、多显示器、录屏兼容性或其他硬件矩阵验证。
