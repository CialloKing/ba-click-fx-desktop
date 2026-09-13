# ba-click-fx-desktop

[English](README.en.md) · [下载最新版本](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest)

[![GitHub Stars](https://img.shields.io/github/stars/CialloKing/ba-click-fx-desktop.svg)](https://github.com/CialloKing/ba-click-fx-desktop/stargazers)

Windows 原生桌面点击特效与鼠标拖尾，以《蔚蓝档案》的 Unity/游戏资源为视觉参考，提供透明覆盖层、
原生控制中心和 OBS 透明特效输出。当前产品版本：**0.2.12**。

运行面向 Windows 10/11 x64；安装器最低要求 OS build `19041`。当前人工特效审核以单主屏 SDR 为准。
发布包无需另装 Visual C++ 运行库、Windows App SDK 或开发工具。Host 负责特效，Control Center 负责设置和启停。

## 目录

- [下载与快速开始](#下载与快速开始)
- [常用设置与渲染模式](#常用设置与渲染模式)
- [支持范围与证书](#支持范围与证书)
- [OBS 与 Spout2](#obs-与-spout2)
- [常见问题与反馈](#常见问题与反馈)
- [源码构建](#源码构建)
- [文档入口](#文档入口)
- [Star 历史](#star-历史)
- [开发说明](#开发说明)
- [许可证](#许可证)

## 下载与快速开始

从[官方 Release](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest) 下载。
官方提供四个 Full 版资产：安装器、便携 ZIP，以及各自的 `.sha256` 校验文件。
两种程序包都包含完整特效、Control Center 和 Spout2 发送功能；使用 OBS 时需另装接收插件。

| 需求 | 选择 | 说明 |
|---|---|---|
| 日常使用、开始菜单与桌面快捷方式 | 安装版 Installer：`*-setup-windows-x64.exe` | 需要管理员 UAC，注册当前用户的应用身份；无边框 WGC 仍需系统授权 |
| 无管理员权限、解压即用 | 便携版 Portable：`*-Portable-windows-x64.zip` | 解压到可写目录；不提供应用身份，不承诺无边框 WGC |

1. 按上表下载安装器或便携 ZIP，以及同名 `.sha256`；校验方法见下方折叠说明。
2. **Installer**：双击安装器并确认 UAC，安装后会打开 Control Center；也可从开始菜单或桌面快捷方式打开。
   **Portable**：完整解压到可写目录，保留目录结构和随附文件，打开 `BAFX.ControlCenter.exe`。
   它必须与 `ba-click-fx-desktop.exe` 位于同一目录才能启动 Host。
3. 点击“启动 Host”，试着点击或按住鼠标拖动即可看到特效；在“基础设置”页调整点击特效、拖尾和背景模式。

<details>
<summary>验证下载文件（SHA-256）</summary>

在 PowerShell 中执行，将占位文件名替换为实际下载的完整文件名：

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath '.\下载的完整文件名'
Get-Content -LiteralPath '.\下载的完整文件名.sha256'
```

将计算出的 SHA-256 与校验文件首列比较；若不一致，请重新下载后再安装或解压。

</details>

**更新和配置**：在“系统”页手动点击“检查更新”，再下载并运行新安装器，或替换完整便携包的程序文件。
更新前退出 Host，备份配置；不要混用不同版本的 Host 与 Control Center。版本检查不会自动下载或安装。

Portable 将 `BAFX.config.json`、`fx-profiles` 和日志保存在 EXE 目录；安装版保存在安装目录的 `data` 子目录。
语言偏好独立保存在同一位置的 `BAFX.ControlCenter.language`，内容为 `auto`、`zh-CN` 或 `en-US`；发行包不携带此文件。
卸载使用开始菜单卸载项或 Windows“已安装的应用”，默认保留 `data`；需彻底清理时先备份，退出程序并卸载后再删除该目录。

## 常用设置与渲染模式

**界面语言**：在“系统 → 系统行为 → 界面语言”选择“跟随系统 / 简体中文 / English”，立即生效，无需重启，Host 未启动时也可切换。
默认跟随 Windows 当前用户的显示语言：中文使用简体中文，其余语言使用英文。重启后保留选择，“重置默认”保留语言偏好。

Control Center 提供“基础设置”“高级参数”“显示与性能”“快捷键”“系统”五个页面。常用设置包括效果大小、
拖尾长度与宽度、Bloom 强度与质量、随 Windows 启动，以及内置和自定义特效预设。
预设只保存特效参数，不覆盖背景、显示、输入、性能或系统设置。

| 背景模式 | 适用场景与行为 |
|---|---|
| 背景感知（`background-aware`，默认） | 使用 WGC 捕获背景参与合成；捕获或自排除失败时回退 FX-only |
| 录屏兼容（测试，`recording-compatible`） | 仅 OS build `28000` 或更高可选择；尝试 WGC 会话级自排除，不可用时回退其他捕获路径或 FX-only；录屏兼容性尚未完成验收 |
| 浅色背景优化（`light-background`） | 关闭 WGC，采用更严格的透明度上限，可用于浅色桌面效果比较 |

FX-only 表示只呈现特效、不合入捕获背景，是内部回退路径，不是第四种可选背景模式。
“录屏兼容”在界面中标为“测试，仅 Windows 11 26H1 及以后”；系统版本过低或无法确认时不会应用该选择。
三种模式都不承诺在任意桌面背景上逐像素还原游戏画面。

“核心性能模式（关闭 Bloom 与背景）”保留圆盘、圆环、碎片和拖尾，跳过 Bloom 与 WGC，固定保守 SDR、60 FPS 和 FX-only。
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

**Host 启动不以证书校验为前提**。证书异常只影响无边框 WGC，请求失败时回退 FX-only，Host 和其他捕获路径仍可运行。
重新运行当前安装器（含同版本修复）可重新签名修复；安装成功本身不代表已获得无边框授权或完成硬件验收。

发布安装器没有公有代码签名，SmartScreen 可能显示“Unknown Publisher”。用户无需另行下载证书、MSIX 或 SDK。

<details>
<summary>证书与“安装状态异常”的区别</summary>

安装器在目标机生成本机证书，为提供 Package Identity 的 Sparse Package 签名。每次运行当前安装器都会生成新证书并重新签名。
有效证书不设临期阈值；实际过期、缺失、损坏或签名不匹配时，无边框请求回退 FX-only。

安装状态校验是另一项检查：Control Center 需要完整匹配的 `INSTALL-STATE.json` 与 `.bak` 才能激活安装版 Host。
若显示“安装状态异常”，启动仍会被阻止。请使用运行 Host 的同一 Windows 用户重新运行安装器修复，不要手动删除状态或事务文件。
签名、证书存储及恢复细节见 [ADR-0009](docs/adr/0009-identity-installer-scheme-c.md)。

</details>

## OBS 与 Spout2

1. 安装与 OBS 匹配的 [Spout2 接收插件](https://github.com/Off-World-Live/obs-spout2-plugin/releases/)，
   在 Control Center“系统”页勾选“启用 OBS 透明特效输出”，检查发送及插件状态。
2. 在 OBS 将游戏／桌面捕获置底，`Spout2 Capture` 来源置顶，发送者选择 `ba-click-fx-desktop`。
3. Composite Mode 选择 **`Premultiplied Alpha`**；来源混合方式保持 **`Default`**，混合模式保持 **`Normal`**。
4. 对 Spout2 来源执行 `Transform -> Fit to Screen`。空闲时只显示底层画面，点击或拖动时叠加特效。

Spout2 只发送透明特效层，不包含桌面背景，WGC 不可用时也可发送。
插件探测、旧场景迁移和验收方法见 [OBS 使用说明](docs/OBS_SPOUT2.md)。

## 常见问题与反馈

**移动鼠标没有拖尾？** 默认只在按住鼠标时产生拖尾。请先启动 Host，在“基础设置”页确认“鼠标拖尾”已启用；
希望普通移动也有拖尾时，开启“拖尾常驻”。

**屏幕出现黄色边框？** 这是 Windows 的捕获提示，默认允许显示。在“基础设置”页取消“允许黄色捕获边框”后，
只有无边框授权与能力检查通过才会开始捕获，否则回退 FX-only。安装版也需要系统授权。

**关闭控制中心后，特效还在？** Control Center 与 Host 是独立进程。临时暂停可使用“暂停特效”／“恢复特效”按钮或通知区域菜单；
彻底停止请点击“关闭 Host”，或从 Host 的通知区域菜单退出。关闭 Control Center 窗口不会自动停止 Host。

**OBS 中没有特效？** 先确认 Host 已启动且未暂停，在“系统”页勾选“启用 OBS 透明特效输出”，检查发送及插件状态。
再确认 OBS 选择了正确发送者、Spout2 来源置顶且可见，并按[上述步骤](#obs-与-spout2)检查透明合成与画布大小；空闲时全透明是正常现象，请点击或拖动验证。

仍有问题，请到 [GitHub Issues](https://github.com/CialloKing/ba-click-fx-desktop/issues) 反馈，并附上：

- Host 与 Control Center 版本、安装版或便携版。
- Windows 版本与 OS build（可运行 `winver` 查看），以及显示器数量、HDR 状态；OBS 问题另附 OBS 与插件版本。
- 复现步骤、发生时间、预期和实际结果，以及使用的背景模式。
- `ba-click-fx-desktop-support.log`；若相关时段已轮转，再附对应的 `.log.1` 至 `.log.3`。便携版日志在 EXE 目录，安装版在安装目录的 `data` 子目录。

安装或卸载失败时，请附错误框给出的完整路径下的安装日志。公开上传前检查日志中的用户名与本机路径。
更多诊断说明见 [SUPPORT.md](SUPPORT.md)。

## 源码构建

Full／Slim 是构建变体，与安装版／便携版的安装方式无关。Full 包含 Spout2；Slim 去掉 Spout2，仍保留 Host、Control Center 和完整特效。
Slim 仅提供源码构建与本地打包入口，没有官方预编译下载。

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

## Star 历史

独立的 `star-history` 分支保存 Star 数量历史，GitHub Actions 每日北京时间 03:17 调度更新，实际执行可能延迟。

<p align="center">
  <a href="https://github.com/CialloKing/ba-click-fx-desktop/blob/star-history/stars.csv">
    <img src="https://raw.githubusercontent.com/CialloKing/ba-click-fx-desktop/refs/heads/star-history/star-history.svg" alt="ba-click-fx-desktop Star 数量历史图" width="960">
  </a>
</p>

[查看 CSV 原始数据](https://github.com/CialloKing/ba-click-fx-desktop/blob/star-history/stars.csv)。

<details>
<summary>Star 历史数据如何记录</summary>

初始化之前的数据按现存 Stargazer 的时间重建（`reconstructed`），无法恢复已取消的 Star；
启用后的每日记录为实测总数（`observed`），允许下降。漏跑日期保持缺失，不插值或补造快照。

</details>

## 开发说明

桌面版使用 C++20、Win32、D3D11、HLSL 和 DirectComposition 从零实现。
独立的[网页版 ba-click-fx](https://github.com/CialloKing/ba-click-fx)提供[浏览器演示](https://ba-click-fx.cialloking.top)；
两者共享 Unity/游戏资源这一视觉参考，渲染实现、配置和 IPC 各自独立。

本项目主要通过 AI 生成和迭代完成（**绝无手写代码**），并经过实际运行测试、参数调校和效果校准。
架构合同仍为 **v0.3 / Proposed**，已实现的路径不自动成为硬件支持声明。

## 许可证

[GNU GPL v2](LICENSE)。随附组件许可见 [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)。
