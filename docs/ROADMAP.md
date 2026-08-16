# 开发路线图

本路线图规定开发顺序；[VALIDATION.md](VALIDATION.md) 规定证据合同，
[SPIKES.md](SPIKES.md) 规定发布前的硬件/API 验收。自 2026-08-14 起，当前迭代优先解决
用户可感知的输入延迟、渲染成本和视觉差异；2026-08-15 的优先级覆盖进一步暂缓第三阶段
WGC/ROI 优化，当前直接收敛 WGC/背景感知可靠性。2026-08-16 的最新覆盖把 HDR、多显示器、
DPI 和 Windows 11 运行时逻辑提前到测试与硬件证据之前，不再按 Spike 编号顺序扩张采集工具。

完成新的 collector、verifier 或证据归档，只计作验证基础设施进展，不能单独计作用户功能更新。
它们只有在解除当前体验问题或正式发布门槛时才进入主线排期。

## 当前优先级覆盖（2026-08-16）

当前先完成新功能的生产代码逻辑，测试与真实硬件矩阵后置：

- HDR 是用户显式启用的最终输出选项，默认关闭；Unity authored color、粒子、Bloom 和背景合成
  始终保持线性 FP16，只有最终交换链与输出 shader 区分 SDR/HDR；
- 优先完成每显示器输出、独立 WGC 会话、混合 DPI/刷新率、热插拔、跨适配器和 Windows 11
  Advanced Color/动态刷新率的运行时状态机；
- Windows 10/11 使用同一个完整二进制。新系统 API 通过运行时能力或稳定 ABI 探测，旧系统走有界
  fallback；测试不得仅因运行系统缺少 Windows 11 能力而失败；
- 当前不为这些 Windows 11 分支扩张测试、collector 或 verifier。未在真实硬件执行的矩阵仍保持
  `Not Run`，不能据代码完成宣称正式支持；
- 测试版配置只接受字段完整的当前 `schemaVersion=13`，不补齐缺失字段、不迁移非当前 schema，
  也不保留未知字段或枚举别名；
- 每项逻辑保持独立中文提交，只做有硬超时的编译或静态检查，避免构建和外部命令无界等待。

当前生产代码已经完成这一轮显示运行时收敛：Host 会验证主线程实际运行在 Per-Monitor V2
物理像素上下文，DPI 查询失败或返回零值时保留上一次完整拓扑；显示源跨适配器变化时，会话按
`HMONITOR`/GDI 逻辑槽保持位置连续，但 D3D/WGC 资源域仍严格按目标 Adapter LUID 重建；异步迁移会
锁存目标屏的 HDR/Advanced Color 能力，提交后查询短暂失败不会立即撤销已经选择的输出合同；DXGI
色彩元数据只接受与 DisplayConfig 目标适配器唯一匹配的输出，重复 `HMONITOR`、跨适配器克隆或
`IDXGIOutput6` 缺失时继续使用保守的 DisplayConfig 路径。Advanced Color 查询瞬时失败会保留同一输出
最后有效的能力快照，并按显示维护节拍有限重试，不会在主循环中反复探测或立刻把 HDR 传输降回 SDR。

WGC 报告旋转/模式尺寸领先于 Shell 通知时，Host 会立即请求权威拓扑刷新并先重建 FramePool；完整
DisplayConfig 快照若确认输出已跟随新尺寸，则继续正常迁移，若仍不一致则终止该 WGC 会话并回退
FX-only，避免永久保留无法复合的 producer。已知 120/144 Hz 或 DRR boost 目标会按真实周期请求
`IGraphicsCaptureSession5::MinUpdateInterval`，60 Hz 下限只用于背景时效容差；跨屏异步事务直接把锁存的
`captureRefreshRate` 传给新会话，不会在权限等待后重新查询出另一份 DRR 状态。这些仅表示代码逻辑和
链接已闭合，真实 HDR、多显示器、混合 DPI/刷新率及跨适配器硬件矩阵仍保持 `Not Run`。

