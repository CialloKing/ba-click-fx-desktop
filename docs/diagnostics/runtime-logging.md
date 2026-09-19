# 运行时日志排查

本文描述 `v0.2.19` 起的日志；`v0.2.18` 不包含下面新增的输入和窗口观察事件。
现有日志路径、schema 2、8 MiB 轮转、三份备份和 64 KiB 单条预算保持不变，
入口见 [SUPPORT.md](../../SUPPORT.md)。

## 采集与关联

在控制中心“系统”页打开日志目录，保留 Host 和控制中心的当前日志、仍存在的轮转备份，
以及 `BAFX.config.json`。记录正常、异常和恢复操作的时间，尽快取走日志以免故障窗口被轮转覆盖。
这些诊断默认启用，无需修改配置。

先按 `Log.SessionId` 区分进程运行，再用 `Event.Sequence`、`Event.Utc` 和
`Event.MonotonicUs` 排列同一运行内的事件。输入与窗口观察另有 `Observation.TickMs`
（Windows 启动后的毫秒数）；它与进程相对时间 `Event.MonotonicUs` 的起点不同。
跨进程关联用 UTC 和操作时间，不比较各自的事件序号。

每个渲染会话有进程内唯一的 `Surface.InstanceId`，同时记录 `Window.Handle` 和矩形。
显示会话重建后即使 HWND 被复用，也会重新建立计数基线。
`Observation.CounterBaselineReset=true` 时，模拟增量从该实例零值计算，
`Observation.IntervalMs=0` 表示没有前次报告时间窗，不代表无限速率。
其余增量按同一事件、同一实例的 `Observation.IntervalMs` 解释；不同事件的窗口不保证对齐。
`Configuration.Generation` 可对照 `Configuration.Applied`，控制命令前后值见
`Control.Mutation.Completed`。

## 事件与含义

| 事件 | 主要证据 | 频率 |
| --- | --- | --- |
| `Input.Health` | `WM_INPUT` 到达、读取成功、Move/Down/Up/Cancel、读取/坐标/QPC 失败、最近成功和失败时间；设备移除、几何重置、策略取消 | 首次写入；有输入或状态变化时最多每秒一条，无变化时 10 秒心跳，正常退出刷新 |
| `Input.Routing` | 路由帧结果、按键和目标状态、无目标、坐标回退/映射失败、丢弃和重置调用 | 首次写入；输入活动时最多每秒一条，无变化时 10 秒心跳，正常退出刷新 |
| `Desktop.ForegroundChanged` | 前台 HWND、PID、窗口类名及查询错误 | 最多每 250 ms 采样，观察到变化才写；不是全部 Win32 激活事件 |
| `Desktop.SurfaceState` | 覆盖窗口状态、上方相交候选、配置/暂停/电源/故障、调度原因、模拟处理结果、Present 累计 | 最多每 250 ms 采样；状态变化写入，稳定输入活动最多每秒一条，否则 10 秒心跳；正常退出刷新 |
| `Desktop.SurfaceRemoved` | 上次观察到的显示会话已从会话列表移除 | 下一次采样时写入 |
| `Performance.Interval` | 原有 10 秒性能汇总、输入队列/等待、WGC、CPU/GPU、进程资源和最慢帧 | 10 秒及正常退出的末次窗口 |

以上周期由 Host 主循环提供服务，不是独立线程的准时保证。主循环卡住或进程异常终止时，
心跳和末次刷新也可能缺失。新观察只读取窗口状态，不改变焦点或层级，不采集窗口标题或截图。

### 输入与模拟

- `Input.Received.*` 在 `WM_INPUT` handler 入口计数；`Input.Accepted.*` 在鼠标包、
  `GetCursorPos` 和 QPC 都成功后计数。原 `Performance.Interval` 的 `Input.RawMessages`
  仍表示后一种成功读取，旧分析脚本的含义不变。
- `Input.Registered` 仅表示本 Host 的注册成功记录，不是实时重新查询的系统注册状态。
  `Input.LastFailure.Stage` 区分 `raw-data`、`packet-header`、`mouse-payload`、
  `cursor-position` 和 `clock`；Win32 失败保留错误码，QPC 失败不伪造 Win32 错误。
