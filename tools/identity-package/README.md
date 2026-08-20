# 方案 C Identity Installer Spike

该目录保留 Sparse / External Location Package 的开发 Spike 和发布机构建模板工具。
正式普通用户安装事务由 `tools/installer/` 与 `BAFX.IdentitySigner.exe` 负责；不要把本目录的
CurrentUser 签名/安装脚本当作面向普通用户的正式发布链。
它不携带 SDK、Windows App SDK 或游戏/Unity 美术资源；三个包图标由脚本生成的透明占位 PNG 提供。

## 构建签名包（不改系统信任）

使用 `pwsh.exe`：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\identity-package\build-identity-package.ps1 `
  -HostExecutable .\build\x64\src\desktop\Release\ba-click-fx-desktop.exe `
  -OutputDirectory .\artifacts\local\identity-spike
```

脚本会：

1. 生成 `uap10:AllowExternalContent` Sparse manifest；
2. 在 `CurrentUser\My` 创建临时代码签名证书；
3. 使用 Windows SDK `makeappx.exe` 和 `signtool.exe` 打包签名；
4. 导出公钥 `.cer` 和安装元数据；
5. 默认删除私钥。

## 安装注册

注册动作必须使用管理员 Windows PowerShell 5.1（`powershell.exe`），而不是 `pwsh.exe`：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\identity-package\install-identity-package.ps1 `
  -SourceDirectory .\build\x64\src\desktop\Release `
  -DisableSystemBorder `
  -Launch
```

正式安装目录默认位于 `%ProgramFiles%\ba-click-fx-desktop`。`data` 子目录授予当前用户写权限，
Host 和 Sparse 包文件保持管理员/SYSTEM 保护。安装器会先运行一次有界的 `--support-info` 引导，
以生成 schema 正确的配置；如果这次直接启动尚未获得 Package Identity，产生在 EXE 根目录的
配置和日志会在注册前搬迁到 `data`，不会留下第二份运行时数据。只有一次性本机 Spike 才应使用
`-AllowUserWritableInstall`，并且该路径不具备生产安全性。

安装器只把公钥证书放入 `LocalMachine\TrustedPeople`，绝不写入 Trusted Root。安装记录位于
`data\identity-install.json`，包含包全名和证书指纹，供卸载时精确清理。

## 卸载

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\identity-package\uninstall-identity-package.ps1
```

默认只撤销包注册、删除本安装生成的证书和 `Identity` 目录，保留配置/日志。卸载器会等待并验证
`Get-AppxPackage` 不再返回该包；如果注销失败，会保留安装目录并报告残留，避免留下指向已删除 EXE
的注册。确认不再需要用户数据和整个程序目录时，同时显式加入 `-RemoveUserData -RemoveProgramFiles`；
卸载器要求这两个开关一起出现，避免误删程序目录内仍需保留的配置。
对使用 `-AllowUserWritableInstall` 的一次性 Spike，删除整个目录时还需在卸载命令中重复加入
`-AllowUserWritableInstall`。

卸载器以 `data\identity-install.json` 和 `Identity\*.identity.json` 校验包全名、外部目录及证书
指纹；缺少这些记录时不会递归删除用户数据或不明的 `Identity` 目录。若安装过程中途失败，先
处理脚本报告的残留注册，再重新运行安装。

Windows 10 可以安装/注册的结果取决于系统的 Appx capability 合同；`graphicsCaptureWithoutBorder`
和最终无黄色边框必须在支持该能力的 Windows 11/硬件上实测。用户拒绝或系统不支持时，Host 会在
`StartCapture` 前回退内部 FX-only transport。
