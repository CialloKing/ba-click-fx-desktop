# 首个 Alpha 支持范围

## 可以测试的范围

- Windows 10/11 x64，单个主显示器。
- 三种渲染模式下的点击与拖拽特效，以及基于 Unity/游戏资源的当前 D3D11/Bloom 渲染路径：
  `background-aware`（背景感知）、`recording-compatible`（录屏兼容拟合）和
  `light-background`（浅色背景优化）。背景感知启用 WGC，失败时回退内部 FX-only transport；
  其余两项关闭 WGC。
- `BAFX.ControlCenter.exe` 的原生 Win32 控制面：启用状态、点击特效、鼠标拖尾、拖尾常驻、效果大小、拖尾长度、
  拖尾宽度、输入采样率上限、Bloom 强度和 Bloom 质量会通过本地 Named Pipe 在下一帧应用到正在运行的 Host；
  “重置默认”经确认后恢复全部持久化设置，但保留当前暂停或运行状态。
- 每次输入消费/呈现更新只为按压 FX 使用一份帧边界当前位置，并以同一 `renderTime` 按
  Down→Held→Up 处理普通路径；普通 Up-only 释放帧的 Held 为 false，不应用该帧的按住移动。输入采样率 `0` 不额外
  限频；`1..1000 Hz` 只按消息分派 QPC 推进可选位置采样相位，不影响边沿或模拟时间。
- Raw Input 的同帧多边沿仍按原序无损保留，仅用于诊断和 native 扩展；严格效果路径将其归约为
  Down/Held/Up 布尔帧态并按 Down→Held→Up 执行，Cancel 最后作为 native 硬边界处理。Unity
  `2021.3.45f1` Player 已确认 `Down-Up-Down` 的聚合帧三态同时为 true；其他边沿排列及游戏所用
  Unity `2021.3.56f2` 仍未验证。含任一边沿的帧不会从尾随 Move 重启常驻拖尾。
- 拖尾常驻是 native/Web 增强，不生成点击 burst。Unity TrailRenderer 的空间参数仍保持
  `m_MinVertexDistance=0.01`。
- D3D11 硬件设备；硬件设备创建失败时尝试 WARP 软件设备。
- 当前验证范围为普通 SDR 桌面合成路径。
- 支持报告会记录主屏 DPI、DXGI 色彩空间、位深和亮度元数据；这些只是当前输出快照，不能据此
  宣称 HDR、Advanced Color 或物理 nits 输出已经受支持。驱动未提供有效亮度时会记录
  `luminance-unknown`。
- 首次生成的 schema 7 配置默认为 `background.mode=background-aware`，同时设置
  `background.allowSystemBorder=true`。schema 1/2/3 迁移时默认允许系统边框；schema 4 迁移时缺失字段
  也使用该默认值，但已有 schema 4 中显式保存的 `false` 会原样保留。背景感知授权、排除或会话失败时
  回退内部 FX-only transport；其余模式不启用 WGC。schema 5 迁移会把当时未接线的拖尾按键策略归一为
  “仅按住时”，因此新增的“拖尾常驻”默认关闭，必须由用户显式开启。schema 6 迁移会新增
  `input.samplingRateHz=0`，保持不施加额外时间限频的输入行为。
- 运行时用户数据采用 portable 规则：`BAFX.config.json`、`ba-click-fx-desktop-support.log`
  和支持报告只写入对应 EXE 所在目录。命令行支持报告即使传入绝对路径，也只采用文件名，
  不会写入 `%LOCALAPPDATA%`、当前工作目录或其他用户目录。
- WGC 只由 `background-aware` 模式使用。portable EXE 没有 package identity，也不会自行声明
  `graphicsCaptureWithoutBorder` capability。新配置默认允许 Windows 显示捕获边框；可见边框状态记录为
  `system-border=visible-allowed`。用户可在 Control Center 中取消勾选“允许黄色捕获边框”；关闭后会在
  `StartCapture` 前确认无边框会话，接口缺失、权限不足或系统仍要求边框时直接报告
  `Support.WGC=fallback-fx-only`，并把当前渲染批次回退到内部 FX-only transport，不会先启动带黄色
  边框的会话。切换到 `recording-compatible` 或 `light-background` 会关闭 WGC。日志中的
  `WGC background sample entered the final desktop composite` 才表示背景样本已经进入最终 pass。
- RecordingCompatible 按 Web 版截图的透明覆盖层、`visual-max`、`bright-core`、`0.90` Alpha 上限、
  `source-over` 和未知透明背景设置拟合；LightBackground 使用同一策略，但将 Alpha 上限收紧为
  `0.85`。原生 DirectComposition 没有 DOM 背景表面的逐像素等价物，因此这两种模式都不读取桌面，
  也不是任意桌面像素的逐点捕获。
