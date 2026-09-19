# ba-click-fx-desktop 0.2.19

补齐故障排查日志，便于用户反馈安装、捕获、颜色变化及桌面拖尾问题，同时改善控制中心刷新。

## 更新内容

- **输入与拖尾诊断**：区分鼠标消息到达、读取成功、路由转交及模拟处理；保留失败阶段、错误码、取消来源、采样限频和拖尾起止信息。
- **窗口与呈现诊断**：关联前台切换、覆盖窗口的可见/置顶/cloak 状态、上方相交候选、暂停/电源/调度原因及各显示会话的 Present 计数。会话重建使用独立编号，避免句柄复用造成错误增量。
- **有界日志**：诊断默认启用，无需打开调试模式。输入活动最多每秒汇总，窗口最多每 250 ms 观察，稳定空闲时保留 10 秒心跳；正常退出刷新最后一个窗口。沿用原有轮转与单条记录预算，不记录窗口标题或截图。
- **控制中心**：完整状态读取移至后台，丢弃过期结果；仅更新变化的控件，保留编辑草稿和诊断详情阅读位置。
- **维护**：复用安装/卸载状态处理，拆分主显示器输出协商和有限恢复逻辑，统一特效配置字段读取。

主配置保持 schema 20，支持日志保持 schema 2，现有配置与特效预设无需迁移。
本次增强排查能力，**不表示 Issue #1、#2 已修复**。窗口状态和成功 Present 均不能证明 DWM 最终可见结果；
本次不新增 AMD、HDR、多屏或 26H2/OBS 实机兼容性声明。

## 如何反馈问题

1. 使用 0.2.19 复现，记录操作步骤、故障时间，以及 Windows 版本和显卡/驱动信息。
2. 在控制中心“系统”页点击“打开日志目录”，提供 `ba-click-fx-desktop-support.log`、`BAFX.ControlCenter.log`、仍存在的 `.log.1`–`.log.3` 备份和 `BAFX.config.json`。复现后先保留日志，再清理或重装。
3. 安装或卸载失败时，另附错误框中“详细安装日志”指向的文件；Host 日志无法代替安装器日志。
4. 颜色、截图缺失或特效不可见问题，附切换前后画面，并标注各阶段时间。公开上传前检查日志中的本机路径等信息。

Issue #2 可按“普通窗口激活时正常 → 点击/框选桌面后继续移动 → 再次激活普通窗口”采集，每阶段约 20 秒。
详细字段与判读方法见 [运行时日志排查](https://github.com/CialloKing/ba-click-fx-desktop/blob/v0.2.19/docs/diagnostics/runtime-logging.md)。

## English

Adds diagnostics for user-reported installation, capture, color and desktop-trail issues, and improves Control Center refresh behavior.

- Correlates raw input delivery and failures, routing, simulation sampling, foreground/surface state, scheduling, and per-display Present counts. Session IDs prevent incorrect deltas when Windows reuses a window handle.
- Diagnostics are enabled by default. Input activity is aggregated at most once per second; windows are sampled at most every 250 ms, with 10-second idle heartbeats and a final flush on normal exit. Existing rotation and record budgets remain in effect. Window titles and screenshots are not collected.
- Full Control Center state reads run in the background with stale-result checks. Refreshes preserve editing and diagnostic scroll position. Installer state handling, output negotiation/recovery and configuration parsing share existing implementations.

Configuration schema 20, log schema 2 and effects profiles remain compatible. **Issues #1 and #2 are not declared fixed.** Successful Present calls and window state do not prove final visible DWM pixels. This release adds no new AMD, HDR, multi-display or 26H2/OBS hardware acceptance claims.

To report a problem, reproduce on 0.2.19 and note the steps and time. Use **System → Open log folder** to collect both Host and Control Center logs, their remaining rotated backups, and `BAFX.config.json`. Installation/uninstallation failures require the separate detailed log referenced by the error dialog. For visual issues, include before/after images or a recording. Check local paths and other personal information before posting logs publicly.

## 验证 / Validation

- Windows SDK `10.0.28000.0`：Full/Slim Release 干净构建通过，无编译器警告或错误；CTest Full **46/46**、Slim **45/45** 全部通过。
- 文档、便携包清单/版本/哈希/PE 依赖、Host 与控制中心启动检查通过；安装器版本、PE 依赖及未签名身份模板检查通过。
- 从实际便携 ZIP 解包运行演示，确认 0.2.19 版本、新增诊断事件、模拟计数、Present 及正常退出末次刷新。未发现重复字段、超限记录或诊断格式化失败。
- 安装器自动验证不等同于反馈者机器上的安装/升级/卸载验收；演示关闭 Raw Input，不构成 Issue #1/#2 或最终可见像素的实机验收。

Full/Slim clean Release builds passed on SDK `10.0.28000.0` without compiler warnings or errors; CTest passed **46/46** and **45/45**. Documentation, Portable contents/version/hashes/dependencies/startup, and installer version/dependencies/unsigned identity-template checks passed. A demo run from the actual Portable ZIP verified the new diagnostics and final flush. Installer automation and the demo do not establish affected-machine installation/upgrade/uninstallation or Issue #1/#2 acceptance.

## 下载 / Downloads

Full 版安装器、便携 ZIP 及各自的 SHA-256 校验文件。Slim 完成构建与测试，不上传预编译资产。

Full installer and Portable ZIP, each with a SHA-256 sidecar. Slim is built and tested without prebuilt release assets.