DisplayConfig 拓扑瞬时不完整、查询失败或找不到目标时，DXGI-only 色彩结果不再覆盖最后有效 HDR
合同；只有运行时明确返回 `ERROR_NOT_SUPPORTED` 才接受该旧系统兜底。颜色模式可以先更新，但同一
物理目标的 SDR reference white 查询失败时会保留最后有效值并继续三次有限重试，避免 HDR 映射瞬时
回落到 `1.0`。次屏在无边框权限等待期间收到新的 DPI/DRR 元数据时，会原位更新 pending target 和
执行中的 target intent，不取消权限事务，后续 WGC StartSensor 直接使用最新捕获刷新率。支持报告会记录
颜色拓扑状态/错误码、reference white 是否保留及已经消费的通知 generation；这些仍是生产逻辑与诊断
合同，不能替代真实 HDR 和混合显示器验收。

颜色管线现在明确分离两类白点：WGC 的物理 scRGB 背景先用背景 reference white 转入 Unity 相对工作空间，
Unity authored color、粒子、材质、Trail 与 Bloom 继续在线性 FP16 中求值；最终输出时才使用目标屏输出
reference white 进行 SDR/HDR 映射。HDR/WCG 要求背景白点而该值未知时，Host 保留 WGC producer 预热，
但失效旧背景快照并强制 FX-only，不能用最终输出白点或固定 `1.0` 代替背景输入合同。

主协调屏现在遵守相同合同：目标身份和资源域未变化时，权限等待期间的新 DPI、物理/虚拟刷新率及更完整
的 DisplayConfig 设备路径会同时合并到 pending target 与执行中的 target intent，不取消最长 `120 s` 的
无边框权限请求，也不替换该事务已经锁存的 HDR 输出策略和颜色快照。`StartSensor` 因而直接使用最新
`captureRefreshRate`，提交后不会先接受旧 DRR 快照再等待下一次拓扑轮询纠正。主副屏共用同一元数据变化
判定，避免后续状态机再次分叉。

支持报告现在保留主协调屏快速摘要，并输出稳定排序的 `Display.Session[n]` 逐屏快照。每个快照覆盖显示
身份、DPI/刷新率、请求与实际 Adapter、颜色能力、请求/解析/实际输出策略、WGC 状态和 renderer fault，
使副屏或跨适配器故障能够归属到具体会话。该诊断闭环不改变真实硬件矩阵的 `Not Run` 状态。

同一逐屏快照现已通过只读 `GetDisplayState` IPC 暴露给 Control Center。“显示与性能”顶层页可以保留
显示器选择并展示实际边界、DPI、显示/捕获刷新率、GPU、输出、HDR、WGC 与故障状态；全局配置入口提供
默认关闭的 HDR 请求，以及 `match-display`、`60`、`120`、`144` 四档帧率策略。解析失败或 Host 报告
未知能力时 UI 显示不可用/未知，不以配置请求推导支持状态。这是代码逻辑与诊断入口，真实 HDR、多显示器、
混合 DPI/刷新率及跨适配器硬件矩阵仍为 `Not Run`。

暂停保留帧的背景时效合同也已统一：主协调屏与副屏在 `background-aware` 下都要求当前 WGC 样本，
不能把此前的桌面快照长期固化在 DirectComposition 表面。最终输出重协商统一使用三次有限预算；耗尽后
只有实际保守 SDR 可以作为安全回退，非 SDR 或未知 transport 必须 fail-closed。副屏会隐藏并以独立
`OutputContractFaulted` 锁存等待有效输出恢复，无关的 Bloom/输入配置成功不能重新显示旧 HDR 表面；
协调屏会在记录 `Display.Output.RenegotiationExhausted` 和支持快照后隐藏并终止 Host。这些是失败处置逻辑，
不改变真实 HDR、多显示器、混合 DPI/刷新率硬件矩阵仍为 `Not Run` 的结论。

## 当前优先级覆盖（2026-08-15）

按当前迭代决定，暂跳过第三阶段的 WGC/ROI 成本优化实现：

- `P1` 的 ROI 规划继续保持观测和 full-screen fallback，未通过 FP16 等价验证前不得接入生产；
- 当前主线直接进入下一阶段的 WGC/背景感知可靠性，优先处理会话失效、FramePool 重建、旧快照清除、
  自排除/光标失败回退和资源配平；
