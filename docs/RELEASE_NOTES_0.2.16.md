# ba-click-fx-desktop 0.2.16

修复录屏兼容构建、诊断日志和无边框包身份读取。

## 修复内容

- 按选中 Windows SDK 的实际 C++/WinRT 编译能力探测 `WindowId` 及其集合接口，SDK 28000 的 Full、Slim 构建均包含完整 Session-local 排除代码。
- 区分编译支持、运行时接口探测、配置读回和帧版本确认；失败日志保留具体阶段与 HRESULT，回退日志保留 Session-local 失败证据。
- 使用 `PACKAGE_INFORMATION_FULL` 读取发布者，并从 Kernel32、KernelBase 依次解析 `GetCurrentPackagePath2`，避免阻断有效安装身份的无边框授权。

## 验证范围

- Windows SDK `10.0.28000.0`：Full、Slim Release 构建通过；Slim CTest 44/44 通过，Full CTest 通过（热键边界测试单独重跑通过）。
- 26H2/OBS 实机录制效果仍需目标系统验证；本地 Windows 10 19045 构建不能替代该验收。

## English

Fixes recording-compatible builds, diagnostics, and packaged identity reads.

- Probes the selected Windows SDK's C++/WinRT `WindowId` and collection support, keeping the complete Session-local exclusion implementation in SDK 28000 Full and Slim builds.
- Separates compile support, runtime interface probing, configuration readback, and frame iteration confirmation, while preserving failed Session-local evidence during fallback.
- Reads publisher data with `PACKAGE_INFORMATION_FULL` and resolves `GetCurrentPackagePath2` from Kernel32 and KernelBase so valid installed identities can reach borderless capture authorization.

Full and Slim Release builds were validated with Windows SDK `10.0.28000.0`; Slim CTest passed 44/44 and the Full suite passed after an isolated retry of a timing-sensitive hotkey boundary test. Real 26H2/OBS recording remains unverified on target hardware.

## 发布资产

提供 Full 版安装器、便携 ZIP 及各自的 SHA-256 校验文件。Slim 保留源码构建与本地验证，不上传预编译资产。
