# ba-click-fx-desktop 0.2.13

控制中心新增英文界面，可在“系统 → 系统行为 → 界面语言”选择跟随系统、简体中文或 English，立即切换，无需重启。

## 主要更新

- 默认跟随 Windows 当前用户的显示语言：中文使用简体中文，其余语言使用英文。
- 五个页面、高级子页面、托盘菜单、应用弹窗、状态详情和已知错误说明支持双语；原始诊断详情、硬件名称和自定义预设名称保持原值。
- Host 未启动、断开或版本不兼容时仍可切换语言。切换保留页面、显示器、预设、未完成的输入和快捷键草稿。
- 内置预设显示英文名称，如 `Unity Original`、`Lightweight`、`Click Only`、`Trail Only`，操作仍使用原有内部名称。
- 系统页增加语言选项，将日志清理移至“版本与维护”；调整英文长标签、页签和预设按钮布局，保留 `860 × 600` 最小逻辑客户区。

## 保存与升级

语言独立保存在 `BAFX.ControlCenter.language`，内容为 `auto`、`zh-CN` 或 `en-US`。便携版位于程序目录，安装版位于 `data` 目录。
重启后保留选择，“重置默认”保留语言偏好。语言切换不写入 Host 配置；主配置继续使用 schema 20，IPC 和特效预设格式均无需迁移。

更新时请使用完整安装器或便携包，保持 Host 与 Control Center 版本一致。新安装和升级均默认跟随系统，发行包不包含本机语言偏好。

## English

The Control Center now supports English and Simplified Chinese. Open **System → System behavior → Language** to switch immediately, even when Host is stopped or incompatible.

- The default follows the current Windows user's display language: Chinese uses Simplified Chinese; all other languages use English.
- Pages, advanced settings, tray menus, application dialogs, status details and known error explanations are localized. Custom profile names and original diagnostic details are preserved.
- Switching languages preserves the current page, display/profile selection, unfinished input and hotkey drafts.
- The preference persists in `BAFX.ControlCenter.language`, beside the portable executables or in the installed `data` folder. Reset defaults preserves it.
- Host configuration schema 20, IPC and effect profile formats are unchanged. Update Host and Control Center together.

## 发布资产与范围

正式 Release 提供 Full 版安装器、便携 ZIP 及各自的 SHA-256 校验文件。Slim 保留源码构建与本地验证，不上传预编译资产。
本次新增控制中心语言支持，不改变现有渲染与 Windows 捕获能力的支持范围。
