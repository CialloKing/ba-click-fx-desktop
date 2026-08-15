# 架构 Spike 验收规范

四个 P0 Spike 都是架构状态从 Proposed 升级和正式发布验收的必要证据；这里的 P0 表示证据不可跳过，
不表示当前体验迭代的执行顺序。当前开发优先级以 [ROADMAP.md](ROADMAP.md) 为准，先收敛延迟诊断、
WGC 成本和三端视觉回归，再集中补齐硬件矩阵。单独新增 collector/verifier 不计作用户功能。
主假设失败不自动阻塞架构接受，但必须有已接受的 fallback 并同步收窄能力矩阵。执行结果保存在
`artifacts/spikes/<spike>/<machine-and-date>/`；本机临时输出放在被 Git 忽略的 `artifacts/local/`。
每次结果必须包含 OS build、GPU/driver、显示器/色彩模式、应用 commit、步骤、原始输出和结论。

## SPK-001 / Spike A：DirectComposition FP16 扩展预乘合成

### 要回答的问题

DXGI premultiplied alpha 的 FP16 surface 经 DirectComposition/DWM 后，`A=0, RGB>0` 与 `RGB>A`
是否按预期形成加法 source-over，且没有被项目自身 pass canonicalize。

### 矩阵

- 普通 SDR；HDR active（若设备支持）。
- 黑、18% 灰、彩色和白背景。
- `(rgb,a)` 至少包含 `(0,0)`、`(0.25,0)`、`(1,0.25)`、`(4,0.5)`。

### Pass

- GPU readback 与最终 shader 期望值在 FP16 容差内。
- 桌面捕获/显示测量与 source-over 方向一致，`A=0, RGB>0` 不被 overlay 内部清零。
- 普通 SDR 白底饱和被记录为产品降级，而不是测试失败或偷偷压暗桌面。

### 已执行证据

- SDR：`Passed`，commit `9f5b777`，Windows `10.0.19045`、RTX 4060 Laptop GPU、
  `G22/P709`、10 bpc。原始结果和复现步骤见
  [`artifacts/spikes/spk-001/rtx4060-win10-19045-sdr-2026-08-14/README.md`](../artifacts/spikes/spk-001/rtx4060-win10-19045-sdr-2026-08-14/README.md)。
- 四种背景、20 次呈现、48 个逐通道公式检查全部通过；最大绝对误差为 `0.001953125`，
  WGC 验证器未记录传输公式降级。黑底 `(0.25,0)`、`(1,0.25)` 和 `(4,0.5)` 的 RGB
  分别保留为 `0.25`、`1.0` 和 `4.0`。
- 该结果不是物理 scanout 测量；白底 GDI 诊断已饱和到 `[255,255,255]`，最终可见 SDR
  余量仍按产品降级边界处理。
- HDR active：`Not Run`。当前只接受 SPK-001 的 SDR 单元格，ADR-001 仍为 `Proposed`。
- 常量通过 `ClearRenderTargetView` 注入生产 FP16 swap-chain；最终 shader 与层公式由当前 FP16
  Golden 独立证明。两类证据共同覆盖 shader 输出和 DComp/DWM 传输边界，不能互相冒充。

复跑真实桌面矩阵时使用：

```powershell
cmake --build --preset alpha-release --target ba_fx_composition_spike
$revision = git rev-parse --short HEAD
$output = "artifacts\local\spikes\spk-001\$env:COMPUTERNAME-$revision"
build\alpha-x64\src\capture\Release\ba-click-fx-composition-spike.exe `
  "--output=$output" `
  "--revision=$revision" `
  --timeout-ms=25000
python -B tools\verify-composition-spike.py `
  "$output\capture.json" `
  "--report=$output\verification.json"
