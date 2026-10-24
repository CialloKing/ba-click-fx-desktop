# 程序规则验证 / Program rules validation

日期 / Date: 2026-10-24。0.3.2 程序规则验证；发布资产检查见 [VALIDATION.md](../VALIDATION.md)。
Program rules validation for 0.3.2; release asset checks are recorded in [VALIDATION.md](../VALIDATION.md).

## 自动验证 / Automated checks

- 配置迁移、重复规则、匹配、多实例、未知检测、两次退出确认、名单修订丢弃旧结果。
- Host 初次命中、手动暂停 / 恢复、重置特效保留名单、规则修改不增加渲染代次。
- 所有背景模式在自动停用时产生合法的无采集请求。
- 控制中心中英文、96 / 144 / 192 DPI 六个页面的布局边界与命中状态展示。
- Config migration, duplicates, matching, multiple instances, unknown results, two clear observations and stale revisions.
- Initial Host suppression, independent manual intent, reset preservation and stable render generation for rule edits.
- Valid no-capture requests in every background mode; bilingual native UI at 96 / 144 / 192 DPI.

## 构建与回归 / Builds and regression

- Full / Slim Release 均构建成功（Visual Studio 2026、Windows SDK 10.0.28000.0）。
- 0.3.2 发布前完整重建后的 CTest：Full **47/47**（110.76 秒），Slim **46/46**（100.82 秒），没有跳过或禁用测试。
- 候选提交 `182bdc4` 的[远端 CI](https://github.com/CialloKing/ba-click-fx-desktop/actions/runs/40082026888) 通过 42/42 自动测试、SDK 19041 / 22621 / 26100 构建和文档检查。
- 实现阶段曾出现 `hotkey_process_boundary` 在第 234 行等待注入的 Ctrl+Alt+Shift+F22 超时，当时 Full 为 46/47、Slim 为 45/46。此次本地 Full / Slim 和远端 CI 均通过；快捷键实现与该测试相对基线 `5b990e5` 未修改，未放宽断言，先前超时的根因未确认。
- 初次检测异常和后台界面刷新的收尾修改已包含在此次完整重建与回归中。
- `node tools/verify-readme.mjs`：通过；ROI 采集及报告工具扩展接受 schema 21，原有 schema 19 / 20 证据仍可读取。
- Both Release variants were clean-built. Full CTest: **47/47** (110.76 s); Slim: **46/46** (100.82 s), with no skipped or disabled tests. Candidate CI passed 42/42 tests, three SDK builds and documentation checks.
- Earlier implementation checks had a native hotkey injection timeout at line 234 (Full 46/47, Slim 45/46). The release checks now pass locally and in CI without changing the hotkey implementation or weakening its assertions; the earlier timeout's cause remains unconfirmed. Final startup-failure and background-refresh adjustments are included in the complete release regression.

## 本机进程与图形检查 / Local process and graphics checks

环境：Windows 10 build 19045、NVIDIA GeForce RTX 4060 Laptop GPU、单屏 2560×1440、约 165 Hz、SDR、150% DPI。
使用 `build/application-rules-runtime` 内的独立程序包和配置，测试进程由该测试创建和结束，未修改安装版或用户原有配置。

- 程序先于 Host 启动时即命中，覆盖层保持隐藏，WGC 未运行。
- 同一路径的两个实例只退出一个时继续停用；两个不同程序分别命中时，剩余一个仍保持停用。
- 所有实例退出后恢复覆盖层可见状态与 WGC；检测期间手动关闭特效、暂停 / 恢复均保留手动意图。
- 关闭总开关、重新启用规则、删除最后一条命中规则均生效，控制中心未运行时检测正常。
- 活跃演示粒子在命中时被清空。Spout2 接收器连续 **108/108** 个采样保持连接，共享句柄始终相同；停用后的 RGB / Alpha 像素均为零，恢复和再次停用后没有旧粒子重现。
- 运行状态与窗口记录：`build/application-rules-runtime/states.json`；Spout 像素及连接证据：`spout.json`；汇总：`summary.json`。这些是本地测试产物，不属于发布包。

On this machine, initial suppression, multiple instances / programs, live rule edits, manual intent, hidden / restored overlay HWNDs and stopped / resumed WGC passed in an isolated bundle without Control Center.
The Spout2 receiver remained connected for **108/108** samples with one unchanged shared handle. After suppression, every sampled pixel remained transparent, including after resume and a second suppression cycle.

## 尚未完成 / Not run

- 多显示器硬件、HDR、Windows 11 异步无边框授权及设备拔插；已复用原状态机并检查所有请求入口，但不能替代对应实机验收。
- OBS 应用内的最终合成 / 录制与人工桌面最终像素验收。本轮通过 Spout2 接收器验证共享输出、通过 HWND 状态验证显示 / 隐藏；不把这些结果当作 OBS 录制或 DWM 最终像素验收。

Multi-display hardware, HDR, Windows 11 borderless permission / hotplug, final OBS composition / recording and human inspection of final desktop pixels remain unverified. Receiver readback and HWND visibility do not substitute for that acceptance.
