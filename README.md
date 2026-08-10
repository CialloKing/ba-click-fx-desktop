# ba-click-fx-desktop

`ba-click-fx-desktop` 是从零实现的 Windows 原生桌面点击特效。项目不复用
`ba-click-fx` 的 JavaScript、WebGL 或 WebGPU 渲染代码；Unity/游戏资源是视觉真值，
Web 版本只作为行为与参数语义参考。

Release Host 运行时是单文件：Visual C++ 运行库静态链接，Circle、Grad Ring、Triangle Atlas、Trail
四张原始 PNG 以压缩文本嵌入 C++，启动时只在内存中解码并上传为 sRGB GPU 纹理；材质 HLSL
也嵌入程序。运行不读取 Unity 工程、游戏目录或旁置 shader/图片文件，但仍使用 Windows 自带的
D3D11、DirectComposition、WIC 和 D3DCompiler 系统组件。独立的 WinUI 3 Control Center 使用
Windows App SDK 直接部署文件，测试时必须与这些旁置文件保持在同一目录。

当前架构版本是 **v0.3**，状态为 **Proposed**。首个可运行 Alpha 已具备 Host、WinUI 3
Control Center、本地 IPC 与独立测试包；当前人工特效审核和支持合同仍以单主屏 FX-only/SDR
路径为准。涉及 DirectComposition、Windows Graphics Capture、HDR/Advanced Color 和多适配器的结论，
必须取得仓库中定义的 Spike 证据或接受明确的 fallback 后，相关 ADR 才能标记为 Accepted。

Host 现在会把主显示器 DPI、DXGI 色彩空间、位深和驱动提供的亮度元数据写入支持报告。这些字段只用于
后续 HDR/显示 Spike 的能力证据；`Support.HDR=not-supported` 在完整输出矩阵通过前保持不变，亮度为零时
也会显式标记为未知，而不会把零值解释成显示器真实亮度。

## 冻结的技术方向

- C++20、Win32、D3D11、HLSL、DirectComposition。
- D3D11 immediate context 只由 Render Owner 使用。
- 一个 DXGI Adapter 定义一个资源域；跨适配器资源不隐式共享。
- Windows Graphics Capture 是可选的 Background Sensor，不是基础点击特效的依赖。
- 未来背景交互采用非负的 Background-aware Differential Bloom；它不是本 Alpha 的支持承诺。
- 最终透明交换链使用 FP16 扩展预乘输出；普通 SDR 下不得承诺白底仍有加法余量。
- 三角碎片保持 crisp-only，可保留未模糊的 HDR 核心，但 `BloomSeed=0`，不产生模糊光晕。

## 文档入口

- [ARCHITECTURE.md](ARCHITECTURE.md)：系统边界、模块、线程、数据流和降级规则。
- [docs/adr](docs/adr)：七项渲染核心决策及产品控制面决策。
- [docs/adr/0008-product-control-plane.md](docs/adr/0008-product-control-plane.md)：Host 配置与本地控制面的首个垂直切片。
- [docs/SPIKES.md](docs/SPIKES.md)：四个必须执行的硬件/API Spike。
- [docs/VALIDATION.md](docs/VALIDATION.md)：测试层级、Golden Oracle 和发布门槛。
- [docs/UNITY_REFERENCE.md](docs/UNITY_REFERENCE.md)：游戏解包资源、Unity 重建工程与 Golden 的证据边界。
- [SUPPORT.md](SUPPORT.md)：首个 Alpha 的可测试范围、退出方式和明确排除项。
- [ASSET-MANIFEST.md](ASSET-MANIFEST.md)：进入可执行文件的纹理哈希与再分发边界。
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

下面的命令会先构建 Release Host 和 Release Control Center，再将二者及 Control Center 的
Windows App SDK 直接部署文件、支持文档和逐文件 SHA-256 清单打入 ZIP；脚本完成前会自动运行
包验证。它要求 `cmake.exe` 可用，且能在 `PATH` 或通过 `-MSBuild` 找到 x64 `MSBuild.exe`：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1
```

默认输出到 `artifacts\local\ba-click-fx-desktop-<version>-test-windows-x64.zip`，并在同目录生成
`.sha256` 文件。解压后必须保留目录结构：先启动 `ba-click-fx-desktop.exe`，再启动
`BAFX.ControlCenter.exe`。Control Center 是依赖同目录 Windows App SDK 文件的直接部署应用，不能
单独复制其 EXE 运行。

### 轻量视觉审核包

如果只需要审核点击和拖尾画面，不需要 WinUI 3 控制面，请使用 Host-only 包入口：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-host-review-bundle.ps1
```

脚本复用 CPack 的四文件安装合同，并将经过验证的 ZIP 放到
`artifacts\local\host-visual-review\<commit>\`。该包只包含 Host、许可证、支持说明和内嵌资产清单，
通常约 0.4 MB；上面的完整测试包则包含 Control Center 的 Windows App SDK 旁置运行时，体积较大是预期的。

## Host 控制面

首个产品化垂直切片已经接入版本化配置和本地 Named Pipe。首次启动会在
`%LOCALAPPDATA%\BAFX\config.json` 创建 schema 3 默认配置；Host 使用
`Local\BAFX.Host.v1` 互斥体保证单实例。

首次生成的配置将 `background.mode` 设为 `fx-only`。`background-aware` 和
`recording-compatible` 需要用户显式选择；WGC 或录屏路径失败时 Host 继续使用 FX-only。
portable EXE 不带 package identity，因此不会把无边框捕获 capability 伪装成已支持；WGC
只有在运行时边框/光标排除接口都可用时才会进入 active 状态。

`BAFX.ControlCenter.exe` 已作为独立的 WinUI 3 进程接入该 Pipe。Host 保持运行时，Control Center
可以读取状态、暂停或恢复特效，并将下列效果配置在下一帧交给 Host：启用状态、点击特效、鼠标拖尾、
效果大小、拖尾长度、拖尾宽度、Bloom 强度与 Bloom 质量。数值控件会合并连续拖动后的写入，避免为
每个滑块像素都写一次配置。

控制中心也会显示 `fx-only`、`background-aware` 和 `recording-compatible` 三个背景模式。当前默认且
受支持的模式是 `fx-only`；其余模式只表示可选背景捕获配置，尚不构成 WGC、录屏兼容性或 HDR 的支持
声明，失败时 Host 必须保持或回退到 FX-only。

底层协议仍可由 PowerShell 或其他 Named Pipe 客户端验证：

```text
GetState
GetConfig
SetConfig {"generation":1,"path":"effects.globalScale","value":1.25}
Pause
Resume
Shutdown
```

`SetConfig` 也接受完整的 schema 3 JSON 快照。路径补丁只允许配置库声明的产品字段，代次
不匹配会返回 `generation_conflict`；所有命令均在下一帧由 Host 应用。

`embedded_unity_textures` 测试逐张锁定内嵌 PNG 的尺寸、字节数和 SHA-256。只有在更新
Unity 真值快照时才需要运行生成器，生成结果会进入源码而不是成为运行时依赖：

```powershell
pwsh -NoProfile -File tools\generate-embedded-unity-textures.ps1 `
  -UnityProjectRoot "D:\path\to\UnityProject"
```

## 许可证

本项目自行编写的代码使用仓库根目录中的 GNU GPL v2 许可证。内嵌的第三方游戏纹理不因本项目
许可证而获得再许可；本地测试与公开分发边界见 [ASSET-MANIFEST.md](ASSET-MANIFEST.md)。