```

collector 使用进程内总 watchdog；自动化调用仍必须设置独立的进程外超时。真实桌面 CTest 默认关闭，
仅在 `BAFX_ENABLE_COMPOSITION_SPIKE_TESTS=ON` 时注册，且固定 `RUN_SERIAL` 与 30 秒超时。

## SPK-002 / Spike B：WGC 生命周期、自排除与录屏

### 场景

- capture 授权成功与拒绝；borderless 请求成功与失败。
- cursor inclusion/exclusion、ContentSize 改变、session restart、窗口/显示器关闭。
- `BackgroundAware` 与 `RecordingCompatible` 模式切换。
- 至少一种桌面捕获与一种窗口捕获录屏路径。

### Pass

- 回调只通知，Render Owner 串行 drain/copy/close；无 frame 生命周期泄漏。
- self-exclusion 不产生递归反馈；失败时退回 FX-only 并有诊断。
- 录屏结果按“观察”记录，不将单一录屏器结论泛化。

### 已执行生命周期子集证据

- 受控窗口生命周期子集：`Passed`，capture/verifier commit `5d56716`，Windows `10.0.19045`、
  RTX 4060 Laptop GPU `32.0.16.1074`。原始结果与复现步骤见
  [`artifacts/spikes/spk-002/rtx4060-win10-19045-window-lifecycle-2026-08-14/README.md`](../artifacts/spikes/spk-002/rtx4060-win10-19045-window-lifecycle-2026-08-14/README.md)。
- 已验证两个不同 `HWND` 上的首帧、`320x240 -> 480x300` ContentSize 变化、显式
  FramePool Recreate、epoch/generation 前进、`item.Closed -> Stopped`、新 Session 重启和幂等 stop。
  共获取并关闭 4 帧；2 个 FramePool、2 个 Session 和两组事件注册全部配平，live/failure 计数为 0。
- collector 的逻辑截止、进程内 watchdog 和调用方超时彼此独立；失败时会原子保存最后阶段和清理后
  ledger。离线验证器拒绝乱序事件、未重建尺寸、未前进 epoch/generation、未归零资源和重复 JSON 字段。
- 该结果不覆盖权限拒绝、无边框、自排除、产品模式切换、外部录屏器、显示器关闭、压力或功耗，
  因此完整 SPK-002 和 ADR-003 仍为 `Not Run` / `Proposed`。

复跑该硬件子集时使用：

```powershell
cmake --build --preset alpha-release --target ba_fx_wgc_lifecycle_spike
$revision = git rev-parse --short HEAD
$output = "artifacts\local\spikes\spk-002\$env:COMPUTERNAME-$revision"
build\alpha-x64\src\capture\Release\ba-click-fx-wgc-lifecycle-spike.exe `
  "--output=$output" `
  "--revision=$revision" `
  --timeout-ms=12000
python -B tools\verify-wgc-lifecycle-spike.py `
  "$output\lifecycle.json" `
  "--report=$output\verification.json"
```

真实桌面 CTest 默认关闭；仅在 `BAFX_ENABLE_WGC_LIFECYCLE_SPIKE_TESTS=ON` 时注册，并固定
`RUN_SERIAL` 和 30 秒超时。离线 `wgc_lifecycle_spike_contract` 始终注册。

### 已执行光标像素子集证据

- 受控窗口光标 inclusion/exclusion 子集：`Passed`，capture/verifier commit `d41b9a0`，Windows
  `10.0.19045`、RTX 4060 Laptop GPU `32.0.16.1074`。原始 FP16、预览、哈希与复现步骤见
  [`artifacts/spikes/spk-002/rtx4060-win10-19045-cursor-pixels-2026-08-14/README.md`](../artifacts/spikes/spk-002/rtx4060-win10-19045-cursor-pixels-2026-08-14/README.md)。
- 同一 `320x240` HWND 依次新建 `included-before -> excluded -> included-after` 三个 Session；每个
  Session 都显式写入并回读 `IGraphicsCaptureSession2::IsCursorCaptureEnabled`，且样本 generation
  和 QPC 时间戳均越过对应 marker。
- 自定义 32x32 单色光标有 176 个不透明像素。两次 included 相对 excluded 都恰好改变 176 像素，
  最大 RGB 差 `1.9711151123046875`；两次 included 之间和远端 control ROI 的差均为 0。三个 Session
  共获取并关闭 6 帧，FramePool、Session 和两类事件注册全部配平，live/failure 计数为 0。
- 离线验证器直接读取三份 `.rgba16f` 重算差异、边界、稳定性和 control ROI；JSON 不能自行放宽
  阈值或伪造汇总。真实桌面 CTest 仅在 `BAFX_ENABLE_WGC_CURSOR_SPIKE_TESTS=ON` 时注册，固定
  `RUN_SERIAL` 与 30 秒超时；离线 `wgc_cursor_spike_contract` 始终注册。
- 该结果不覆盖 monitor capture、其他 DPI/光标主题、权限/无边框、自排除、产品模式切换、外部
  录屏器或多显示器，因此完整 SPK-002 和 ADR-003 仍为 `Not Run` / `Proposed`。

复跑该硬件子集时使用：

