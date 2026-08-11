# ADR-0009：方案 C 本机身份安装通道

- 状态：**Proposed**
- 日期：2026-08-11

## 背景

portable Win32 Host 没有 Package Identity，不能可靠地使用
`graphicsCaptureWithoutBorder`。方案 C 让安装器在目标机生成一张仅用于本机的代码签名证书，
签名 Sparse / External Location Package，再为当前用户注册该包。这样不需要把项目私钥或公用
代码签名证书放进公开发布包。

## 决策

1. 方案 C 是独立的 Identity Installer 通道；portable ZIP 继续保留，作为无安装权限、旧系统和
   Borderless 授权失败时的回退。alpha 测试包不假设拥有 Package Identity。
2. Sparse manifest 使用固定的 `Identity.Name` 和 `Publisher` Subject，声明
   `uap10:AllowExternalContent=true`、`rescap:runFullTrust` 和
   `uap11:Capability Name="graphicsCaptureWithoutBorder"`。Manifest Publisher 必须与本机
   代码签名证书 Subject 完全匹配。
3. 安装阶段先将 Host 文件放入受保护的程序目录，再生成本机非导出私钥；公钥证书只允许加入
   `LocalMachine\\TrustedPeople`，禁止加入 Trusted Root。签名完成后删除私钥容器和临时签名材料。
4. 注册使用 `Add-AppxPackage -ExternalLocation` 或等价的 Windows Packaging API，并记录本次
   安装创建的包全名、证书指纹和外部位置。Host 必须从 Package Activation / 注册的应用入口启动，
   不能把裸 EXE 直启当作已获得 identity。
5. Host 启动时先用 `GetCurrentPackageFullName` 探测 identity。只有存在 identity 且
   `GraphicsCaptureAccess::RequestAccessAsync(Borderless)` 返回 `Allowed` 后，才允许调用
   `IGraphicsCaptureSession3::IsBorderRequired(false)`；任一条件失败都在 `StartCapture` 前回退
     Classic，并记录明确原因。
6. identity 安装版的程序文件目录不可由普通用户写入。运行数据仍限制在程序目录树内，但必须放在
   单独授予用户写权限的 `data` 子目录；不能让用户可写的目录同时承载拥有 Borderless capability
   的外部 Host EXE。
7. 升级必须递增 Package Version，并验证换证、降级、修复和旧包清理；卸载只按安装记录中的包全名
   和证书指纹删除，不能按 Subject 批量删除其他安装实例的证书。

## 明确不承诺

- 自签安装器不会消除 SmartScreen 的 Unknown Publisher 提示。
- `RequestAccessAsync` 的用户同意、Windows 版本支持、DWM 最终像素和无边框效果必须在真实
  Windows 11/目标硬件上验证；离屏测试不能把 ADR 变为 Accepted。
- Windows SDK 中的 `makeappx.exe`、`signtool.exe` 是否可随公开安装器再分发，必须依据其许可和
  安装环境确认；安装器不能假设用户已安装 SDK。

## 验收

- portable Host 的支持报告明确显示 `Package.Identity=absent`，不会请求 Borderless。
- identity Host 能显示 package full name、external location 和证书指纹，并在授权成功后隐藏边框。
- 用户拒绝、系统拒绝、无 capability、直接裸 EXE 启动和外部文件被替换时均不启动无边框会话，
  而是回退 Classic。
- 安装、重启、升级、卸载后不残留包注册、私钥或非本安装实例的证书；程序目录和数据目录权限
  符合上述分离合同。
