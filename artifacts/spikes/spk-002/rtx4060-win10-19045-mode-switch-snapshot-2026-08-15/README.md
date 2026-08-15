# SPK-002 产品模式切换与快照失效子集 / 2026-08-15

- Spike：`SPK-002 / product mode-switch and paused-maintenance subset`
- 采集提交：`ab4be5a`
- 系统：Windows 10 Pro for Workstations `10.0.19045`，x64
- GPU/驱动：NVIDIA GeForce RTX 4060 Laptop GPU，`32.0.16.1074`
- 显示输出：`3840x2160 @ 144 Hz`，SDR
- Host：`0.1.0-alpha.12`
- 结果：**模式切换、暂停保鲜和有效快照失效子集通过**

## 场景

`pause-maintenance` 执行：

```text
background-aware -> recording-compatible -> background-aware
-> Pause 2s -> Resume -> delayed demo click -> Shutdown
```

该场景的控制代次为 `1 -> 2 -> 3 -> 4 -> 5`。暂停且没有可见特效时执行了
`32` 次 sensor-only maintenance；最终取得并关闭 `377/377` 个 WGC frame，背景样本
最大年龄为 `29131 us`，`756` 个帧实际使用背景快照。首次结构化参与事件为 WGC/快照
`epoch=2, generation=147`，批次结束时以 `visible-batch-ended` 失效。

`live-snapshot-switch` 在日志观察到首次 `BackgroundComposite.Participated` 后立即切换：

```text
background-aware with live snapshot -> recording-compatible
-> background-aware -> Pause 2s -> Resume -> Shutdown
```

首次参与发生在控制代次 `1`、帧 `2522`，WGC/快照身份均为 `epoch=1,
generation=10`。切到录屏兼容模式后，控制代次 `2` 产生帧外
`BackgroundSnapshot.Invalidated`，原因为 `capture-disabled`，旧快照仍属于 `epoch=1`；
当时 producer 与快照已刷新到 generation `11`。该顺序证明模式切换先使有效旧快照失效，
随后才进入 FX-only 合同。

两个场景均以 Host 退出码 `0` 结束，最终资源账本均为 `Failures=0`、
`AllReleased=true`。两套 WGC Session、FramePool、FrameArrived 注册和 item.Closed 注册全部配平。

## 原始证据

- `pause-maintenance/ba-click-fx-desktop-support.log`
  - SHA-256：`4A3CEB0250338411916D23D0DB1599081D74A7E97B915436639063F87A5EAFBE`
- `live-snapshot-switch/ba-click-fx-desktop-support.log`
  - SHA-256：`8DFAE6023D97E95CD272F523192BC7B69909846608F2702A0F4D702FE79488B2`
- 两个 `BAFX.config.json`
  - SHA-256：`8D00E9BB3FE112AF3965F9682403B347DBEA48054D284399ECD7A645C81741E7`
- 被测 `ba-click-fx-desktop.exe`
  - SHA-256：`87E300D32048272F95756CB973FD5ECD6B999016D3FB8A34C69F0FA19F082C40`
- `verification.json` 汇总结构化事件、性能窗口和最终资源账本。
- 两个 `ipc-transcript.txt` 保留控制代次、模式和正常退出回执。

所有 Host、IPC、构建和测试步骤均有外层硬超时；未遗留 Host 进程。

## 限制

- 这是本机 SDR 单显示器上的产品模式切换与暂停保鲜子集，不是完整 SPK-002。
- 本证据允许系统捕获边框，不证明无边框权限允许/拒绝路径。
- 未使用外部桌面或窗口录屏器，不证明录屏可见性。
- 未覆盖长时间压力、功耗、显示器关闭、热插拔、真实 device lost/reset 或同步 WinRT
  `Close()` 永久不返回。
- HDR、Advanced Color、多显示器、混合刷新率/DPI 和多适配器仍为 `Not Run`。
- ADR-003、ADR-004 和 ADR-007 继续保持 `Proposed`。