```powershell
cmake --build --preset alpha-release --target ba_fx_wgc_cursor_spike
$revision = git rev-parse --short HEAD
$output = "artifacts\local\spikes\spk-002-cursor\$env:COMPUTERNAME-$revision"
build\alpha-x64\src\capture\Release\ba-click-fx-wgc-cursor-spike.exe `
  "--output=$output" `
  "--revision=$revision" `
  --timeout-ms=12000
python -B tools\verify-wgc-cursor-spike.py `
  "$output\cursor.json" `
  "--report=$output\verification.json"
```

### 已执行 WDA 自排除像素子集证据

- 受控主显示器 WDA 动态像素子集：`Passed`，collector commits `5a34c29`、`7dff929`，
  verifier commit `c1c4536`。Windows `10.0.19045.6466`、RTX 4060 Laptop GPU
  `32.0.16.1074`。原始 FP16、预览、哈希与复现步骤见
  [`artifacts/spikes/spk-002/rtx4060-win10-19045-self-exclusion-pixels-2026-08-14/README.md`](../artifacts/spikes/spk-002/rtx4060-win10-19045-self-exclusion-pixels-2026-08-14/README.md)。
- 一个 monitor-WGC Session 内依次执行
  `WDA_NONE -> WDA_EXCLUDEFROMCAPTURE -> WDA_NONE`；每阶段均确认请求值、回读值、
  `WS_EX_LAYERED | WS_EX_TRANSPARENT` 恢复、generation 前进、QPC 新鲜度及两个稳定样本。
- 两次 included 相对 excluded 均改变 overlay ROI 的全部 `36864/36864` 像素，最大 RGB 差
  `0.7938690185546875`。excluded ROI 与同帧远端背景逐像素相同，重复 included 和 control ROI
  差异也均为 0，因此均匀黑色保护面不能冒充成功的自排除。
- 三个阶段各有独立 `64x64` 原始像素 marker；每对 marker 均改变全部 4096 像素。离线 verifier
  从三份 `.rgba16f` 重算所有指标，36 项合同测试覆盖旧帧、伪造指标、弱化阈值、黑块、错误 marker、
  样式丢失、路径逃逸、非有限 FP16 和资源泄漏。
- 共获取并关闭 14 帧；一个 FramePool、一个 Session 和两类事件注册全部配平，live/failure 为 0。
  真实桌面 CTest 仅在 `BAFX_ENABLE_WGC_SELF_EXCLUSION_SPIKE_TESTS=ON` 时注册，固定 `RUN_SERIAL`
  与 30 秒进程超时；离线 `wgc_self_exclusion_spike_contract` 始终注册并有 45 秒硬超时。
- 该单 Session 动态矩阵不验证产品 `Stop sensor -> change WDA -> start sensor` 事务，也不覆盖
  外部录屏器、模式切换、HDR、其他显示器或 device lost。因此完整 SPK-002 仍为 `Not Run`，
  ADR-003/ADR-004 仍为 `Proposed`。

复跑该硬件子集时使用：

```powershell
cmake --build --preset alpha-release --target ba_fx_wgc_self_exclusion_spike -- /m:1
$revision = git rev-parse --short HEAD
$output = "artifacts\local\spikes\spk-002-self-exclusion\$env:COMPUTERNAME-$revision"
build\alpha-x64\src\capture\Release\ba-click-fx-wgc-self-exclusion-spike.exe `
  "--output=$output" `
  "--revision=$revision" `
  --timeout-ms=15000
python -B tools\verify-wgc-self-exclusion-spike.py `
  "$output\self-exclusion.json" `
  "--report=$output\verification.json"
```

### 已执行产品模式切换与暂停保鲜子集证据

- 产品模式切换、暂停保鲜和有效快照失效子集：`Passed`，capture commit `ab4be5a`，Windows
  `10.0.19045`、RTX 4060 Laptop GPU `32.0.16.1074`、`3840x2160 @ 144 Hz` SDR。原始日志、
  配置、IPC transcript、哈希和汇总见
  [`artifacts/spikes/spk-002/rtx4060-win10-19045-mode-switch-snapshot-2026-08-15/README.md`](../artifacts/spikes/spk-002/rtx4060-win10-19045-mode-switch-snapshot-2026-08-15/README.md)。
- `pause-maintenance` 完成
  `background-aware -> recording-compatible -> background-aware -> Pause 2s -> Resume -> click -> Shutdown`，
  控制代次为 `1 -> 2 -> 3 -> 4 -> 5`。无可见内容的暂停阶段有 `32` 次 sensor-only maintenance，
  样本最大年龄 `29131 us`；取得/关闭 `377/377` 帧，最终有 `756` 个背景参与帧。
