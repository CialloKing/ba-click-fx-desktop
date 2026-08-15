# SPK-002 portable 无边框拒绝与恢复子集 / 2026-08-15

- Spike：`SPK-002 / portable borderless rejection, FX-only fallback and recovery subset`
- 采集提交：`85c1e9b`
- 系统：Windows 10 Pro for Workstations `10.0.19045`，x64
- GPU/驱动：NVIDIA GeForce RTX 4060 Laptop GPU，`32.0.16.1074`
- 显示输出：`3840x2160 @ 144 Hz`，SDR
- Host：`0.1.0-alpha.12`
- 结果：**portable 无边框拒绝、FX-only 回退和允许边框恢复子集通过**

## 场景

模式始终保持 `background-aware`，只切换 `background.allowSystemBorder`：

```text
allowSystemBorder=true -> injected click -> background participation
-> allowSystemBorder=false -> portable rejection -> FX-only fallback
-> first visible batch retires
-> allowSystemBorder=true -> injected recovery click -> background participation
-> Shutdown
```

使用两个独立点击批次是有意的：背景路径在一个可见批次内禁止从 FX-only 升级，避免复合合同中途闪变。
因此恢复 WGC 后先让失败批次结束，再用新点击证明新的 capture contract 可以正常取得背景。

## 结果

- 初始点击在控制代次 `1` 产生 `BackgroundComposite.Participated`：事件 `20`、帧 `1887`，
  WGC/快照均为 `epoch=1, generation=8`。
- 关闭系统边框后，事件 `31` 记录 `WGC.BorderlessAccess.Checked`：
  `not-packaged / 0x80073D54 / Allowed=false`。该 portable 身份检查发生在第二个 Session 或
  FramePool 创建前。
- 失败事务严格执行
  `stop-sensor -> set-affinity-excluded -> apply-overlay-profile -> start-sensor(false)
  -> stop-sensor -> set-affinity-included`。最终 WDA 为
  `Requested=0x00000000;Observed=0x00000000`，控制代次 `2` 保持
  `captureMode=background-aware`，实际路径为 `fallback-fx-only`。
- 失败事务资源账本为 `17/17` 帧、一个 FramePool/Session 和两类事件注册全部释放，
  `Failures=0`、`AllReleased=true`。控制代次 `2` 没有背景参与事件；事件 `44` 以
  `capture-disabled` 失效旧的 `epoch=1, generation=10` 快照。
- 重新允许系统边框后，四步启动事务全部成功。恢复点击在控制代次 `3` 产生事件 `60`、帧
  `10003`，WGC/快照均为 `epoch=2, generation=8`，证明失败请求没有错误锁住 retry token
  或合法配置变更。
- 两次注入点击共记录 `4` 条 Raw Input 和 `4` 个按钮边沿。最终 WGC 获取 `33` 帧、接受
  `18` 个样本，样本最大年龄 `10866 us`，背景参与 `6` 帧。
- `Shutdown` 返回 `OK {"shutdownRequested":true}`，Host 退出码为 `0`。最终 `33/33` 帧、
  `2/2` FramePool、`2/2` Session、`2/2` FrameArrived 注册和 `2/2` item.Closed 注册全部
  配平，`Failures=0`、`AllReleased=true`；仅有一个 `Process.Exited`，67 个事件序号连续。

## 原始证据

- `ba-click-fx-desktop-support.log`
  - SHA-256：`5E5B3DC525B566E40A38AE802650CF5570776BBF9D12B257697266A74ED6A0B6`
- `BAFX.config.json`
  - SHA-256：`8D00E9BB3FE112AF3965F9682403B347DBEA48054D284399ECD7A645C81741E7`
- `ipc-transcript.txt`
  - SHA-256：`CC98C6A8CA3BAFFC775F1400A181BBE7303E8EBD7707F3913EB7FE1430C0DE03`
- 被测 `ba-click-fx-desktop.exe`
  - SHA-256：`0448B03E3370A0D170163B6AB20D3A0375816FCDF34C0087655975FEEE29F304`
- `verification.json` 汇总状态、事件身份、动作序列和最终资源账本。

所有 Host、IPC、状态轮询和退出步骤均有独立硬超时；采集后未遗留 Host 进程。日志观察器以
`FileShare.ReadWrite | FileShare.Delete` 打开文件，未阻挡被测进程追加事件。

## 限制

- 这是 portable EXE 的 `not-packaged` 早期拒绝证据，不是 packaged
  `DeniedByUser`/`DeniedBySystem` 权限拒绝证据，也不证明无边框成功路径。
- 未使用外部桌面或窗口录屏器，不证明录屏可见性。
- 未覆盖长期压力、功耗、显示器关闭、热插拔、真实 device lost/reset 或同步 WinRT
  `Close()` 永久不返回。
- HDR、Advanced Color、多显示器、混合刷新率/DPI 和多适配器仍为 `Not Run`。
- 完整 SPK-002 继续为 `Not Run`；ADR-003、ADR-004 和 ADR-007 继续为 `Proposed`。
