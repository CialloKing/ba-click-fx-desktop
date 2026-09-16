# 开发指南

[返回 README](../README.md) · [English guide](DEVELOPMENT.en.md)

以下命令均从仓库根目录执行。诊断字段与支持边界见 [SUPPORT.md](../SUPPORT.md)，
渲染细节见 [ARCHITECTURE.md](../ARCHITECTURE.md)，安装事务见 [ADR-0009](adr/0009-identity-installer-scheme-c.md)。

## 构建与测试

### 源码构建前置条件

源码构建面向 Windows x64。请先准备以下工具：

- Git。
- Visual Studio 2022（17.x）配合 CMake 3.25+，或 Visual Studio 2026（18.x）配合 CMake 4.2+；
  [VS 2026 生成器从 CMake 4.2 开始提供](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)。
  安装 **Desktop development with C++** 工作负载，并勾选 MSVC x64/x86 生成工具和 Windows 10/11 SDK。
  当前发行构建使用 Visual Studio 2026 与 Windows SDK `10.0.28000.0`（补丁版 `28000.2526`）。
  Windows SDK 10.0.19041 或更高版本可用于兼容构建；CI 分别检查 19041、22621 和 26100。
  发行构建环境与旧 SDK 兼容检查各自独立，均不代表录屏模式已通过目标硬件验收。
- Windows PowerShell 5.1 或 PowerShell 7。Python 3 不是编译器依赖，但安装后可以启用完整的
  Python 合同测试；Node.js 用于维护 Unity 纹理快照和 Star 历史，普通构建不需要。

从仓库根目录开始：

```powershell
git clone https://github.com/CialloKing/ba-click-fx-desktop.git
cd ba-click-fx-desktop
```

建议从 **Developer PowerShell for VS** 执行命令，或确认 `cmake.exe`、MSVC 和 Windows SDK
已经在当前终端可用。可以先检查：

```powershell
cmake --version
git --version
```

### Full 版（包含 Spout2）

普通 `x64` 预设启用 Spout2。第一次构建前需要准备独立的 vcpkg checkout；仓库只提供
`vcpkg.json` manifest，不会把 vcpkg 本身提交进来：

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& C:\dev\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
```

如果已经有 vcpkg，只需将 `VCPKG_ROOT` 设置为该 checkout 的绝对路径。Full 配置使用 manifest
模式，根据固定 builtin baseline `a0400024711b283056538ac19ced80b91a83c24c` 安装
`vcpkg.json` 中锁定的 `spout2[dx]` `2.007.010#0`，目标 triplet 为
`x64-windows-static`，依赖会放在 `build\x64\vcpkg_installed`。第一次配置需要联网下载和构建
依赖；之后可以复用本地 vcpkg 缓存。不要依赖仓库外的临时 Spout2 压缩包，也不要手动改动
`build\x64\vcpkg_installed`。

准备好 vcpkg 后，执行完整 Release 配置、构建和测试：

```powershell
cmake --workflow --preset release-verify
```

### Slim 版（不包含 Spout2）

如果只需要本地编译、测试或不需要 OBS/Spout2 输出，可以使用 Slim 预设。它不加载 vcpkg
toolchain，也不需要 `VCPKG_ROOT`：

```powershell
cmake --workflow --preset slim-release-verify
```

Slim 仍然编译完整特效、Host 和 Control Center，只隐藏 Spout2 输出开关。Full 和 Slim 使用
不同的构建目录（分别为 `build\x64` 与 `build\x64-slim`），不要在同一目录之间切换预设。

日常只验证桌面 Host 时使用按目标构建，避免触发包含全部测试和 Spike 的 `ALL_BUILD`：

```powershell
cmake --build --preset host-release --parallel 4
```

该命令使用已经配置好的 Full `x64` 构建树；Slim 构建应改为：

```powershell
cmake --build build\x64-slim --config Release --target ba_click_fx_desktop --parallel 4
```

两个 `*-release-verify` workflow 都保留完整 Release 构建和 CTest 流程，不是快速迭代命令。
配置完成后也可以显式执行：

```powershell
ctest --preset release
ctest --preset slim-release
```

`vcpkg.json` 通过固定 builtin baseline 将 `spout2[dx]` 锁定为 `2.007.010#0`。Full 首次配置
若报告找不到 `vcpkg.cmake`、Spout2 头文件或 `SpoutDX_static.lib`，先确认 `VCPKG_ROOT` 指向
包含 `scripts\buildsystems\vcpkg.cmake` 的 vcpkg checkout，再删除对应的 `build\x64` 配置树
并重新运行 workflow；CMake 会缓存 toolchain 路径，单纯修改环境变量不会修复旧缓存。