- `live-snapshot-switch` 先观察到控制代次 1、WGC/快照 `epoch=1, generation=10` 的
  `BackgroundComposite.Participated`，再切换到录屏兼容模式；控制代次 2 随后记录帧外
  `BackgroundSnapshot.Invalidated`，原因为 `capture-disabled`，旧快照仍属于 epoch 1，且已刷新到
  generation 11。旧快照不会跨事务进入第二个 Session。
- 两个场景均以退出码 0 结束；最终 Frame/FramePool/Session 和两类事件注册全部配平，
  `Failures=0`、`AllReleased=true`。旧文本参与标记仍保留，但新的结构化事件是身份和顺序判据。
- 该证据不覆盖权限拒绝、无边框、外部录屏器、显示器关闭、长期压力/功耗、HDR、多显示器或真实
  device lost/reset，也不能证明同步 WinRT `Close()` 可被取消。当前生产 Host 会在 WGC stop 超过默认
  `10 s` 时以退出码 `124` 结束进程，而不是继续 WDA 回滚或复用旧 Session；该进程边界不等同于 WinRT
  清理成功。因此完整 SPK-002 继续为 `Not Run`，ADR-003/ADR-004/ADR-007 继续为 `Proposed`。

### 已执行 portable 无边框拒绝与恢复子集证据

- portable `not-packaged` 拒绝、FX-only 回退和允许系统边框后的恢复子集：`Passed`，capture commit
  `85c1e9b`，Windows `10.0.19045`、RTX 4060 Laptop GPU `32.0.16.1074`、
  `3840x2160 @ 144 Hz` SDR。原始日志、配置、IPC transcript、哈希和汇总见
  [`artifacts/spikes/spk-002/rtx4060-win10-19045-portable-borderless-fallback-2026-08-15/README.md`](../artifacts/spikes/spk-002/rtx4060-win10-19045-portable-borderless-fallback-2026-08-15/README.md)。
- 场景始终保持 `background-aware`，按
  `allowSystemBorder=true -> false -> true` 切换。控制代次 1 的背景样本进入最终复合；代次 2 在创建
  第二个 Session/FramePool 前记录 `WGC.BorderlessAccess.Checked=not-packaged / 0x80073D54 / false`，
  严格执行 stop、自排除、profile、失败 start、清理 stop 和 included 恢复，实际路径为
  `fallback-fx-only`，且没有背景参与事件。
- 代次 2 的 WDA 最终请求/回读均为 `0x00000000`，旧快照以 `capture-disabled` 失效。重新允许边框后，
  控制代次 3 创建第二个 WGC 会话并重新出现背景参与，证明普通 Start 失败没有错误锁住合法请求变化。
- 最终获取/关闭 `33/33` 帧；FramePool、Session、FrameArrived 和 item.Closed 注册均为 `2/2`，
  `Failures=0`、`AllReleased=true`。`Shutdown` 已收到 IPC 确认，Host 退出码为 0，67 个事件序号连续，
  没有 Error/Fatal 或遗留 Host 进程。
- 该结果只覆盖 portable EXE 的 `not-packaged` 早期拒绝，不是 packaged
  `DeniedByUser`/`DeniedBySystem`，也不证明无边框成功、外部录屏、显示器关闭、长期压力/功耗、HDR、
  多显示器或真实 device lost/reset。因此完整 SPK-002 继续为 `Not Run`，
  ADR-003/ADR-004/ADR-007 继续为 `Proposed`。
- 上述归档固定在 capture commit `85c1e9b`，不能证明当前 HEAD 的跨帧授权实现。当前实现已将权限请求移到
  stop/WDA/profile 之前并移除同步 `100 ms` 等待；正式关闭当前 portable 单元格前仍需按相同硬件场景重跑
  collector，packaged 允许/拒绝单元格继续独立保持 `Not Run`。

## SPK-003 / Spike C：Color/HDR 输出

### 场景

- 普通 SDR、HDR active、Advanced Color SDR（设备支持项）。
- 枚举并记录 SDR white level。
- 保存 `Display.ColorMode`、`Display.DxgiColorSpaceValue`、位深和亮度元数据；零亮度元数据按
  `unknown` 处理，不能作为显示器实测亮度。
- FP16 负值、0..1、`>1` 与名义 1000-nit 编码输入。

