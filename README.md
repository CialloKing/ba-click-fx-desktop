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
  回退到内部 FX-only transport。
- `background-aware`、`recording-compatible`、`light-background` 是唯一的产品渲染模式。前者使用
  WGC 和完整的 Differential Bloom；`recording-compatible` 关闭 WGC，拟合 Web 版透明覆盖层的
  `visual-max`、`bright-core`、`0.90` Alpha 上限和 source-over；`light-background` 使用同一颜色策略，
  但将桌面 Alpha 上限收紧为 `0.85`。
- 最终透明交换链使用 FP16 扩展预乘输出；普通 SDR 下不得承诺白底仍有加法余量。
- 三角碎片保留清晰的 HDR 直接能量，同时按游戏 `FX_SHADER_Additive_0` 进入全场景 Bloom，
  因此既有锐利核心也有对应的模糊光晕。

桌面版的 DirectComposition overlay 没有浏览器 `Screen` API 的逐像素等价物。只有
`background-aware` 在 WGC 样本有效时能把异步桌面纹理带入合成；`recording-compatible` 和 `light-background`
是没有捕获背景时的确定性传输策略，不能宣称对任意桌面像素逐点还原。

## 文档入口

- [docs/ROADMAP.md](docs/ROADMAP.md)：当前体验优先的开发顺序、交付物和执行边界。
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

日常只验证桌面 Host 时使用按目标构建，避免触发包含全部测试和 Spike 的 `ALL_BUILD`：

```powershell
cmake --build --preset alpha-host-release --parallel 4
```

`alpha-release-verify` 仍然保留完整 Release 构建和 CTest 流程；它不是快速迭代命令。

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
“重置默认”会在确认后一次性恢复全部持久化设置，不改变当前的暂停或运行状态。

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
未签名的 Sparse Package 模板、原生签名器和安装脚本，不包含预签名 MSIX、公钥证书或私钥。

1. 从 Release 下载与系统匹配的 `*-setup-windows-x64.exe`，同时下载同名 `.sha256`，按页面提供的哈希校验文件。
2. 双击安装器并确认一次 UAC。安装器会把程序放到 `Program Files`，为当前用户注册方案 C Package Identity，
   在开始菜单和桌面创建 Control Center 快捷方式，然后打开 Control Center。
3. 在 Control Center 中点击“启动 Host”，再按需选择“背景感知”“录屏兼容拟合”或“浅色背景优化”。
   关闭 Control Center 不会停止 Host；可以从通知区域退出 Host。
4. 卸载时使用开始菜单中的卸载项或 Windows“已安装的应用”。默认保留程序目录下的 `data` 配置，重新安装
   后仍可继续使用；如需彻底清理，请在卸载前手动备份并删除该目录。

该安装器使用目标机生成的本机证书为 Sparse Package 签名，不是公有代码签名。Windows SmartScreen 可能显示
“Unknown Publisher”，这是预期提示。安装器只把公钥加入 `LocalMachine\TrustedPeople`，签名验证完成后立即删除
`LocalMachine\My` 中的证书和不可导出私钥；不要从 Release 单独下载或安装证书、MSIX、私钥或 SDK 工具。若没有管理员权限，
请改用上面的 portable 测试 ZIP，直接解压运行即可，但 portable 没有 Package Identity，无法承诺无边框 WGC。

## Host 控制面

首个产品化垂直切片已经接入版本化配置和本地 Named Pipe。portable Host 会在主程序
`ba-click-fx-desktop.exe` 同目录创建 `BAFX.config.json` 和
`ba-click-fx-desktop-support.log`；Identity 安装版则将这些文件放入同一目录下可写的 `data` 子目录。
支持报告也会被限制在这棵目录树内。运行时不再使用
`%LOCALAPPDATA%`、当前工作目录或其他用户目录保存数据。Host 使用
`Local\BAFX.Host.v1` 互斥体保证单实例。

首次生成的 schema 12 配置将 `background.mode` 设为 `background-aware`、
`background.allowSystemBorder` 设为 `true`、`display.hdrEnabled` 设为 `false`，并以
`input.trailOnlyWhilePressed=true`、`input.samplingRateHz=0` 保持按住拖尾且不额外限频。
测试版只接受字段完整的显式 `schemaVersion=12`：缺少版本、section 或字段，非当前版本、
未知字段和枚举别名都会被拒绝。Host 记录错误后仅在内存中使用当前默认值，不补齐、不迁移也不改写
原文件。只有
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
可以读取状态、暂停或恢复特效。基础页提供启用状态、点击特效、鼠标拖尾、拖尾常驻、效果大小、拖尾长度、
拖尾宽度、输入采样率上限、Bloom 强度与 Bloom 质量；高级页再按“时间与透明度”“粒子与材质”
“圆环参数”“Bloom 参数”分成四个二级页面。粒子与材质页直接使用与 Web 相同的
`disk.radius`、`disk.lifetimeMs`、`rings.hdrIntensity`、`shards.hdrIntensity` 和
`trail.trailOpacity` 路径；圆环页提供 `rings.count`、`rings.lifetimeMs`、
`rings.radiusMin`、`rings.radiusMax`、`rings.angularVelocityMultiplier` 和
`rings.rotationDirection`。另外两页提供透明度、点击/拖尾时间倍率、拖尾寿命，以及 Bloom 扩散、
阈值、软阈值和亮度上限。背景区域包含指针排除、系统捕获边框和默认关闭的 HDR 输出开关。
调整结果在下一帧交给 Host。“拖尾常驻”默认关闭；开启后
无需按住鼠标，普通移动也会生成纯拖尾，但不会伪造点击圆盘或圆环。这是参考 Web 行为提供的原生产品增强，
不属于游戏原脚本的按压 FX 路径。数值控件会合并连续拖动后的写入，避免为每个滑块像素都写一次配置。
`bloom.intensity` 是 Web/Unity 原始标量，默认值为 `1.7`、有效范围为 `0..10`，不是相对 `1.0` 的倍率。
Bloom 质量只是 diffusion 的派生预设：紧凑、适中、原版、极宽分别对应 `4/6/7/10`，其他值显示为“自定义”。

