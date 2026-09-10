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

## Full 发布资产

正式 Release 上传便携 ZIP、便携 ZIP 哈希、安装器和安装器哈希四个 Full 资产；每个资产的最终
SHA-256 以发布目录中的 sidecar 文件和 GitHub Release 上传后复核结果为准。
