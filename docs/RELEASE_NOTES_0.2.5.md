# ba-click-fx-desktop 0.2.5

0.2.5 是增加统一产品版本识别和手动更新检查的正式补丁版（非 prerelease）。本版不改变
特效渲染、配置 schema 或 Profile 数据合同。

## 统一版本识别

Host、Control Center、Windows 文件版本和安装状态现在使用同一份生成的产品版本来源。窗口标题会显示
`BAFX Control Center 0.2.5`，页内标题会显示 `BAFX Desktop 0.2.5` 和当前安装状态。

Host 的 `GetState` 新增规范 `MAJOR.MINOR.PATCH` 格式的 `productVersion`。Control Center 只有在该值
与自身版本完全一致时才开放设置写入；字段缺失、格式错误或版本不同都会显示双方版本并禁用设置控件。
Host 的“启动 Host”与“关闭 Host”入口不受该门限制，用户仍可关闭旧 Host、完成升级后再启动新版本。

Control Center 单实例恢复改用固定窗口类名，不再依赖包含产品版本的动态窗口标题。

## 版本与更新

系统页新增“版本与更新”，分别显示：

- Control Center 版本；
- 已连接 Host 版本；
- 安装状态；
- 最近一次手动查询到的最新公开正式版本。

“检查更新”只在用户点击后查询 GitHub 的最新公开正式 Release。启动 Control Center、连接 Host、刷新
状态或从托盘恢复都不会自动联网。检查结果只用于版本比较；程序不会自动下载 Release 资产、替换文件、
运行安装器或执行网络响应中的命令。

“打开 Release”始终打开固定的
[ba-click-fx-desktop 官方最新 Release 页面](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest)，
不会采用 API 响应中的资产 URL、Release 文本或其他跳转目标。

0.2.4 及更早版本的 Control Center 没有该检查入口，因此第一次升级到 0.2.5 仍需用户手动打开官方
Release 页面，下载安装器或完整便携包。0.2.5 的手动检查也不会代替后续版本的人工下载与安装。

## 安装状态含义

- **安装版**：安装状态完整，产品版本与 Package 版本一致并匹配当前 Control Center，或已从同样
  有效的备份成功恢复。
- **便携版**：主安装状态和备份都不存在；配置与 Profile 位于便携目录约定的位置。
- **安装状态异常**：状态损坏、产品版本与 Package 版本冲突、安装状态属于其他产品版本、只完成
  部分升级，或只剩备份等不完整情况。异常不会被伪装成便携版，应重新运行当前版本安装器修复。

## 配置、Profile 与升级

主配置 schema 保持为 19，不新增迁移。现有 `BAFX.config.json`、`data`、显示器 override、日志和
`fx-profiles` 均保留；安装版升级继续保留 `data`，便携版升级应整体替换程序文件并保留原数据目录。

四个内置 Profile 和自定义 Profile 的行为不变。Profile 仍是严格的 effects-only JSON，只保存或应用
`effects.*`，不包含背景、显示器、输入、性能、系统或 Active-FX ROI。0.2.5 不重写、不删除现有
自定义 Profile。

## Host 热路径优化

Host 渲染循环改用只含配置、配置代次、暂停和退出状态的轻量快照，不再每帧复制 Profile 目录或序列化
候选项。`GetState` 的完整 Profile 目录、活动项和告警合同保持不变。

## 下载哪个包

本次官方 Release 只发布带 Spout2 的 Full 版四个资产：

- `ba-click-fx-desktop-0.2.5-Portable-windows-x64.zip`
- `ba-click-fx-desktop-0.2.5-Portable-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.5-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.5-setup-windows-x64.exe.sha256`

Slim 版继续保留源码构建、测试和本地打包入口，但不发布预编译 Release 资产。便携版没有 Package
Identity，因此不承诺无边框 WGC。安装器仍没有公开代码签名，SmartScreen 显示
`Unknown Publisher` 属于预期。

## 发布验证

2026-08-24 的正式发布验证结果：

- Full `release-verify`：`41/41` 通过。
- Slim `slim-release-verify`：`40/40` 通过。
- Windows SDK 兼容 CI：Windows SDK `10.0.19041.0`、`10.0.22621.0`、`10.0.26100.0`
  三矩阵通过（[run 32703993528](https://github.com/CialloKing/ba-click-fx-desktop/actions/runs/32703993528)）。

## 支持边界

本版增加的是版本识别、混合版本 fail-closed 设置门和用户主动触发的查询入口，不是自动更新器，也不
扩大硬件支持范围。HDR/Advanced Color、多显示器、混合 DPI/刷新率、跨适配器、真实 GPU device lost、
无边框 WGC 跨版本稳定性、完整热插拔矩阵和 Active-FX ROI 硬件矩阵仍为实验项或 `Not Run`。
