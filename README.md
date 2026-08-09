# ba-click-fx-desktop

`ba-click-fx-desktop` 是从零实现的 Windows 原生桌面点击特效。项目不复用
`ba-click-fx` 的 JavaScript、WebGL 或 WebGPU 渲染代码；Unity/游戏资源是视觉真值，
Web 版本只作为行为与参数语义参考。

Release 运行时是单文件：Visual C++ 运行库静态链接，Circle、Grad Ring、Triangle Atlas、Trail
四张原始 PNG 以压缩文本嵌入 C++，启动时只在内存中解码并上传为 sRGB GPU 纹理；材质 HLSL
也嵌入程序。运行不读取 Unity 工程、游戏目录或旁置 shader/图片文件，但仍使用 Windows 自带的
D3D11、DirectComposition、WIC 和 D3DCompiler 系统组件。

当前架构版本是 **v0.2**，状态为 **Proposed**。这意味着候选技术栈和资源所有权底座已经冻结，但涉及
DirectComposition、Windows Graphics Capture、HDR/Advanced Color 和多适配器的结论，
必须取得仓库中定义的 Spike 证据或接受明确的 fallback 后，相关 ADR 才能标记为 Accepted。

## 冻结的技术方向

- C++20、Win32、D3D11、HLSL、DirectComposition。
- D3D11 immediate context 只由 Render Owner 使用。
- 一个 DXGI Adapter 定义一个资源域；跨适配器资源不隐式共享。
- Windows Graphics Capture 是可选的 Background Sensor，不是基础点击特效的依赖。
- 背景交互使用非负的 Background-aware Differential Bloom。
- 最终透明交换链使用 FP16 扩展预乘输出；普通 SDR 下不得承诺白底仍有加法余量。
- 三角碎片保持 crisp-only，可保留未模糊的 HDR 核心，但 `BloomSeed=0`，不产生模糊光晕。

## 文档入口

- [ARCHITECTURE.md](ARCHITECTURE.md)：系统边界、模块、线程、数据流和降级规则。
- [docs/adr](docs/adr)：七项尚待证据闭环的架构决策。
- [docs/SPIKES.md](docs/SPIKES.md)：四个必须执行的硬件/API Spike。
- [docs/VALIDATION.md](docs/VALIDATION.md)：测试层级、Golden Oracle 和发布门槛。
- [docs/UNITY_REFERENCE.md](docs/UNITY_REFERENCE.md)：游戏解包资源、Unity 重建工程与 Golden 的证据边界。
- [SUPPORT.md](SUPPORT.md)：首个 Alpha 的可测试范围、退出方式和明确排除项。
- [ASSET-MANIFEST.md](ASSET-MANIFEST.md)：进入可执行文件的纹理哈希与再分发边界。

## 项目状态

仓库正在按上述规范建立首个可运行版本。文档中的 `Proposed`、`Verified` 和
`Accepted` 是严格状态，不代表完成百分比：没有证据的能力不会因为代码路径存在而被宣称支持。

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

`embedded_unity_textures` 测试逐张锁定内嵌 PNG 的尺寸、字节数和 SHA-256。只有在更新
Unity 真值快照时才需要运行生成器，生成结果会进入源码而不是成为运行时依赖：

```powershell
pwsh -NoProfile -File tools\generate-embedded-unity-textures.ps1 `
  -UnityProjectRoot "D:\path\to\UnityProject"
```

## 许可证

本项目自行编写的代码使用仓库根目录中的 GNU GPL v2 许可证。内嵌的第三方游戏纹理不因本项目
许可证而获得再许可；本地测试与公开分发边界见 [ASSET-MANIFEST.md](ASSET-MANIFEST.md)。