如果出现 `No CMAKE_CXX_COMPILER could be found` 或找不到 Windows SDK，请在 Visual Studio
Installer 中补装 C++ 工作负载、MSVC x64 工具和 SDK，然后重新打开 Developer PowerShell。

### 开发构建启动

源码构建的两个程序输出在不同目录。可先分别启动，再由 Control Center 连接已运行的 Host：

```powershell
Start-Process .\build\x64\src\desktop\Release\ba-click-fx-desktop.exe
Start-Process .\build\x64\src\control-center\Release\BAFX.ControlCenter.exe
```

Slim 将路径中的 `x64` 替换为 `x64-slim`。验证“启动 Host”按钮时，使用下文的便携包，
将两个程序保留在同一目录。打包示例使用 PowerShell 7 的 `pwsh`；Windows PowerShell 5.1 可改用 `powershell`。

### 交互式 smoke test

Full 和 Slim 预设都设置 `BAFX_ENABLE_DESKTOP_SMOKE_TESTS=ON`，所以对应的 CTest 流程会注册
需要交互式 Windows 桌面的四个桌面集成测试：smoke、定时退出、设备恢复和帧 pacing stall。请在已登录且未锁屏
的桌面会话中运行 workflow；无桌面或无头 CI 不应把这些测试当作可运行的验收。可以单独重跑
smoke target：

```powershell
cmake --build --preset debug --target smoke_desktop
```

Slim 没有单独的 Debug build preset；如需运行 Slim smoke，请显式配置并构建：

```powershell
cmake --preset x64-slim
cmake --build build\x64-slim --config Debug --target smoke_desktop
```

该 target 运行有界的 `--smoke-test` 中心像素检查，成功退出码为 `0`。视觉演示是持续运行的另一个入口，
不是 smoke test：

```powershell
build\x64\src\desktop\Debug\ba-click-fx-desktop.exe --demo-click
build\x64-slim\src\desktop\Debug\ba-click-fx-desktop.exe --demo-click
```

兼容性 CI 使用 `BUILD_TESTING=OFF` 的普通 CMake 配置，只验证指定 Windows SDK 下的编译，不验证
Full/Spout2，也不代表交互式 smoke、WGC、HDR 或跨适配器能力已经通过。

`.github/workflows/windows-build-compat.yml` 使用 VS2022 以 Windows SDK `10.0.19041.0`、
`10.0.22621.0` 和 `10.0.26100.0` 构建 Host、Control Center 与 Identity Signer 的完整二进制；每个
job 还会记录 runner 实际安装的 Include/Lib SDK 清单。19041 是最低旧 SDK 基线，22621 是中间
Windows 11 SDK，26100 是当前 runner 清单中的最高 SDK。该矩阵只证明编译兼容，不代表 Windows
build `26300+` 的运行时能力或 WGC Session-local exclusion；Windows 11 API 始终采用运行时能力探测，
旧 SDK/Windows 10 构建不能通过裁剪产品目标来规避这些功能。

### 便携版

下面的命令会先构建 Release Host 和原生 Win32 Control Center，再将两个 EXE、支持文档和逐文件
SHA-256 清单打入 ZIP；脚本完成前会自动运行包验证。脚本自行配置/构建时，Full（默认）需要 `cmake.exe`
和已经设置的 `VCPKG_ROOT`；如果没有 vcpkg，请使用 Slim 参数：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1
# 不包含 Spout2，不需要 VCPKG_ROOT
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1 -Slim
```

默认输出到 `artifacts\local\ba-click-fx-desktop-<version>-Portable-windows-x64.zip`，并在同目录生成
`.sha256` 文件。解压后必须保留目录结构：先打开 `BAFX.ControlCenter.exe`，再点击“启动 Host”。Control Center 只依赖 Windows 自带的 User32、Comctl32 和配置 IPC，
可以直接复制该 EXE 运行，但需要与 Host 放在同一目录才能使用“启动 Host”按钮。连接后同一按钮会
切换为“关闭 Host”，通过 IPC 请求正常退出，并等待 Host 的单实例生命周期真正结束后才允许再次启动。
“重置默认”会在确认后一次性恢复其他持久化设置，保留 Host 已保存的快捷键，不改变当前暂停或运行状态。

### 轻量视觉审核包

如果只需要审核点击和拖尾画面，不需要控制面，请使用 Host-only 包入口：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-host-review-bundle.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-host-review-bundle.ps1 -Slim
```

