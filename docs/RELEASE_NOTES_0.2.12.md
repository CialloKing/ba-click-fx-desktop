# ba-click-fx-desktop 0.2.12

0.2.12 是证书校验作用域修正版。目标机自签名证书只影响无边框 WGC 捕获；Host 启动、FX-only 和其他捕获路径
不再因证书过期而被 Control Center 拦截。

## 变更

- 无边框 WGC 请求继续校验 Package Identity、Package 签名和目标机证书。
- 校验失败时在 `StartCapture` 前回退到 FX-only，并保留诊断信息。
- Control Center 可以继续启动安装版 Host；证书状态只用于提示无边框 WGC 回退和修复方式。

## 发布边界

- 正式 Release 只提供 Full 版便携 ZIP、ZIP 哈希、安装器和安装器哈希四个资产。
- Slim 只保留源码构建和本地验证，不上传预编译资产。
- Windows 11 目标硬件上的无边框授权、DWM 最终像素和真实用户授权未在本版本验收，不能据此宣称新增硬件支持。

## 本地验证

- Full `release-verify`：`45/45`。
- Slim `slim-release-verify`：`44/44`。
- 安装器候选完成未签名 Sparse 模板、原生签名器、Inno Setup 和 PE 静态依赖检查。

## Full 发布资产候选

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `ba-click-fx-desktop-0.2.12-Portable-windows-x64.zip` | 1,436,637 | `40E1D3FFC5BBC7D9034EE2C656BA1827C73ED7A1AC6CE5E02FE52FC99E9A2FBF` |
| `ba-click-fx-desktop-0.2.12-Portable-windows-x64.zip.sha256` | 117 | `35A2AA651BE1EE159B7EA7078E25C90B1EBD25C32999263177DCC1356358ECC3` |
| `ba-click-fx-desktop-0.2.12-setup-windows-x64.exe` | 4,111,819 | `604C484D71F319201624EA7ED1062A304417081C0609CF1F3D0F10DD3E6444F7` |
| `ba-click-fx-desktop-0.2.12-setup-windows-x64.exe.sha256` | 114 | `88AC087515146578F73C7B4D60B2FA6C1139847E18B002E020B651E8A5688E8A` |
