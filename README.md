# ba-click-fx-desktop

`ba-click-fx-desktop` 是从零实现的 Windows 原生桌面点击特效。项目不复用
`ba-click-fx` 的 JavaScript、WebGL 或 WebGPU 渲染代码；Unity/游戏资源是视觉真值，
Web 版本只作为行为与参数语义参考。

Release Host 运行时是单文件：Visual C++ 运行库静态链接，Circle、Grad Ring、Triangle Atlas、Trail
四张参考纹理的 RGBA8 texel 以 raw LZ4 Block 无损压缩为 C 字节串，直接编译进 EXE。启动时逐张分配
无需预清零的输出缓冲区，执行一次有界解压并直接创建 D3D11 immutable sRGB 纹理；上传返回后立即
释放 CPU texel。该路径没有 Base64、PNG 容器、WIC 解码或临时图片文件。材质 HLSL 也嵌入程序；
运行不读取 Unity 工程、游戏目录或旁置 shader/图片文件，只使用 Windows 自带的 D3D11、
DirectComposition 和 D3DCompiler 系统组件。独立的 Control Center 使用纯 Win32 Common Controls，
不需要 Windows App SDK 或其他旁置运行时。

当前架构版本是 **v0.3**，状态为 **Proposed**。首个可运行 Alpha 已具备 Host、原生 Win32
Control Center、本地 IPC 与独立测试包；当前人工特效审核和支持合同以单主屏 SDR 下的三种渲染模式
为准。涉及 DirectComposition、Windows Graphics Capture、HDR/Advanced Color 和多适配器的结论，
必须取得仓库中定义的 Spike 证据或接受明确的 fallback 后，相关 ADR 才能标记为 Accepted。

Host 现在会把主显示器 DPI、DXGI 色彩空间、位深和驱动提供的亮度元数据写入支持报告。这些字段只用于
后续 HDR/显示 Spike 的能力证据；`Support.HDR=not-supported` 在完整输出矩阵通过前保持不变，亮度为零时
也会显式标记为未知，而不会把零值解释成显示器真实亮度。

## 冻结的技术方向

- C++20、Win32、D3D11、HLSL、DirectComposition。
- D3D11 immediate context 只由 Render Owner 使用。
- 一个 DXGI Adapter 定义一个资源域；跨适配器资源不隐式共享。
- Windows Graphics Capture 只由 `background-aware` 模式使用；它不是基础点击特效的硬依赖，失败时
  回退到 `classic`。
- `background-aware`、`classic`、`light-background` 是唯一的产品渲染模式。前者使用 WGC 和完整的
  Differential Bloom；`classic` 使用现有 FX-only coverage transport；`light-background` 使用
  `visual-max`、`bright-core` 和 `0.85` 的桌面 Alpha 上限。后两者关闭 WGC。
- 最终透明交换链使用 FP16 扩展预乘输出；普通 SDR 下不得承诺白底仍有加法余量。
- 三角碎片保持 crisp-only，可保留未模糊的 HDR 核心，但 `BloomSeed=0`，不产生模糊光晕。

桌面版的 DirectComposition overlay 没有浏览器 `Screen` API 的逐像素等价物。只有
`background-aware` 在 WGC 样本有效时能把异步桌面纹理带入合成；`classic` 和 `light-background`
是没有捕获背景时的确定性传输策略，不能宣称对任意桌面像素逐点还原。

## 文档入口

- [ARCHITECTURE.md](ARCHITECTURE.md)：系统边界、模块、线程、数据流和降级规则。
- [docs/adr](docs/adr)：七项渲染核心决策及产品控制面决策。
- [docs/adr/0008-product-control-plane.md](docs/adr/0008-product-control-plane.md)：Host 配置与本地控制面的首个垂直切片。
- [docs/SPIKES.md](docs/SPIKES.md)：四个必须执行的硬件/API Spike。
- [docs/VALIDATION.md](docs/VALIDATION.md)：测试层级、Golden Oracle 和发布门槛。
- [docs/UNITY_REFERENCE.md](docs/UNITY_REFERENCE.md)：游戏解包资源、Unity 重建工程与 Golden 的证据边界。
- [SUPPORT.md](SUPPORT.md)：首个 Alpha 的可测试范围、退出方式和明确排除项。
- [tools/package-test-bundle.ps1](tools/package-test-bundle.ps1)：构建并生成可解压测试包，同时调用完整性验证。

## 项目状态

仓库已经进入首个可运行版本的人工审核阶段。文档中的 `Proposed`、`Verified` 和
`Accepted` 是严格状态，不代表完成百分比：没有证据的能力不会因为代码路径存在、配置项存在或
日志显示为可用而被宣称支持。

本地可用下列命令核对 Unity 外部证据。脚本只读取并校验哈希，不会复制或修改游戏资产：

```powershell
cmake --build build\vs2026 --target verify_unity_reference
```