- 正式发布前仍须补齐可复跑状态或硬件证据。已有的 WGC 生命周期子集仍需继续补齐打包身份下的
  权限允许/拒绝、无边框成功、外部录屏、设备移除/重置和多显示器单元格，未执行项保持 `Not Run`。

当前已落地的可靠性工作包括生产 WGC 资源账本日志、停止通知竞态修复，以及 WGC 失败后允许同轮
窗口 resize 进入清理/重启事务。stop 现在分别锁存 FrameArrived/item.Closed 退订、Session Close 和
FramePool Close 失败，并汇总到 `OverallSucceeded`；任一阶段失败仍完成 included/FX-only 回退，保留
`SensorStopFailed`，并在本进程永久阻止 WGC 重启，`retryToken` 不能绕过。它们不改变动画或 ROI 画面合同。

可见帧始终 drain WGC；暂停或没有可见内容时，仅在首次、新 epoch、时钟回退或距上次尝试达到
`50 ms` 时执行 sensor-only drain。该维护路径不创建批次背景快照、不执行 Bloom、不 Present，也不计入
呈现帧数。背景快照成功参与和有效快照失效分别记录 `BackgroundComposite.Participated` 与
`BackgroundSnapshot.Invalidated`，包含控制代次、帧号、WGC 与快照 epoch/generation 和失效原因。
RTX 4060/Windows 10 的模式切换、暂停保鲜与存活快照失效子集已通过，证据见
[`artifacts/spikes/spk-002/rtx4060-win10-19045-mode-switch-snapshot-2026-08-15`](../artifacts/spikes/spk-002/rtx4060-win10-19045-mode-switch-snapshot-2026-08-15/README.md)。

同一机器上的 portable 无边框拒绝与恢复子集也已在 capture commit `c3781f7` 通过：允许系统边框时 WGC
正常参与，关闭边框后以 `WGC.BorderlessAccess.Checked=not-packaged / 0x80073D54 / not-started` 在 stop、
WDA 变化和新 Session/FramePool 创建前得出结论，事务随后回退 FX-only 并恢复 `WDA_NONE`；重新允许边框后
新会话与背景参与恢复。证据见
[`artifacts/spikes/spk-002/rtx4060-win10-19045-portable-borderless-async-current-head-2026-08-15`](../artifacts/spikes/spk-002/rtx4060-win10-19045-portable-borderless-async-current-head-2026-08-15/README.md)。
这只关闭 portable `not-packaged` 单元格，不覆盖 packaged 权限拒绝或无边框成功，完整 SPK-002 仍为
`Not Run`。

当前 Host 已移除渲染所有者线程上的 `wait_for(100 ms)`：无边框授权现在是所有资源副作用之前的独立
跨帧动作，使用 `120 s` 有限截止时间。Pending 不停止旧 Sensor、不改变 WDA/profile、不创建 FramePool
或 Session；原控制代次、动作起点、累计动作数和恢复禁令跨帧保留。配置变化、resize、device recovery
和退出会显式取消旧请求并执行一次 FX-only 回滚。自动化覆盖 Pending 不推进、截止竞态、取消幂等和最长
动作序列；owner cancel 与 broker failure 已分离，前者允许相同捕获身份在新控制代次重新申请，后者仍是
稳定终态，避免权限 UI 被渲染循环重复触发。portable `not-packaged` 本地回退已复跑，但 Windows 11
packaged `Allowed/DeniedByUser/
DeniedBySystem` 仍需独立快照证据，不能据实现或本次 portable 证据标记为通过。
portable 的 `not-started` 立即返回也不覆盖 Windows 11 权限 UI 长时间 Pending、用户取消或截止竞态。

显示拓扑现按 monitor handle、设备名和物理边界识别目标，同尺寸换屏也会执行
`StopSensor -> ResizeOutput -> StartSensor`。`WM_DISPLAYCHANGE`/`WM_DPICHANGED` 只发布失效信号，渲染
所有者再固定一次目标并完成事务；负虚拟桌面原点不会被截断。若 `A -> B` 的无边框权限仍 Pending 时目标
又变为 C，owner cancel 会丢弃 B 的旧 resize，C 必须以新事务提交；shutdown cancel 同样不会移动窗口。
`Display.Topology.Observed/Applied` 和事务目标字段可区分观察、提交和实际应用。当前只有确定性测试与同屏
有界探针，真实多屏、混合 DPI/刷新率、热插拔和跨适配器仍保持 `Not Run`。
没有新几何替代时，配置代次、device recovery、WDA 或 Session 故障取消会保留已经消费的 resize；该
策略与新目标/新窗口尺寸的 discard 分支由不同状态机观察值表示，不能根据诊断字符串隐式推断。