脚本从 Release Host 单独组装三文件审核包，并将经过验证的 ZIP 放到
`artifacts\local\host-visual-review\<commit>\`。该包只包含 Host、许可证和支持说明，
包大小以实际产物为准。完整便携包提供 Host 和 Control Center，不携带 Windows App SDK 旁置运行时。

### 本地安装器

需要生成安装器时，先完成对应的 Full 或 Slim 构建，再安装 Inno Setup 6.3 或更高版本，确保
`ISCC.exe` 在 `PATH` 或标准安装目录中：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-user-installer.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-user-installer.ps1 -Slim
```

也可以通过 `-ISCC <path>` 指定其他 `ISCC.exe`。`-SkipBuild` 只适用于已有对应构建输出；
`-SkipVerification` 是明确的可选绕过，不应作为发布证据。


## Host 控制面

首个产品化垂直切片已经接入版本化配置和本地 Named Pipe。portable Host 会在主程序
`ba-click-fx-desktop.exe` 同目录创建 `BAFX.config.json` 和
`ba-click-fx-desktop-support.log`，首次保存自定义特效 Profile 时再创建 `fx-profiles` 子目录；
Identity 安装版则将这些文件和目录放入同一目录下可写的 `data` 子目录。每个自定义 Profile 对应
`fx-profiles/<名称>.json` 一个文件，内容是完整、平面的 `EffectsConfig` JSON；Host 是这些文件的唯一写入者。
支持报告也会被限制在这棵目录树内。运行时不再使用
`%LOCALAPPDATA%`、当前工作目录或其他用户目录保存数据。Host 使用
`Local\BAFX.Host.v1` 互斥体保证单实例。

首次生成的 schema 20 配置将 `background.mode` 设为 `background-aware`、
`background.allowSystemBorder` 设为 `true`、`display.hdrEnabled` 设为 `false`，并以
`performance.framePacing=match-display`、`input.trailOnlyWhilePressed=true`、`input.samplingRateHz=0`
保持跟随显示器刷新率、按住拖尾且不额外限制输入 Move。显示刷新率缺失或无效时，
`match-display` 保守回退到 60 FPS；只有显式选择 `unlimited` 才不设置额外最小帧周期。
当前配置要求字段完整的显式 `schemaVersion=20`；14 至 19 会按固定顺序迁移到当前版本，
其他版本、缺少 section、未知字段和枚举别名都会被拒绝。Host 记录错误后仅在内存中使用当前默认值，
不会猜测未知格式。只有
`background-aware` 会启用 WGC；WGC 或捕获排除路径失败时，Host 将当前批次回退到内部
FX-only coverage transport，支持报告仍记为 `fallback-fx-only`。这个故障回退不是一个可选的产品模式，
也不会把背景感知配置改写成其他模式。
portable EXE 不带 package identity，因此不会把无边框捕获 capability 伪装成已支持。允许系统边框时，
WGC 可以在 Windows 要求隐私提示的情况下启动；可见边框会在日志中记为
`system-border=visible-allowed`。用户可以在 Control Center 中取消勾选“允许黄色捕获边框”；此后 Host
会在 `StartCapture` 前请求并确认无边框会话，接口缺失、权限不足或 Windows 仍要求系统边框时直接回退
FX-only，不会先启动带黄色边框的会话。无论该开关如何设置，Overlay 的跨进程鼠标穿透都具有更高
优先级，任何自排除冲突都必须回退 FX-only。