## 构建与测试

首版固定使用 C++20；本机验证工具链为 Visual Studio 2026 与 Windows SDK 10.0.26100。推荐使用
仓库预设完成全新 Release 配置、构建和测试：

```powershell
cmake --workflow --preset alpha-release-verify
```

DirectComposition smoke test 需要交互式桌面，因此默认不进入普通 CTest：

```powershell
cmake --build --preset alpha-debug --target smoke_desktop
```

启用 `BAFX_ENABLE_DESKTOP_SMOKE_TESTS=ON` 时，smoke 会生成一次确定性中心点击，实际经过
Unity 材质 shader、MRT、FP16 预乘交换链和 DirectComposition present。单独查看效果可运行：

```powershell
build\alpha-x64\src\desktop\Debug\ba-click-fx-desktop.exe --demo-click
```

Overlay 不抢焦点且保持鼠标穿透；右键通知区域图标可退出，也可按 `Ctrl+Alt+F12` 或备用的
`Ctrl+Shift+F12`。即使组合键已被其他软件注册，轮询兜底仍会识别它。当前 smoke 仍不等同于
HDR、跨适配器或 WGC 的 Spike 已通过。

### 独立测试包

下面的命令会先构建 Release Host 和原生 Win32 Control Center，再将两个 EXE、支持文档和逐文件
SHA-256 清单打入 ZIP；脚本完成前会自动运行包验证。它只要求 `cmake.exe` 可用：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1
```

默认输出到 `artifacts\local\ba-click-fx-desktop-<version>-test-windows-x64.zip`，并在同目录生成
`.sha256` 文件。解压后必须保留目录结构：先启动 `ba-click-fx-desktop.exe`，再启动
`BAFX.ControlCenter.exe`。Control Center 只依赖 Windows 自带的 User32、Comctl32 和配置 IPC，
可以直接复制该 EXE 运行，但需要与 Host 放在同一目录才能使用“启动 Host”按钮。连接后同一按钮会
切换为“关闭 Host”，通过 IPC 请求正常退出，并等待 Host 的单实例生命周期真正结束后才允许再次启动。

### 轻量视觉审核包

如果只需要审核点击和拖尾画面，不需要控制面，请使用 Host-only 包入口：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-host-review-bundle.ps1
```