控制中心的“重置默认”按钮会先请求确认，再用内置默认 schema 整体替换持久化配置。它不会恢复已经
暂停的特效；需要继续显示时仍应单独点击“恢复特效”。

Host 在每次输入消费/交换链呈现更新中，为按压 FX 锁存一份帧边界当前鼠标位置，并用同一份
`renderTime` 执行本轮模拟动作。普通调用顺序为 Down→Held→Up；普通 Up-only 释放帧的 Held 为 false，
因此不会先把该帧 Move 应用为按住移动再释放。含任一边沿的帧也不会用边沿后的尾随 Move 重启常驻拖尾。Raw Input 的
Down/Up/Cancel 仍按原顺序无损保留，仅用于诊断和 native 扩展；严格效果路径将它们归约为单帧
Down/Held/Up 布尔态，并固定按 Down→Held→Up 执行，Cancel 最后作为 native 硬边界处理。Unity
`2021.3.45f1` Player 黑盒已确认 `Down-Up-Down` 在聚合帧中三态同时为 true；其他边沿排列及游戏使用的
Unity `2021.3.56f2` 仍未验证。没有待消费的位置时不会仅为输入适配而读取光标；按住静止期间由模拟
`advance` 推进距离发射的时间基线。

“输入采样率上限 (Hz)”默认值 `0` 表示不额外限频；`1..1000` 仅使用位置样本的消息分派 QPC 推进
可选输入采样相位。QPC 不决定模拟动作时间、帧态归约、严格路径执行顺序或释放时刻。`30 Hz` 是参考 Web 版提供的手机客户端
视觉近似，`15 Hz` 折线更明显，`60 Hz` 更平滑；这些是人工审核入口，不是从 Prefab 提取出的固定客户端
帧率，也不会修改 Unity TrailRenderer 的 `m_MinVertexDistance=0.01`、`time=0.3` 或
`widthMultiplier=0.005`。

控制中心也会显示三个渲染模式（显示名依次为“背景感知”“录屏兼容拟合”“浅色背景优化”），以及
“允许黄色捕获边框”复选框。它们对应的 wire values 分别为 `background-aware`、
`recording-compatible`、`light-background`；切换到后两项会关闭 WGC。新配置默认请求
`background-aware` 并允许 Windows 显示捕获边框；用户可取消勾选以要求无边框捕获。这不构成 WGC、
录屏兼容性或 HDR 的支持声明；关闭边框后若无边框 WGC 无法安全建立，Host 必须保持或回退到内部
FX-only transport。

“录屏兼容拟合”固定使用截图中 Web 版设置的原生对应：`browser-overlay` 透明覆盖层、
`visual-max` Alpha 策略、`bright-core` 颜色补偿、`0.90` Alpha 上限、`source-over` 宿主合成，
并按未知透明背景处理，不让 WGC 样本进入最终 pass。原生桌面没有 Web 的 DOM 背景表面；这里由
DirectComposition 的 FP16 预乘透明 surface 承担最接近的传输角色，因此这是录屏可见性优先的视觉拟合，
不是对 `hostCompositingSurface=dom-backdrop` 的逐像素实现，也不保证所有录屏器都能捕获。

底层协议仍可由 PowerShell 或其他 Named Pipe 客户端验证：

```text
GetState
GetConfig
GetFxConfig
SetConfig {"generation":1,"path":"effects.globalScale","value":1.25}
SetConfig {"generation":1,"path":"input.trailOnlyWhilePressed","value":false}
SetConfig {"generation":1,"path":"input.samplingRateHz","value":30}
SetConfig {"generation":1,"path":"background.mode","value":"background-aware"}
SetConfig {"generation":1,"path":"background.mode","value":"recording-compatible"}
SetConfig {"generation":1,"path":"background.mode","value":"light-background"}
SetConfig {"generation":1,"path":"background.allowSystemBorder","value":false}
SetFxParam {"generation":1,"path":"disk.radius","value":40}
SetFxParam {"generation":1,"path":"disk.lifetimeMs","value":250}
SetFxParams {"generation":1,"patch":{"rings.count":3,"rings.lifetimeMs":700,"rings.radiusMin":60,"rings.radiusMax":90,"rings.angularVelocityMultiplier":12,"rings.rotationDirection":-1}}
ResetFxConfig
Pause
Resume
Shutdown
```

`SetConfig` 也接受完整的 schema 12 JSON 快照。`GetFxConfig`、`SetFxParam`、原子批量的
`SetFxParams` 和 `ResetFxConfig` 对应 Web 的实例 API 命名；当前只返回和接受已经接入 Native
模拟或材质求值的参数。`ResetFxConfig` 只恢复 `effects`，保留背景、HDR、输入和系统设置；Control Center
中的“重置默认”则使用完整 schema 恢复全部持久化设置。路径补丁只允许配置库声明的产品字段，代次不匹配会返回
`generation_conflict`；所有命令均在下一帧由 Host 应用。

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