`BAFX.ControlCenter.exe` 已作为独立的 Win32 进程接入该 Pipe。Host 保持运行时，Control Center
可以读取状态、暂停或恢复特效。五个顶层页面分别为“基础”“高级”“显示与性能”“快捷键”和“系统”。基础页提供启用状态、
点击特效、鼠标拖尾、拖尾常驻、完整/核心性能模式、效果大小、拖尾长度、拖尾宽度、输入采样率上限、Bloom 强度与 Bloom 质量，
并管理背景模式、指针排除和系统捕获边框；右侧的特效预设区可以选择四个内置 Profile，或按名称应用、
保存和删除自定义 effects-only Profile。高级页再按“时间与透明度”“粒子与材质”
“圆环参数”“点击碎片”“Bloom 参数”“分层开关”分成六个二级页面。分层页可分别隐藏中心圆盘、
圆环、点击碎片、拖尾碎片、拖尾线和 Bloom；关闭 Bloom 会旁路整条 Bloom 金字塔，但保留直接材质。
所有控件只使用项目原生的
`effects.*` 配置路径，例如 `effects.diskRadius`、`effects.diskLifetimeMs`、
`effects.ringsCount`、`effects.ringsLifetimeMs`、`effects.ringsRadiusMin`、
`effects.ringsRadiusMax`、`effects.ringsAngularVelocityMultiplier`、
`effects.ringsRotationDirection`、`effects.shardsClickCount`、`effects.shardsSizeMin`、
`effects.shardsSizeMax` 和 `effects.trailOpacity`。两个碎片尺寸字段由原生模拟统一应用于点击与拖尾碎片。其余两页提供
透明度、点击/拖尾时间倍率、拖尾寿命，以及 Bloom
扩散、阈值、软阈值和亮度上限。“显示与性能”页通过 `GetDisplayState` 选择并查看每个显示会话的
实际边界、DPI、物理/捕获刷新率、DRR、颜色查询、SDR white level、色彩/输出回退、WGC 和故障状态，
并提供默认关闭的全局 HDR 请求、默认关闭的“启用自适应 Active-FX ROI（实验）”，以及
`match-display`、`60`、`120`、`144`、`unlimited` 五种 `performance.framePacing` 策略。具有稳定 DisplayConfig
标识的显示器可以启用独立设置，分别控制特效、HDR 请求和帧率策略；关闭独立设置后恢复继承全局值。
没有稳定标识的会话仍可查看，但逐屏写入控件保持禁用。完整拓扑下，选择器还会列出未连接显示器的
遗留 override；这些条目没有伪造的运行状态，只能通过现有原子命令删除。诊断文本使用可滚动只读区域；
系统页提供“打开日志目录”和“清理诊断日志”：确认清理后分别处理控制中心和 Host 的日志及轮转备份，
显示删除文件数、释放字节数和失败文件数；Host 未连接时只清理控制中心日志。两个进程各保留最多约
32 MiB 日志，详细事件和采样边界见 [SUPPORT.md](../SUPPORT.md)。系统页的“版本与维护”区域分别显示 Control Center、Host、安装状态和
最新公开版本，并提供手动“检查更新”、“打开 Release”和常驻可用的“打开项目仓库”。仓库入口旁会提示
用户可前往项目仓库点 Star。Control Center 的通知区域菜单在 Host 已连接时
可直接暂停或恢复特效；Host 断开时该项置灰。该操作复用现有运行时命令，不写入配置，也不改变下一次
Host 启动时的默认运行状态。Active-FX ROI 工程面板在页面可见时每秒
刷新选中显示器的 primary/recording-rebuild 路径、回退原因、近 5 秒帧数与像素、dirty/aligned rect、
guard/phase、prefilter/downsample/upsample/resolve 分阶段像素和 Prefilter/Pyramid/FinalComposite GPU
p50/p95；离页停止轮询，样本超过 3 秒显示 stale。这些数字描述实际执行路径和测量样本，不会据像素
比例推导 GPU 节省。
选择器刷新后尽量保留同一显示器；状态缺失或解析失败时显示错误，而不会把请求状态显示为实际能力。

录屏兼容诊断区分编译支持与目标系统的实际调用：

- `SessionLocalExclusion.CompileSupport` 来自选中 SDK 的 `WindowId` 集合编译探针，与构建机 OS 版本无关。
- `SessionLocalExclusion.RuntimeProbe` 为 `not-run`、`succeeded` 或 `failed`；成功仅表示会话配置和读回通过。
- `SessionLocalExclusion.RuntimeHRESULT` 仅在有实际调用结果时输出；`FailureStage` 标明失败操作。编译缺失记为
  `compile-time-unavailable`，不报告虚构的运行时 `E_NOINTERFACE`。
- 未执行的 `QI.*`、`WindowId.Get`、`Set`、`Get` 和 iteration 查询输出 `not-run`。实际调用才输出十六进制结果。
- `FrameIterationConfirmed` 单独说明是否收到了配置版本匹配的帧；外部录制是否持续包含特效仍需画面验收。

回退的最终路径日志保留失败的 Session-local 尝试；其后的 active-session 日志描述当前实际会话，可能是
`LegacyGlobalExclusion`。不能把当前 Legacy 会话的 `not-requested` 当成此前没有尝试过 Session-local。