### Pass

- `ArtisticRelative`、`ReferenceWhiteRelative`、`AbsoluteNits` 的映射路径可在日志和 readback 中区分。
- 没有把 Unity 艺术值标成 nits。
- SDR white saturation、DWM clamp/映射和 HDR 显示观察均明确记录；未知路径保守降级。

## SPK-004 / Spike D：显示拓扑与资源域

### 场景

- 60+144 Hz、144+240 Hz（有硬件才执行）。
- SDR+HDR、iGPU+dGPU、旋转、不同 DPI、热插拔、运行中 HDR toggle。

### Pass

- 每个 monitor 可追溯到 Adapter LUID 和独立资源域。
- 指针跨屏连续，present cadence 不被最慢显示器全局锁定。
- 跨 adapter、热插拔、旋转和 device lost 使用重建路径，无悬挂共享资源。
- 缺少硬件的单元格标为 `Not Run`，不能以模拟结果标成 Passed。

### 当前实现证据

- Host 已对 `DXGI_ERROR_DEVICE_REMOVED`、`DEVICE_RESET`、`DEVICE_HUNG` 和
  `DRIVER_INTERNAL_ERROR` 建立 renderer 级一次性恢复边界，覆盖渲染提交、Bloom 配置资源、
  swap-chain resize 和 WGC FramePool Recreate；恢复失败或同一进程再次遇到 device-lost 时不再
  重试，避免渲染线程进入无界重建循环。恢复后 Adapter LUID 变化会阻止 WGC Start。
- `--device-recovery-probe` 在 FX-only 模式下完成首帧、资源域重建和同快照重渲染，当前 CTest
  `desktop_device_recovery_probe` 通过；日志事件为 `Graphics.DeviceRecovery.Probe.Begin` 和
  `Graphics.DeviceRecovery.Probe.Succeeded`。
- Host 会在 D3D11.4 可用时注册 device-removed event，并把该句柄排在 frame-latency 句柄之前；同时信号
  时设备移除优先，已确认的 device-lost 路由到同一恢复边界。启动与成功恢复后记录
  `Graphics.DeviceRemovalNotification.Status`；性能窗独立统计 `FramePacing.DeviceRemovedWakes`。暂停态也同时
  等待设备移除与 WGC 背景帧，设备事件优先并请求一个恢复帧。
- 通知接口不可用或注册失败时继续使用轮询兜底：frame-latency wait 失败保留原始 Win32 error，连续
  `250 ms` 未取得 FrameReady 时检查 D3D device-removed reason。`desktop_frame_pacing_stall` 以永久不信号
  句柄验证运行截止检查不会被 `TimedOut`/`MessagesPending` 的 `continue` 绕过。
- WGC stop 在 FrameArrived/Closed 退订、Session Close 和 FramePool Close 的调用前后写入阶段检查点，并在
  返回后汇总各阶段与总耗时；渲染阶段的真实 stop 不会被随后无 sensor 清理覆盖，延后交接时记录
  `DeferredReport=true`。当前机器的正常 WGC 会话已产生
  `SensorPresent=true;Completed=true;DeferredReport=false` 的完整汇总记录；阶段异常继续清理、线程一致性和
  双 stop 交接由单元测试覆盖。生产 watchdog 在 stop 开始前启动，默认 `10 s` 截止；独立子进程探针验证
  默认终止处理器会产生精确退出码 `124`。超时不会继续执行 `WDA_NONE`；只有已写入的四个阶段级
  `StageState=begin` 能定位具体阻塞调用，`Stop/begin` 只表示 watchdog 已启动。该边界不会取消同步 Close。
- 当前机器已观察到通知注册及主动资源重建后的重新注册，但该探针没有制造真实 GPU reset，也没有覆盖
  WGC Session/FramePool 在 device-lost 中的系统级行为；
  因此本 Spike 的真实 device lost、跨适配器、热插拔和多显示器单元格仍为 `Not Run`，不能据此发布
  完整硬件支持声明。frame-latency 恢复分支已有模拟覆盖，WGC stop 的生产终止处理器已有独立子进程覆盖，
  但同步 WGC stop 在 device-lost 下是否触发退出码 `124`、实际会停在哪一阶段以及系统资源状态仍无真实
  故障证据。

## 状态模板

```text
Spike:
Commit:
Machine/OS:
GPU/driver:
Displays/color modes:
Steps:
Raw evidence:
Result: Passed | Failed | Not Run
Limitations:
```