设备丢失路径现已接入 Host：渲染提交、Bloom 配置资源、swap-chain resize 或 WGC FramePool
Recreate 遇到可识别的 DXGI device-lost HRESULT 时，整个 renderer 最多执行一次 D3D/DComp/WGC
资源重建；渲染提交会用同一 CPU 快照重试一次，第二次故障直接退出并保留原始事件。恢复后 WGC
仅在故障调用前锁存到 Sensor 实际活跃时通过递增 `retryToken` 重新走有限事务；已有 FX-only 终态不会被
自动重启。该锁存发生在 render/Present、Bloom 设置和 FramePool Recreate 之前，Session.Closed 回调不能
在异常展开期间把故障前事实改写为 FX-only。
恢复到 WARP 或适配器变化时，renderer 会拒绝 WGC Start。
`--device-recovery-probe` 已作为有界 CTest 验证资源域主动重建和中心像素有效，但它不模拟真实
设备移除，真实 device-reset、热插拔和跨适配器单元格仍保持 `Not Run`。

Host 现在优先使用可选的 `ID3D11Device4::RegisterDeviceRemovedEvent`：设备移除句柄排在 frame-latency
句柄之前，同时信号时先进入设备恢复；异常信号不会被手动复位，而是立即失败，避免对 manual-reset 句柄
形成忙循环。暂停态的等待也同时观察设备移除与 WGC 背景帧，设备移除优先并触发一次恢复帧，不再推迟到
用户恢复运行。启动和每次成功恢复后都会记录 `Graphics.DeviceRemovalNotification.Status` 的可用状态与注册
HRESULT，`Performance.Interval` 另记 `FramePacing.DeviceRemovedWakes`，非零时提升为 Warning。当前机器已验证
通知注册和主动恢复后的重新注册，但没有制造真实 device-lost。

接口不可用或注册失败时，Host 保留原有轮询兜底：frame-latency wait 的 Win32 错误在调用点锁存；等待失败
或连续 `250 ms` 未得到 FrameReady 时查询 D3D device-removed reason，只有可识别的 device-lost 才进入上述
一次性恢复边界。运行截止检查已移到所有 `TimedOut`/`MessagesPending` 的 `continue` 之前，
`desktop_frame_pacing_stall` 使用永久不信号句柄验证 `--quit-after-ms` 不会再等到 CTest 外层超时。WGC stop
会在 FrameArrived/Closed 退订、Session Close 和 FramePool Close 的每次调用前后分别写入
`BackgroundCapture.StopProgress`，包含阶段、状态和 owner/caller 线程；即使调用不返回，最后一条完整日志
也能指出阻塞阶段。调用返回后仍汇总各阶段与总耗时；渲染阶段先完成的真实 stop 会跨随后无 sensor 清理
动作保留到首次日志消费，并以 `DeferredReport=true` 标识。能够返回的异常继续清理、线程不一致和双 stop
交接由确定性单元测试覆盖。生产 stop 另有进程级 watchdog：它在首条 `Stop/begin` 前启动，默认 `10 s`
未完成即
以退出码 `124` 强制结束 Host。由于阻塞的 WinRT Close 无法取消，超时后不会继续执行 WDA 回滚或复用旧
WGC 资源；只有已成功写入的四个阶段级 `StageState=begin` 能定位具体系统调用，`Stop/begin` 只表示
watchdog 已启动。真实 device-lost 下 Close 的行为、阶段和是否会触发该边界仍需故障注入，保持 `Not Run`。