无边框身份日志的 `EffectiveExternalPath.Query` 区分 `not-run`、`api-unavailable`、`query-failed` 和
`succeeded`；`EffectiveExternalPath.ApiModule` 记录 `kernel32.dll`、`kernelbase.dll` 或 `none`。
包信息使用 `PACKAGE_INFORMATION_FULL` 读取发布者。读取成功后仍需通过安装状态、路径、签名和哈希校验，
再由系统决定是否授权无边框捕获；普通包安装路径不能代替系统返回的有效外部路径。

控制中心通过 `UiLanguage`、`TextId` 和编译内置的中英文表翻译界面，动态提示保留文案标识与参数。
“系统 → 系统行为 → 界面语言”独立保存为 `BAFX.ControlCenter.language`，路径复用主配置目录判断。
便携版保存在 EXE 目录，安装版保存在安装目录的 `data` 子目录；内容为 `auto`、`zh-CN` 或 `en-US`，发行包不携带此文件。
重译仅更新控件与缓存状态的显示，保留草稿，不调用 Host 写入或更新检查。

Host 和 Control Center 从 0.2.5 起共享同一产品版本合同。Host 的 `GetState.productVersion` 必须是
与当前 Control Center 完全相同的规范 `MAJOR.MINOR.PATCH`；字段缺失、格式错误或版本不一致时，
控制中心会显示双方版本并禁用设置写入，但“启动 Host”/“关闭 Host”仍可用。这样可以完成混合版本
修复，而不会让旧控制面向新 Host 写入未经确认的配置。

安装状态的显示含义如下：主状态与备份完整成对、属于同一事务且内容相符，产品版本与 Package 版本
一致且匹配 Control Center 时为“安装版”；主安装状态和备份都不存在时为“便携版”；状态损坏、两种
版本冲突、安装版本与控制中心不同，或只剩备份等部分升级情况均为“安装状态异常”。异常状态不会
回退显示为便携版，应重新运行当前版本安装器修复。