- Release Host 静态链接 Visual C++ 运行库；仍使用 Windows 自带的 D3D11、DirectComposition 和
  D3DCompiler 系统组件。四张纹理以 raw LZ4 字节编译进 EXE，运行时不读取图片，也不使用 WIC；
  仅开发用的 GPU 捕获工具使用 WIC 写出验证 PNG。

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

解压测试包时必须保留其完整目录结构。Control Center 不携带 Windows App SDK 运行时，只有在需要
通过按钮启动 Host 时才要求与 Host EXE 位于同一目录。

## 普通用户安装

Release 页面中的 `*-setup-windows-x64.exe` 是面向普通用户的单文件安装器。运行时不需要 Windows SDK、Visual
Studio、Inno Setup 或旁置 Windows App SDK；安装器会在一次 UAC 确认后完成程序文件部署、当前用户的 Sparse
Package 注册和 Control Center 快捷方式创建。安装完成后打开 Control Center，点击“启动 Host”即可开始使用。

安装器使用目标机生成的本机证书签名 Package，因此 SmartScreen 可能显示“Unknown Publisher”。Release 不提供
可单独安装的证书、MSIX、私钥或 SDK 工具；Setup 内部携带的是未签名模板和约束到 `LocalMachine\My` 的原生
签名器。公钥只导入 `LocalMachine\TrustedPeople`，签名后使用 `-DeleteKey` 删除私钥。卸载可从开始菜单或
Windows“已安装的应用”执行，默认保留安装目录
下的 `data` 用户配置；需要无管理员权限时可改用 portable ZIP，但它没有 Package Identity。

## 尚未支持或尚未验证

- HDR、Advanced Color 和物理 nits 输出声明。
- WGC 背景感知的外部录屏兼容性、会话长时间压力与权限拒绝矩阵。Control Center 中三种模式和
  “允许黄色捕获边框”仍是实验入口；本 Alpha 不将 WGC、录屏兼容性或 HDR 作为可依赖的效果路径。
  `background-aware` 启动失败或会话中止后回退内部 FX-only transport，并撤销窗口捕获排除，避免
  回退画面被录屏器隐藏；`recording-compatible` 和 `light-background` 始终关闭 WGC。
- 当前 Windows 10 19045 实测中，默认允许可见系统边框时，窗口保留
  `WS_EX_LAYERED | WS_EX_TRANSPARENT`，请求 `WDA_EXCLUDEFROMCAPTURE` 后查询值为 `0x11`，
  WGC 会话可正常取帧，光标排除成功，426 个渲染帧中有 423 个背景合成帧。该数据不证明无边框能力；
  在 Control Center 中关闭黄色边框后，该系统因边框隐藏接口不可用而在 `StartCapture` 前回退
  FX-only。已有 schema 4 配置若显式保存了 `false`，迁移到当前 schema 7 后仍保持该关闭状态。
- 无论 WGC 是否可用，都不能移除 Layered/Transparent 样式来换取背景采样；这会破坏跨进程按钮点击。
- 多显示器、跨显示器输入、多适配器和混合刷新率。
- device removed/reset 后的原地恢复。
- 开机启动、自动更新、公有代码签名，以及无边框 WGC 的跨版本稳定性。方案 C 安装器已经作为普通用户发布
  通道提供，但其背景感知能力仍受 Windows 版本、权限和显卡环境影响；portable ZIP 继续作为无安装权限的备选。

这些能力即使存在实验代码或架构文档，也不属于本 Alpha 的支持合同。

## 测试入口

- `ba-click-fx-desktop.exe --demo-click`：在主屏中心生成一次可见点击后继续运行。
- `ba-click-fx-desktop.exe --smoke-test`：执行有界的 D3D11/DirectComposition 中心像素检查并退出；
  成功退出码为 `0`。
- `ba-click-fx-desktop.exe --quit-after-ms=1000`：运行正常消息/渲染循环并在约一秒后退出，用于验证
  退出清理路径。
- `BAFX.ControlCenter.exe`：在 Host 已运行时打开 Win32 设置窗口，通过本地 IPC 读取并调整三种
  渲染模式及 FX 参数；它不是独立渲染器。

smoke 只证明当前 Windows 会话中的基本渲染链路可用。运行日志中的
`Support.WGC=active` 表示本次背景感知会话成功创建了 WGC 路径；随后出现背景合成日志才表示样本已
参与；`fallback-fx-only` 表示已安全降级到内部 FX-only transport。RecordingCompatible 和
LightBackground 不创建 WGC，但仍可进行人工视觉审核；这些状态都不替代 HDR、多显示器、
录屏兼容性或其他硬件矩阵验证。
