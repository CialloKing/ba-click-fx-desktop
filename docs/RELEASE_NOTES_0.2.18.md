# ba-click-fx-desktop 0.2.18

改善显示状态刷新，并整理 Host、控制中心与配置模块，降低后续维护成本。

## 更新内容

- **显示页刷新**：状态查询和 JSON 解析移到单个后台线程，合并重复请求并丢弃过期结果，减少界面线程等待。仅在已连接且页面可见、未最小化时查询；完整刷新保留 Host 版本与配置代次校验，配置写入等操作仍采用原有同步流程。
- **多显示器维护**：拆出副显示器捕获、恢复和渲染逻辑，主副显示器复用视觉配置映射；显示快照与输出诊断各自独立，保留渲染线程状态所有权及发布顺序。
- **控件与配置**：统一滑块绑定和页面控件描述，集中注册 41 个特效配置字段，复用允许字段、补丁分派和 JSON 输出，保留参数联动及历史迁移规则。
- **恢复状态**：集中维护 WGC 重试身份和待处理状态，主副显示器共用有限输出重试预算并保留溢出保护。
- **开发与验证**：应用和测试复用构建目标，拆出启动选项和性能样本转换；CI 增加自动测试与文档检查，补齐中英文开发指南，整理当前路线图并归档历史记录。

主配置保持 schema 20，IPC 命令与特效预设格式保持兼容，现有配置无需迁移。本次发布不新增多屏、HDR 或 26H2/OBS 录屏实机兼容性声明。

## 验证范围

- Windows SDK `10.0.28000.0`：Full 与 Slim Release 干净构建通过，无编译警告或错误。
- Full CTest 46/46、Slim CTest 45/45 通过，包括后台请求合并、过期结果丢弃和 Host 恢复状态检查。
- 文档检查、便携包文件清单、版本、PE 依赖与 Host/控制中心启动验证通过；安装器版本、未签名身份模板和 PE 依赖检查通过。

## English

Improves display-status refresh and separates Host, Control Center and configuration responsibilities to reduce maintenance costs.

- Display-state IPC and JSON parsing run on one background worker. Duplicate requests are coalesced and stale results discarded. Polling runs only while connected with the display page visible and not minimized. Full refreshes retain Host version and configuration-generation checks; writes and other actions remain synchronous.
- Secondary-display capture, recovery and rendering have dedicated modules. Displays share visual configuration mapping and finite retry budgets, while render-thread ownership and snapshot publication order remain unchanged.
- Slider bindings and page control descriptors are centralized. A registry of 41 effects fields drives allowed keys, patch dispatch and JSON output while preserving coupled parameters and historical migration rules.
- WGC retry identity, pending state and overflow handling are grouped. Applications and tests share build targets; startup options and performance sample conversion have separate modules.
- CI adds automated tests and documentation checks. Bilingual development guides and the current roadmap have been updated, with historical records archived.

Configuration schema 20, IPC commands and effects profiles remain compatible. This release adds no new hardware acceptance claims for multiple displays, HDR or 26H2/OBS recording.

Full and Slim clean Release builds passed with Windows SDK `10.0.28000.0`, without compiler warnings or errors. CTest passed 46/46 for Full and 45/45 for Slim. Documentation, Portable ZIP contents/version/PE dependencies and Host/Control Center startup checks passed. Installer version, unsigned identity template and PE dependency checks also passed.

## 发布资产 / Downloads

Full 版安装器、便携 ZIP 及各自的 SHA-256 校验文件。Slim 完成构建与测试，不上传预编译资产。

Full installer and Portable ZIP, each with a SHA-256 sidecar. Slim is built and tested without prebuilt release assets.