“检查更新”只有在用户点击后才查询 GitHub 的最新公开正式 Release；启动、连接 Host、刷新状态和
托盘恢复都不会触发网络请求。它只比较版本，不会自动下载 Release 资产、运行安装器或改写文件。
“打开 Release”始终打开固定的
[官方最新 Release 页面](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest)，
不采用网络响应中的资产 URL 或跳转目标。
“打开项目仓库”不依赖更新检查结果，始终打开固定的
[官方项目仓库](https://github.com/CialloKing/ba-click-fx-desktop)。
渲染配置调整在下一帧交给 Host；快捷键保存则在 IPC 响应前完成注册准备、原子写盘和切换。“拖尾常驻”默认关闭；开启后
无需按住鼠标，普通移动也会生成纯拖尾，但不会伪造点击圆盘或圆环。这是桌面版的原生产品增强，
不属于游戏原脚本的按压 FX 路径。数值控件会合并连续拖动后的写入，避免为每个滑块像素都写一次配置。
`effects.bloomIntensity` 是 Unity Bloom 强度标量，默认值为 `1.7`、有效范围为 `0..10`，不是相对 `1.0` 的倍率。
Bloom 质量只是 diffusion 的派生预设：紧凑、适中、原版、极宽分别对应 `4/6/7/10`，其他值显示为“自定义”。
Active-FX ROI 当前裁剪纯特效 Bloom 的 prefilter 和完整 down/up 金字塔，并为 resolve 生成逻辑有效矩形。
在 steady pure-FX primary 中，经过完整合同验证的 partial final output 只对 `resolveRect` 执行
Context1 `ClearView`、绘制和 `Present1` dirty rect 提交；矩形移动时会合并并清理上一帧可见区。
warmup、background-aware、recording-rebuild、WGC、Spout2 格式转换以及任何回退路径仍执行完整输出和普通
Present。ROI 继续默认关闭并标记为实验性；WARP 和 dirty Present 计数只验证渲染器与路径合同，不证明
DWM 可见结果、功耗收益或跨硬件表现。

Active-FX ROI 旧 capture schema 2/3 与对应报告的 `FAIL` 是历史结果，不追溯改判。新 report schema 4
将重复反映同一同步等待的四项 Frame/Present 条件改为 non-blocking advisory；真实硬件性能与输入延迟
仍为 `Not Run`，因此当前不声明整机提速。采集器仍会在开始及每个 ABBA 块前检查系统空闲度并 fail-closed。

控制中心的“重置默认”按钮会先请求确认，再用内置默认 schema 替换快捷键以外的持久化配置。它保留
Host 已保存的快捷键，也不会恢复已经暂停的特效；需要继续显示时仍应单独点击“恢复特效”。

Host 在每次输入消费/交换链呈现更新中，为按压 FX 锁存一份帧边界当前鼠标位置，并用同一份
`renderTime` 执行本轮模拟动作。普通调用顺序为 Down→Held→Up；普通 Up-only 释放帧的 Held 为 false，
因此不会先把该帧 Move 应用为按住移动再释放。含任一边沿的帧也不会用边沿后的尾随 Move 重启常驻拖尾。Raw Input 的
Down/Up/Cancel 仍按原顺序无损保留，仅用于诊断和 native 扩展；严格效果路径将它们归约为单帧
Down/Held/Up 布尔态，并固定按 Down→Held→Up 执行，Cancel 最后作为 native 硬边界处理。Unity
`2021.3.45f1` Player 黑盒已确认 `Down-Up-Down` 在聚合帧中三态同时为 true；其他边沿排列及游戏使用的
Unity `2021.3.56f2` 仍未验证。没有待消费的位置时不会仅为输入适配而读取光标；按住静止期间由模拟
`advance` 推进距离发射的时间基线。

“输入采样率上限 (Hz)”默认值 `0` 表示不额外限频；`1..1000` 仅使用位置样本的消息分派 QPC 推进
可选输入采样相位。QPC 不决定模拟动作时间、帧态归约、严格路径执行顺序或释放时刻。`30 Hz` 是低功耗视觉
审核预设，`15 Hz` 折线更明显，`60 Hz` 更平滑；这些是人工审核入口，不是从 Prefab 提取出的固定客户端
帧率，也不会修改 Unity TrailRenderer 的 `m_MinVertexDistance=0.01`、`time=0.3` 或
`widthMultiplier=0.005`。

控制中心也会显示三个渲染模式（“背景感知”“录屏兼容（测试，仅 Windows 11 26H2 及以后）”“浅色背景优化”），以及
“允许黄色捕获边框”复选框。它们对应的 wire values 分别为 `background-aware`、
`recording-compatible`、`light-background`。浅色背景优化关闭 WGC；录屏兼容会尝试 WGC 会话级窗口排除，
不可用时依次回退到全局窗口排除和 FX-only，全局排除可能使外部录屏看不到特效。新配置默认请求
`background-aware` 并允许 Windows 显示捕获边框；用户可取消勾选以要求无边框捕获。这不构成 WGC、
录屏兼容性或 HDR 的支持声明；关闭边框后若无边框 WGC 无法安全建立，Host 必须保持或回退到内部
FX-only transport。

“录屏兼容”仅在 Windows 11 26H2 及以后（最低 OS build `26300`）开放测试。
版本检查通过不代表运行时接口可用或录制兼容，仍需验证目标硬件上的最终录制结果。
OBS 可通过 Spout2 接收独立的透明特效层，无需启用此测试模式。

底层协议仍可由 PowerShell 或其他 Named Pipe 客户端验证：

```text
GetState
GetDisplayState
GetConfig
GetFxConfig
GetHotkeyState
BeginHotkeyCapture
EndHotkeyCapture 42
RetryHotkeys
SetConfig {"generation":1,"path":"effects.globalScale","value":1.25}
SetConfig {"generation":1,"path":"input.trailOnlyWhilePressed","value":false}
SetConfig {"generation":1,"path":"input.samplingRateHz","value":30}
SetConfig {"generation":1,"path":"background.mode","value":"background-aware"}
SetConfig {"generation":1,"path":"background.mode","value":"recording-compatible"}
SetConfig {"generation":1,"path":"background.mode","value":"light-background"}
SetConfig {"generation":1,"path":"background.allowSystemBorder","value":false}
SetConfig {"generation":1,"path":"display.hdrEnabled","value":true}
SetConfig {"generation":1,"path":"performance.framePacing","value":"120"}
SetConfig {"generation":1,"path":"performance.framePacing","value":"unlimited"}
SetFxParam {"generation":1,"path":"effects.diskRadius","value":40}
SetFxParam {"generation":1,"path":"effects.diskLifetimeMs","value":250}
SetFxParams {"generation":1,"patch":{"effects.ringsCount":3,"effects.ringsLifetimeMs":700,"effects.ringsRadiusMin":60,"effects.ringsRadiusMax":90,"effects.ringsAngularVelocityMultiplier":12,"effects.ringsRotationDirection":-1}}
SetFxParams {"generation":1,"patch":{"effects.shardsClickCount":6,"effects.shardsClickLifetimeMinMs":500,"effects.shardsClickLifetimeMaxMs":650,"effects.shardsClickRadius":55,"effects.shardsClickSpeedMin":45,"effects.shardsClickSpeedMax":75,"effects.shardsSizeMin":14,"effects.shardsSizeMax":30}}
SetHotkeys 1 {"togglePause":{"modifiers":["ctrl"],"key":80},"toggleAlwaysOnTrail":null,"nextFxProfile":null,"shutdown":null}
SaveFxProfile 1 夜间 柔和
ApplyFxProfile 2 夜间 柔和
DeleteFxProfile 3 夜间 柔和
SetDisplayOverride {"generation":1,"displayKey":"displayconfig-v1-sha256:...","enabled":true,"hdrEnabled":false,"framePacing":"120"}
RemoveDisplayOverride {"generation":1,"displayKey":"displayconfig-v1-sha256:..."}
ResetFxConfig
ClearLogs
Pause
Resume
Shutdown
```

`GetDisplayState` 只接受同版本 Host 生成的严格 schema 4：未知、重复、缺失字段和旧 schema 都会被
Control Center 拒绝。它返回独立运行代次、配置/应用代次、全局拓扑状态、权威离线 override 列表，以及
每个会话实际应用的特效、HDR、颜色、cadence、输出状态和 Active-FX ROI 近 5 秒不可变工程快照；它不
修改配置，也不代表其中的实验能力已经完成硬件验收。schema 4 不提供 schema 3 兼容层。
`SetConfig` 仍接受完整的 schema 20
JSON 快照。`GetFxConfig`、`SetFxParam`、原子批量的 `SetFxParams` 和 `ResetFxConfig` 是本项目的原生
特效控制接口。`GetFxConfig` 返回平面的 `EffectsConfig` 字段，写入路径只接受唯一的 `effects.*`
命名空间，不接受 Web 别名或额外单位换算。FX 快照不包含 HDR、背景、输入、性能或系统字段；这些
产品设置只通过 `GetConfig`/`SetConfig` 管理。`ResetFxConfig` 只恢复 `effects`，保留背景、HDR、输入和系统设置；
Control Center 的“重置默认”使用完整 schema 恢复其他持久化设置，同时保留 Host 已保存的整组快捷键。
路径补丁只允许配置库声明的产品字段，代次不匹配会返回 `generation_conflict`；渲染配置在下一帧应用，
当前暂停或运行状态不因重置而变化。

`GetState` 的 `productVersion` 是 Host/Control Center 设置兼容门，不是配置 schema 版本。只有它与
Control Center 自身版本完全一致时，控制面才允许修改设置；缺失、非法或不匹配都 fail-closed。
Host 生命周期入口不受该门限制。0.2.10 将主配置升级为 schema 20，增加默认全未绑定的 `hotkeys`。
显示器 override、`data` 目录和 effects-only `fx-profiles` 无需重建。

### 全局快捷键

“快捷键”页支持直接录制、清除、整组保存和重试注册。四项动作是暂停／恢复、切换常驻拖尾、
下一个特效预设和退出 Host。暂停仅影响运行时；拖尾和预设动作按现有配置流程保存，预设不包含快捷键。

执行仅使用 `RegisterHotKey` / `WM_HOTKEY`，注册统一附加 `MOD_NOREPEAT`。支持单个非修饰主键及
Ctrl/Alt/Shift/Win 加一个主键，不区分左右修饰键，长按不重复。不支持多普通键、宏或仅修饰键；
F12 禁止绑定，Win 组合不保证可用。
注册可能影响其他应用的原按键行为，不提供输入透传。重复组合被拒绝，`A` 与 `Ctrl+A` 则可分别绑定。

录制期间保留旧注册但不执行动作；候选只进入草稿。失焦、取消、30 秒超时或 5 秒失联后结束录制。
其他软件或系统占用的组合可能无法录到，请换一组。保存按当前 Host `generation` 执行：保留旧注册并先
申请全部新增组合，随后原子写入完整配置，最后启用新绑定并释放不再使用的旧注册。注册、代次冲突或
写盘失败均不改变旧绑定；配置已经写入但激活结果无法确认，或旧注册清理失败时，页面会确认权威配置并
明确提示重启 Host。启动注册失败不会阻止特效运行，“重试注册”只重试已保存绑定，并汇总已保存、已注册
和失败数量。

支持报告记录 `Hotkeys.StateScope=startup`，以及 Host 启动时的注册掩码、四项动作的注册结果和 Win32
错误、清理错误；它不是导出瞬间的实时状态。`Exit.PollingFallback=disabled` 明确表明旧固定 F12 轮询
退出路径已删除。

新增 IPC：`SetHotkeys <generation> <hotkeys-json>`、`GetHotkeyState [capture-token]`、`RetryHotkeys`、
`BeginHotkeyCapture`、`EndHotkeyCapture <capture-token>`。`GetHotkeyState` 返回标准 Host 状态加完整的
快捷键状态组；绑定通过 `hotkeysJson` 字符串携带，`hotkeyRegisteredMask` 的低四位依次对应上述四个动作，
`hotkeyError0` 至 `hotkeyError3` 为 Win32 注册错误码。仅匹配的非零 token 查询才会续期录制。

特效 Profile 同样由 Host 持有，内置且不可删除的四项是“Unity 原版”“轻量”“纯点击”和“纯拖尾”。
`GetState` 通过 `fxProfileCatalog` 返回内置/自定义目录，通过 `activeFxProfile` 返回当前特效与某一
Profile 完全匹配的名称，并用 `fxProfileWarning` 报告启动时被跳过的损坏、冲突或不可读文件；没有精确
匹配时显示“自定义”。`SaveFxProfile`、`ApplyFxProfile` 和
`DeleteFxProfile` 的负载均以当前 `generation` 开头，名称可以包含空格；过期请求返回
`generation_conflict`。保存使用同目录临时文件、flush 和替换，应用先通过主配置的原子写入提交候选
`effects`，删除以单个自定义 Profile 文件的移除作为提交点。只有操作成功后 Host 才更新内存状态；三种
操作都会把用于并发冲突检测的控制 `generation` 增加一次，但只有应用会推进独立的配置 generation。
纯目录的保存/删除不会触发渲染与捕获状态的无意义重应用，失败也不会发布半更新的目录或配置。
为保证 `activeFxProfile` 身份稳定，Host 会拒绝与其他内置或自定义 Profile 完全相同的 effects 快照。

Profile 是严格的 effects-only 快照：保存和应用只涉及 `effects`，明确不包含也不改变 `background`、
`display`、`input`、`performance` 或 `system`。因此实验性 Active-FX ROI 的
`performance.activeFxRoiEnabled` 也不属于 Profile；切换 Profile 不会顺带打开、关闭或覆盖 ROI。


## README 与 Star 历史维护

首页特效图片的来源与重现命令见[预览素材说明](images/README.md)。更新渲染后应重新取样，并同步检查中英文图片说明。

使用 Node.js 24，无需 npm 安装：

```powershell
node tools/verify-readme.mjs
node tests/star-history.mjs
```

工作流 `.github/workflows/star-history.yml` 在相关 PR／main 推送时只读检查文档和更新器；
每日北京时间 03:17 或手动触发时才写入独立 `star-history` 分支。数据提交只包含
`README.md`、`stars.csv` 和 `star-history.svg`，不修改产品源码或版本。

本地重现更新时，先将数据分支检出到独立目录，再从本仓库根目录执行：

```powershell
$env:GITHUB_REPOSITORY = 'CialloKing/ba-click-fx-desktop'
node tools/update-star-history.mjs --data-dir ..\ba-click-fx-desktop-star-history
```

脚本读取 `GITHUB_TOKEN` 或 `GH_TOKEN` 环境变量；无令牌时使用公开 API 限额，不要把令牌写入文件或提交。
首次初始化仅对空数据目录使用 `--bootstrap`：先按当前 Stargazer 的时间重建过去数据，再记录当天实测总数。
重建无法恢复已经取消的 Star；历史 CSV 的 `source` 和 `observed_at` 区分重建与实测。
漏跑日期不补造，同日同总数重跑不修改文件；API 或数据校验失败时不写入结果。
此任务不属于产品构建或 Windows 图形能力验收。

## Unity 资源维护

`packed_fx_textures` 测试逐张解压 raw LZ4 Block，并锁定 RGBA8 texel 的尺寸、行距和 SHA-256。
生成器是仅供维护者使用的开发工具；只有在更新 Unity 真值快照时才需要运行，输入 PNG、Node.js 和
Unity 工程都不是构建产物或运行时依赖：

```powershell
node tools\generate-packed-fx-textures.mjs `
  --project "D:\path\to\UnityProject"
```


配置完成后可执行只读的 Unity 外部证据校验：

```powershell
cmake --build build\x64 --config Release --target verify_unity_reference
```