脚本复用 CPack 的三文件安装合同，并将经过验证的 ZIP 放到
`artifacts\local\host-visual-review\<commit>\`。该包只包含 Host、许可证和支持说明，
通常约 0.4 MB；完整测试包现在也只包含两个原生 EXE，压缩后约 0.7 MB，不再携带 Windows App SDK
旁置运行时。

### 普通用户安装器

Release 页面提供单文件 `*-setup-windows-x64.exe` 安装器。普通用户不需要安装
Visual Studio、Windows SDK、Inno Setup 或 PowerShell 依赖包；安装器已经包含 Host、Control Center、
Sparse Package、公钥证书和安装脚本。

1. 从 Release 下载与系统匹配的 `*-setup-windows-x64.exe`，同时下载同名 `.sha256`，按页面提供的哈希校验文件。
2. 双击安装器并确认一次 UAC。安装器会把程序放到 `Program Files`，为当前用户注册方案 C Package Identity，
   然后打开 Control Center。
3. 在 Control Center 中点击“启动 Host”，再按需选择“背景感知”“贴近原版”或“浅色背景优化”。
   关闭 Control Center 不会停止 Host；可以从通知区域退出 Host。
4. 卸载时使用开始菜单中的卸载项或 Windows“已安装的应用”。默认保留程序目录下的 `data` 配置，重新安装
   后仍可继续使用；如需彻底清理，请在卸载前手动备份并删除该目录。

该安装器使用目标机生成的本机证书为 Sparse Package 签名，不是公有代码签名。Windows SmartScreen 可能显示
“Unknown Publisher”，这是预期提示；不要从 Release 单独下载或安装证书、MSIX、私钥或 SDK 工具。若没有管理员权限，
请改用上面的 portable 测试 ZIP，直接解压运行即可，但 portable 没有 Package Identity，无法承诺无边框 WGC。

## Host 控制面

首个产品化垂直切片已经接入版本化配置和本地 Named Pipe。首次启动会在主程序
`ba-click-fx-desktop.exe` 同目录创建 `BAFX.config.json` 和
`ba-click-fx-desktop-support.log`；支持报告也会被限制在该目录。运行时不再使用
`%LOCALAPPDATA%`、当前工作目录或其他用户目录保存数据。Host 使用
`Local\BAFX.Host.v1` 互斥体保证单实例。

首次生成的 schema 7 配置将 `background.mode` 设为 `background-aware`，并将
`background.allowSystemBorder` 设为 `true`；schema 1/2/3 配置迁移到当前版本时采用该值，schema 4
迁移时缺失字段也使用该默认值，以便保留旧系统上的背景感知路径；schema 4 中已经显式保存的
`false` 会原样保留。schema 5 虽然序列化了尚未接线的 `trailOnlyWhilePressed=false`，实际行为始终要求
按住鼠标；迁移到 schema 6 时会归一为 `true`，避免升级后意外开启拖尾常驻。schema 6 迁移到
schema 7 时新增 `input.samplingRateHz=0`，保持此前不限频的输入行为。只有
`background-aware` 会启用 WGC；WGC 或捕获排除路径失败时，Host 将当前批次回退到 `classic` 的
FX-only coverage transport，支持报告仍记为 `fallback-fx-only`。
portable EXE 不带 package identity，因此不会把无边框捕获 capability 伪装成已支持。允许系统边框时，
WGC 可以在 Windows 要求隐私提示的情况下启动；可见边框会在日志中记为
`system-border=visible-allowed`。用户可以在 Control Center 中取消勾选“允许黄色捕获边框”；此后 Host
会在 `StartCapture` 前请求并确认无边框会话，接口缺失、权限不足或 Windows 仍要求系统边框时直接回退
FX-only，不会先启动带黄色边框的会话。无论该开关如何设置，Overlay 的跨进程鼠标穿透都具有更高
优先级，任何自排除冲突都必须回退 FX-only。

`BAFX.ControlCenter.exe` 已作为独立的 Win32 进程接入该 Pipe。Host 保持运行时，Control Center
可以读取状态、暂停或恢复特效，并将下列效果配置在下一帧交给 Host：启用状态、点击特效、鼠标拖尾、
拖尾常驻、效果大小、拖尾长度、拖尾宽度、输入采样率上限、Bloom 强度与 Bloom 质量。“拖尾常驻”默认关闭；开启后
无需按住鼠标，普通移动也会生成纯拖尾，但不会伪造点击圆盘或圆环。数值控件会合并连续拖动后的写入，避免为
每个滑块像素都写一次配置。

“输入采样率上限 (Hz)”默认值 `0` 表示保留全部 Raw Input Move；`1..1000` 使用每个样本自己的 QPC
时间戳限频，不受渲染帧率或暂停后的模拟时间影响。`30 Hz` 是参考 Web 版提供的手机客户端视觉近似，
`15 Hz` 折线更明显，`60 Hz` 更平滑；这些是人工审核入口，不是从 Prefab 提取出的固定客户端帧率。
限频只丢弃过密 Move，按下、抬起和取消不会被延迟，也不会修改 Unity TrailRenderer 的
`m_MinVertexDistance=0.01`、`time=0.3` 或 `widthMultiplier=0.005`。

控制中心也会显示三个渲染模式（显示名依次为“背景感知”“贴近原版”“浅色背景优化”），以及
“允许黄色捕获边框”复选框。它们对应的 wire values 分别为 `background-aware`、`classic`、
`light-background`；切换到后两项会关闭 WGC。新配置默认请求 `background-aware` 并允许 Windows
显示捕获边框；用户可取消勾选以要求无边框捕获。这不构成 WGC、录屏兼容性或 HDR 的支持声明；
关闭边框后若无边框 WGC 无法安全建立，Host 必须保持或回退到 `classic`。

底层协议仍可由 PowerShell 或其他 Named Pipe 客户端验证：

```text
GetState
GetConfig
SetConfig {"generation":1,"path":"effects.globalScale","value":1.25}
SetConfig {"generation":1,"path":"input.trailOnlyWhilePressed","value":false}
SetConfig {"generation":1,"path":"input.samplingRateHz","value":30}
SetConfig {"generation":1,"path":"background.mode","value":"background-aware"}
SetConfig {"generation":1,"path":"background.mode","value":"classic"}
SetConfig {"generation":1,"path":"background.mode","value":"light-background"}
SetConfig {"generation":1,"path":"background.allowSystemBorder","value":false}
Pause
Resume
Shutdown
```

`SetConfig` 也接受完整的 schema 7 JSON 快照。路径补丁只允许配置库声明的产品字段，代次
不匹配会返回 `generation_conflict`；所有命令均在下一帧由 Host 应用。

`packed_fx_textures` 测试逐张解压 raw LZ4 Block，并锁定 RGBA8 texel 的尺寸、行距和 SHA-256。
生成器是仅供维护者使用的开发工具；只有在更新 Unity 真值快照时才需要运行，输入 PNG、Node.js 和
Unity 工程都不是构建产物或运行时依赖：

```powershell
node tools\generate-packed-fx-textures.mjs `
  --project "D:\path\to\UnityProject"
```


## 开发说明

本项目主要通过 AI 生成和迭代完成（**绝无手写代码**），并经过实际运行测试、参数调校和效果校准。项目目标是尽可能还原《蔚蓝档案》风格的点击特效与拖尾轨迹。

## 许可证

GNU GPL v2 许可证。