Sensor 构造期间若已取得部分 WinRT 资源，回滚 stop 的聚合结果现在会在对象尚未发布给 Renderer 时同步交给
调用方 mailbox；任一 Close/退订失败会沿用同一进程级重启禁令，成功回滚则不误锁后续显式恢复。活跃 Sensor
另外以最高 `1 Hz` 回读覆盖层 WDA：成功只汇总进性能窗，丢失或查询失败写结构化错误并在下一次 Present 前
完成 stop、`WDA_NONE` 和 FX-only 回退。该状态机与日志合同已有确定性测试，但尚未用外部程序在真实运行中
强制篡改 affinity，因此不能据此关闭 packaged、外部录屏或跨版本无边框硬件单元格。

## P0：输入、渲染与 Present 延迟诊断

先建立可复现的性能基线，再改变 WGC 或渲染路径。诊断必须低开销、可聚合，并同时覆盖：

- Raw Input 队列年龄、待处理消息量、Move 收敛量和按钮边沿到消费时刻；
- WGC producer/accepted FPS、被合并或拒绝的样本数、样本年龄和 `drainLatest` CPU 耗时；
- 背景 FP16 copy 与 Bloom 的 GPU 耗时，使用异步 D3D11 timestamp query，不把 CPU 提交耗时
  冒充 GPU 执行耗时；
- `Present(0, 0)` 的调用阻塞时间，以及输入边沿到对应 `Present` 返回的相关耗时；
- 渲染模式、adapter、driver、显示模式、降级原因、WGC 生命周期和资源账本等排障上下文。

日志默认输出固定时间窗的计数、FPS、`p50/p95/p99/max` 与有速率限制的慢帧样本，不逐帧刷盘。
启动、模式切换、设备重建、fallback 和失败仍作为独立事件记录。诊断本身需要记录采样开销和丢失计数，
避免测量行为掩盖原问题。

`input-to-Present-return` 只表示应用收到输入后完成本次 Present 调用的时间，不是真实上屏延迟。
物理显示还包含 DWM、显示队列、扫描输出和面板响应；在取得 ETW/PresentMon 类证据或外部高速测量前，
报告必须使用 `input-to-Present-return` 这一准确名称，不能写成“输入到显示”或“上屏延迟”。

本阶段交付为一份可复跑的基线报告，至少分别覆盖 FX-only 与 `background-aware`，并能回答延迟来自
输入积压、WGC、copy、Bloom 还是 Present。没有基线数据时，不凭主观卡顿直接选择优化点。

当前配对采集入口先给 WGC `50 ms` acquire 预热，再固定一个 `130 ms` 点击时间片，并在同一完整
10 秒性能窗内每 `25 ms` 投递一批 5 条无害线程消息：

```powershell
pwsh -NoProfile -File tools/collect-performance-baseline.ps1 `
  -Executable build/alpha-x64/src/desktop/Release/ba-click-fx-desktop.exe `
  -OutputDirectory artifacts/local/performance-baseline-<timestamp>
python -B tools/report-performance-baseline.py `
  artifacts/local/performance-baseline-<timestamp>