- `Input.GeometryDiscardedEvents.Delta` 记录几何变化前清掉的排队事件。
  设备移除、几何重置和按键策略取消计数用于解释 Cancel，不能一概解释为硬件掉线。
- `Routing.*.Frames` 是报告窗口内有输入事件的路由帧最终结果计数。`EdgeFrame` 表示按键边沿分支，
  `FreeForwarded`/`HeldForwarded` 表示移动或跨屏笔划延续已交给模拟；`AmbientDisabled`、
  `HeldWithoutStroke`、`NoTargetOrOutside`、`MappingFailed`、`Discarded` 表示对应分支。
  `Routing.ResetCalls.Total` 是重置方法的调用次数，包含重复取消，不是实际消失的拖尾数量。
- `Simulation.MoveCalls.*` 才统计模拟收到的 Move。其处理结果分为 `HeldMoves`、
  `AmbientDisabled`、`RateLimited`、`AmbientAnchors` 和 `AmbientMoves`，以 `.Delta` 输出。
  起点建立或采样限频可能不产生新拖尾段；`AmbientEnds` 统计常驻实例结束或被硬清除。
  `Surface.AmbientActive` 仅表示存在常驻实例或起点，不能据此断言有可见几何。

原始包、路由帧和模拟调用不是一一对应：输入会合并，跨屏笔划延续与内部演示也有独立入口。
应结合各阶段语义判断，不能把计数差直接当成丢包。

### 窗口、调度与呈现

`Window.Valid/Visible/Minimized/Topmost`、矩形和 `Window.Cloaked` 描述查询时的窗口状态。
查询失败保留错误码或 HRESULT，并省略相应状态数值。`Window.AboveCandidate` 是最多扫描
128 个上方窗口后找到的首个可见且矩形相交的候选；它可能透明或被 DWM 隐藏，不能证明遮挡。
`Window.AboveScan.Truncated/QueryFailures/LastWin32Error` 说明扫描是否完整及是否遇到错误。

`Schedule.Reason` 记录窗口采样前最近一次全局空闲策略决定：
`display-power-unavailable`、`pause-cleanup`、`paused`、`continuous-rendering`、
`render-invalidated`、`pointer-input-pending`、`active-effects`、`clear-last-frame`、`idle-no-content`。
它不是每显示器的帧等待结果；应检查 `Schedule.Available` 和配置代次，并结合
`FramePacing.*`、图形恢复事件判断后续是否真正提交。

`Surface.DrawableLastFrame` 是最后一次成功 Present 对应帧的内容标志。
`Surface.PresentedFrames.Total` 统计该会话成功返回的 Present；
`Surface.LastPresentedFrameStart.AgeMs` 从最后一次成功呈现帧的开始时间计算，
不是精确 Present 完成时刻。它们均不证明 DWM 合成、扫描输出或肉眼可见结果。

## Issue #2 的复现判读

参照 [原始排查证据](issue-2-desktop-trail.md)，依次记录普通窗口激活、点击/框选桌面后移动、
再次激活普通窗口后三个阶段。每阶段约 20 秒可同时覆盖新事件和原有性能窗口。

1. `Input.Received` 停止：检查消息泵和心跳是否也停止；仅凭计数不能区分用户没有移动、
   消息没有投递和主循环没有处理。Received 增长而 Accepted 不增长时，先查具体读取失败。
2. Accepted 增长：检查路由是否转交、是否无目标或发生取消，再查模拟是否只是建立起点、
   被限频或关闭了常驻模式。
3. 模拟继续处理而 Present 不增长：结合暂停、电源、调度、帧等待和图形故障事件定位。
4. 模拟和 Present 都继续：比较前台、覆盖窗口状态及上方候选。状态都正常仍不可见时，
   需要故障机的同步录屏或进一步图形取证；日志不能独立确认 DWM 最终可见结果。

日志中的 `*.ReportFailed`、`Performance.IntervalFormattingFailed`、`Log.RecordTruncated`
以及 `Log.Health.*` 表示诊断本身可能不完整，应先检查它们再解释缺失数据。
