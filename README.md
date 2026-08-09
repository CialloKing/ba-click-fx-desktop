# ba-click-fx-desktop

`ba-click-fx-desktop` 是从零实现的 Windows 原生桌面点击特效。项目不复用
`ba-click-fx` 的 JavaScript、WebGL 或 WebGPU 渲染代码；Unity/游戏资源是视觉真值，
Web 版本只作为行为与参数语义参考。

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

## 项目状态

仓库正在按上述规范建立首个可运行版本。文档中的 `Proposed`、`Verified` 和
`Accepted` 是严格状态，不代表完成百分比：没有证据的能力不会因为代码路径存在而被宣称支持。

本地可用下列命令核对 Unity 外部证据。脚本只读取并校验哈希，不会复制或修改游戏资产：

```powershell
cmake --build build\vs2026 --target verify_unity_reference
```

## 构建与测试

首版固定使用 C++20；本机验证工具链为 Visual Studio 2026 与 Windows SDK 10.0.26100：

```powershell
cmake -S . -B build\vs2026 `
  -G "Visual Studio 18 2026" -A x64 `
  "-DCMAKE_SYSTEM_VERSION=10.0.26100.0"
cmake --build build\vs2026 --config Debug --parallel
ctest --test-dir build\vs2026 -C Debug --output-on-failure
```

DirectComposition smoke test 需要交互式桌面，因此默认不进入普通 CTest：

```powershell
cmake --build build\vs2026 --config Debug --target smoke_desktop
```

当前 Windows 骨架已创建 PMv2、无激活/穿透 overlay、D3D11 FP16 premultiplied swap chain 和
DirectComposition visual。透明 smoke 只验证创建、present 和销毁，不等同于 SPK-001 已通过。

## 许可证

本项目使用仓库根目录中的 GNU GPL v2 许可证。
