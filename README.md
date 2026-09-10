# ba-click-fx-desktop

[English](README.en.md) · [下载最新版本](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest)

Windows 原生桌面点击特效与鼠标拖尾，以《蔚蓝档案》的 Unity/游戏资源为视觉参考，提供透明覆盖层、
原生控制中心和 OBS 透明特效输出。当前产品版本：**0.2.12**。

运行面向 Windows 10/11 x64；当前人工特效审核以单主屏 SDR 为准。发布包无需另装 Visual C++ 运行库、
Windows App SDK 或开发工具。Host 负责特效，Control Center 负责设置和启停。

## 目录

- [下载与快速开始](#下载与快速开始)
- [常用设置与渲染模式](#常用设置与渲染模式)
- [支持范围与证书](#支持范围与证书)
- [OBS 与 Spout2](#obs-与-spout2)
- [源码构建](#源码构建)
- [文档入口](#文档入口)
- [开发说明](#开发说明)
- [许可证](#许可证)

## 下载与快速开始

从[官方 Release](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest) 下载。
Installer／Portable 是安装方式；Full／Slim 是构建变体。官方只提供 Full 的四个资产：安装器、便携 ZIP，
以及各自的 `.sha256` 校验文件。Slim 保留源码构建与本地打包入口，没有预编译下载。

| 需求 | 选择 | 说明 |
|---|---|---|
| 日常使用、开始菜单入口或尝试无边框 WGC | Full Installer：`*-setup-windows-x64.exe` | 安装需要管理员 UAC，为当前用户注册 Package Identity |
| 无管理员权限、解压即用 | Full Portable：`*-Portable-windows-x64.zip` | 无 Package Identity，不承诺无边框 WGC |
| OBS 透明特效输出 | 上述任一 Full 发布包 | 包含 Spout2 发送功能；OBS 接收插件需自行安装 |
| 自行构建且不需要 Spout2 | Slim | 仍包含 Host、Control Center 和完整特效 |

1. 下载安装器或便携 ZIP，以及同名 `.sha256`。在 PowerShell 中核对文件的 SHA-256 与校验文件首列一致：

   ```powershell
   Get-FileHash -Algorithm SHA256 -LiteralPath '.\下载的完整文件名'
   Get-Content -LiteralPath '.\下载的完整文件名.sha256'
   ```

2. **Installer**：双击安装器并确认 UAC，安装后会打开 Control Center；也可从开始菜单或桌面快捷方式打开。
   **Portable**：完整解压到可写目录，保留目录结构和随附文件，打开 `BAFX.ControlCenter.exe`。
   它必须与 `ba-click-fx-desktop.exe` 位于同一目录才能启动 Host。
3. 点击“启动 Host”，在“基础”页调整点击特效、拖尾和背景模式。默认拖尾只在按住鼠标时出现，
   开启“拖尾常驻”后普通移动也会产生拖尾。
4. 暂停或恢复可使用 Control Center 或通知区域菜单。停止特效进程可点击“关闭 Host”或从 Host 的通知区域菜单退出；
   关闭 Control Center 窗口不会自动停止 Host。

**更新和配置**：在“系统”页手动点击“检查更新”，再下载并运行新安装器，或替换完整便携包的程序文件。
更新前退出 Host，备份配置；不要混用不同版本的 Host 与 Control Center。版本检查不会自动下载或安装。

Portable 将 `BAFX.config.json`、`fx-profiles` 和日志保存在 EXE 目录；安装版保存在安装目录的 `data` 子目录。
卸载使用开始菜单卸载项或 Windows“已安装的应用”，默认保留 `data`；需彻底清理时先备份，退出程序并卸载后再删除该目录。

## 常用设置与渲染模式

Control Center 提供“基础”“高级”“显示与性能”“快捷键”“系统”五个页面。常用设置包括效果大小、
拖尾长度与宽度、Bloom 强度与质量、随 Windows 启动，以及内置和自定义特效预设。
预设只保存特效参数，不覆盖背景、显示、输入、性能或系统设置。

| 背景模式 | 适用场景与行为 |
|---|---|
| 背景感知（`background-aware`，默认） | 使用 WGC 捕获背景参与合成；捕获或自排除失败时回退 FX-only |
| 录屏兼容拟合（`recording-compatible`） | 关闭 WGC，使用透明覆盖层拟合；不保证适配所有录屏软件 |
| 浅色背景优化（`light-background`） | 关闭 WGC，采用更严格的透明度上限，可用于浅色桌面效果比较 |

FX-only 表示只呈现特效、不合入捕获背景，是内部回退路径，不是第四种可选背景模式。
三种模式都不承诺在任意桌面背景上逐像素还原游戏画面。

“核心性能模式（低配测试）”保留圆盘、圆环、碎片和拖尾，跳过 Bloom 与 WGC，固定保守 SDR、60 FPS 和 FX-only。
它与 Full／Slim 构建变体无关。HDR 请求和自适应 Active-FX ROI 实验开关默认关闭。

全局快捷键默认全部未绑定，可在“快捷键”页配置暂停／恢复、常驻拖尾、下一个预设和退出 Host。
支持单主键或 Ctrl／Alt／Shift／Win 加单主键，F12 不可绑定；被系统或其他程序占用的组合可能注册失败。
“重置默认”保留已保存的快捷键和当前暂停／运行状态。

## 支持范围与证书

| 能力 | 当前范围 |
|---|---|
| 点击、拖尾、Control Center、FX-only | Windows 10/11 x64、单主屏 SDR 下可测试；具体证据见支持文档 |
| Full／Slim 构建、Portable／安装器 | 已有构建及打包验证；Slim 仅供源码构建 |
| WGC 背景感知 | 依赖系统、运行时接口和自排除能力；失败回退 FX-only |
| Windows 11 目标硬件无边框 WGC | **Not Run**：真实用户授权、无边框效果和 DWM 最终像素尚未验收 |
| HDR／Advanced Color | 实验路径存在，尚未完成支持声明 |
| 多显示器、混合 DPI／刷新率、跨适配器 | 尚未完成完整硬件验收 |
| OBS／Spout2 | Full 可按下节配置；插件和录制结果以具体环境验收为准 |

构建通过、离屏测试或 WARP 软件渲染不代表真实硬件能力已验收。
详细证据、排除项及诊断字段见 [SUPPORT.md](SUPPORT.md) 和 [验证说明](docs/VALIDATION.md)。

**用户自签名方案**：安装器在目标机生成本机证书，为 Package Identity 的 Sparse Package 签名。
发布安装器没有公有代码签名，SmartScreen 可能显示“Unknown Publisher”。用户无需另行下载证书、MSIX 或 SDK。

**Host 启动不以证书校验为前提**。证书仅影响无边框 WGC；实际过期、缺失、损坏或签名不匹配时，
无边框请求回退 FX-only，Host 和其他捕获路径仍可运行。有效证书不设临期阈值。
重新运行当前安装器（含同版本修复）会生成新证书并重新签名。

**安装状态校验是另一项检查**：Control Center 需要完整匹配的 `INSTALL-STATE.json` 与 `.bak` 才能激活安装版 Host。
若显示“安装状态异常”，请使用运行 Host 的同一 Windows 用户重新运行安装器修复；不要手动删除状态或事务文件。
签名、证书存储及恢复细节见 [ADR-0009](docs/adr/0009-identity-installer-scheme-c.md)。

默认允许 Windows 显示黄色捕获边框。取消“允许黄色捕获边框”后，只有无边框授权及相关能力检查通过才启动捕获；
否则回退 FX-only。安装成功本身不等于无边框 WGC 已获授权或完成目标硬件验收。

## OBS 与 Spout2

1. 安装与 OBS 匹配的 [Spout2 接收插件](https://github.com/Off-World-Live/obs-spout2-plugin/releases/)，
   在 Control Center“系统”页启用“OBS 透明特效输出”，检查发送及插件状态。
2. 在 OBS 将游戏／桌面捕获置底，`Spout2 Capture` 来源置顶，发送者选择 `ba-click-fx-desktop`。
3. Composite Mode 选择 **`Premultiplied Alpha`**；来源混合方式保持 **`Default`**，混合模式保持 **`Normal`**。
4. 对 Spout2 来源执行 `Transform -> Fit to Screen`。空闲时只显示底层画面，点击或拖动时叠加特效。

Spout2 只发送透明特效层，不包含桌面背景，WGC 不可用时也可发送。
插件探测、旧场景迁移和验收方法见 [OBS 使用说明](docs/OBS_SPOUT2.md)。

## 源码构建

准备 Git、CMake 3.25+、Visual Studio 2022+ 的 **Desktop development with C++** 工作负载、
MSVC x64 工具与 Windows SDK 10.0.19041+。脚本支持 Windows PowerShell 5.1／PowerShell 7；
Python 3 用于完整 Python 合同测试。Node.js 仅供 Unity 资源和 Star 历史等维护工具使用，普通产品构建无需安装。

在 Developer PowerShell 中执行；测试需要已登录且未锁屏的交互式 Windows 桌面：

```powershell
git clone https://github.com/CialloKing/ba-click-fx-desktop.git
cd ba-click-fx-desktop
```

**Full（包含 Spout2）**：首次准备独立 vcpkg；若已有 checkout，直接设置 `VCPKG_ROOT`。

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& 'C:\dev\vcpkg\bootstrap-vcpkg.bat'
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
cmake --workflow --preset release-verify
```

**Slim（不包含 Spout2，无需 vcpkg）**：

```powershell
cmake --workflow --preset slim-release-verify
```

开发构建的 Host 和 Control Center 分别位于各自的输出目录；启动布局、完整构建、打包、排错、测试和 IPC 示例见[开发指南](docs/DEVELOPMENT.md)。

## 文档入口

- [开发指南](docs/DEVELOPMENT.md)：完整构建、打包、IPC 和资源维护。
- [支持与验证范围](SUPPORT.md)：当前可测试行为、日志和明确排除项。
- [架构](ARCHITECTURE.md)与 [ADR](docs/adr/README.md)：渲染、控制面和安装决策。
- [路线图](docs/ROADMAP.md)、[Spike](docs/SPIKES.md) 与[验证说明](docs/VALIDATION.md)：开发顺序和证据门槛。
- [Unity 参考](docs/UNITY_REFERENCE.md)：视觉参考与外部资源证据边界。
- [更新记录](CHANGELOG.md)：各版本变更。

## 开发说明

桌面版使用 C++20、Win32、D3D11、HLSL 和 DirectComposition 从零实现。
独立的[网页版 ba-click-fx](https://github.com/CialloKing/ba-click-fx)提供[浏览器演示](https://ba-click-fx.cialloking.top)；
两者共享 Unity/游戏资源这一视觉参考，渲染实现、配置和 IPC 各自独立。

本项目主要通过 AI 生成和迭代完成（**绝无手写代码**），并经过实际运行测试、参数调校和效果校准。
架构合同仍为 **v0.3 / Proposed**，已实现的路径不自动成为硬件支持声明。

## 许可证

[GNU GPL v2](LICENSE)。随附组件许可见 [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)。