```

采集器拒绝覆盖目录、已有 Host、脏工作树和无界进程等待；该诊断场景关闭 Raw Input 注册以隔离
操作者活动。报告器要求 Raw Input 为零，并校验同一 HEAD/EXE、配置差异、WGC 参与、GPU 样本
覆盖率、丢样计数、资源账本与帧节流上限。

### P0 当前状态

配对渲染基线已在提交 `c87c83a` 完成并通过门禁，追踪证据位于
[`artifacts/performance/p0/rtx4060-win10-19045-4k-170hz-2026-08-15`](../artifacts/performance/p0/rtx4060-win10-19045-4k-170hz-2026-08-15/README.md)。
FX-only 与 background-aware 在 `3840x2160 @ 170 Hz` 下均保持
`GPU.PendingFrames.Max=1`；background-aware 的 GPU command span p95 增加 `975 us`，
所列单阶段中 Bloom/final 增量最大，为 `491 us`。Present p95 没有形成稳定区间瓶颈，
但最大值仍保留 `+7252 us` 的尾部风险记录。

该场景为了获得可比较的渲染成本而关闭 Raw Input，因此 P0 尚未全部完成：仍需加入受控 Down
边沿场景，记录 Win32 消息年龄、dispatch-to-Present-return 与 message-to-Present-return。
这项缺口不阻止使用已经闭环的渲染配对数据启动 P1，但任何报告都不得把当前结果称为完整输入延迟基线。

## P1：WGC 成本优化与 guard-band ROI

当前 P1 原型已完成 Unity Bloom footprint 的纯函数规划，但尚未改变生产画面：
`planUnityBloomRoi` 会输出保守 guard/phase，生产渲染仍保持 full-screen fallback。
当前 Host 已从活跃 `FrameSnapshot` 生成跨帧 dirty rect，并在 `Performance.Interval` 记录
full-screen/计划工作区像素、guard、phase 和各状态计数；这仍是观测数据，生产渲染继续保持
full-screen fallback。下一小项是以该计划测量 ROI 与 full-screen 的资源尺寸、GPU 时间和
FP16 差异。

基线确认瓶颈后，按下列顺序降低背景感知路径成本：

1. 没有可见特效时保留会话，并把 sensor drain/copy 限制为最高 `20 Hz`；跳过批次背景快照、Bloom 和
   Present。重新进入活跃状态时使用保鲜样本，不能复用已失效的桌面快照。
2. 仅在特效存活期间处理背景，并验证暂停、模式切换和 WGC frame event 不会留下静态旧背景。
3. 以活跃特效包围盒为基础加入 Bloom 半径、滤波核、移动余量和 mip 对齐所需的 guard band，建立
   局部 ROI 原型；边缘、尺寸或资源状态不满足合同时必须回退全屏路径。
4. 使用相同场景比较全屏与 ROI 的 GPU 时间、copy 带宽、Present 阻塞和 FP16 像素结果；不能只报告
   CPU 提交时间或资源尺寸。

本阶段交付为 active-FX-only WGC 处理和 guard-band ROI 原型，以及相对全屏基线的成本与像素差异报告。
ROI 数值合同继续服从 ADR-006；原型通过前，全屏仍是生产 fallback。

## P2：Unity/Web/Native 固定时间片视觉回归

使用相同 viewport、配置、输入轨迹和背景，对 Unity、Web 与 Native 执行一次面向可见差异的回归：

- 点击固定 `50/100/120/250/450 ms` 时间片；
- 拖拽固定采样点、按下/保持/释放边界和释放后寿命；
- 比较总动画时长、粒子包络与径向能量、拖尾几何/材质/生命周期、圆环以及三角碎片 Bloom；
- Native 继续核对 FP16 中间层；Web 与 Unity 差异先单独记录，再以 Unity/游戏证据作为视觉真值；
- 每个可见差异必须有截图或数值证据、原因归类和处理结论，不能用新增测试数量代替结果。

本阶段交付为三端固定时间片差异报告和针对已确认差异的修正。随机流尚未等价的部分继续使用统计与
包络合同，不把不同随机布局误报成动画时序回归。

## P3：正式发布硬件验收

输入跟手、WGC 成本和视觉回归收敛后，再集中完成以下发布矩阵：

- WGC 打包身份权限允许/拒绝、无边框成功、外部录屏、模式切换、压力和功耗；
- HDR/Advanced Color 输出与 SDR fallback；
- 多显示器、混合刷新率、DPI、多适配器、热插拔和 device lost。

这些单元格仍是相关 ADR 接受和正式支持声明的硬门槛。未执行项保持 `Not Run`，相关 ADR 保持
`Proposed`；优先级后移不等于放宽证据，也不允许用模拟结果代替真实硬件结果。除非发现会阻塞 P0-P2
或造成数据/资源安全问题的缺陷，当前迭代不继续扩张独立硬件 collector/verifier。

## 执行规则

- 每项小工作单独使用中文提交，并在提交前执行与改动范围相称的有界验证。
- 构建、测试、实机采集和外部工具都必须设置硬超时；超时后保存阶段与诊断，不做无界等待。
- 同一失败最多进行一次有新证据支撑的复跑；结果不变时先收敛根因或调整方案，避免盲目循环。
- 性能优化必须同时给出优化前后同场景数据；视觉修改必须同时给出固定时间片回归结果。
- 路线图进度按用户可感知结果、可复跑报告和已解释差异统计，不按新增日志行、collector 或 verifier 数量统计。
