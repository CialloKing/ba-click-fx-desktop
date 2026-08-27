# 0.2.7 支持与验证范围

## 可以测试的范围

- Windows 10/11 x64，单个主显示器。
- 三种渲染模式下的点击与拖拽特效，以及基于 Unity/游戏资源的当前 D3D11/Bloom 渲染路径：
  `background-aware`（背景感知）、`recording-compatible`（录屏兼容拟合）和
  `light-background`（浅色背景优化）。背景感知启用 WGC，失败时回退内部 FX-only transport；
  其余两项关闭 WGC。
- 基础页还提供“核心性能模式（低配测试）”。该模式保留中心圆盘、圆环、点击/拖拽碎片和拖尾，
  仅跳过 Bloom 与 WGC/背景捕获，固定 60 FPS、保守 SDR 和 FX-only；它只用于低性能机器反馈，
  不证明完整背景合成、HDR 或 WGC 能力。
- `BAFX.ControlCenter.exe` 的原生 Win32 控制面：基础页管理启用状态、点击特效、鼠标拖尾、拖尾常驻、
  左/右/中键触发策略、效果大小、
  拖尾长度、拖尾宽度、输入采样率上限、Bloom 强度和 Bloom 质量；高级页按时间、粒子与材质、圆环、点击碎片、
  Bloom 和分层开关分为六个二级页面，并通过原生 `effects.*` 路径提供透明度、点击/拖尾时间倍率、拖尾寿命、
  `effects.diskRadius`、`effects.diskLifetimeMs`、`effects.ringsCount`、`effects.ringsLifetimeMs`、
  `effects.ringsRadiusMin`、`effects.ringsRadiusMax`、`effects.ringsAngularVelocityMultiplier`、
  `effects.ringsRotationDirection`、`effects.ringsHdrIntensity`、`effects.shardsHdrIntensity`、
  `effects.shardsClickCount`、点击寿命上下限、出生半径、速度上下限、`effects.shardsSizeMin`、
  `effects.shardsSizeMax`、`effects.trailOpacity`、Bloom 扩散/阈值/软阈值/亮度上限等参数。分层页可独立
  隐藏中心圆盘、圆环、点击碎片、拖尾碎片、拖尾线和 Bloom。基础页还提供背景模式、指针排除、系统捕获
  边框和空闲资源优化，并管理四个内置及用户保存的 effects-only 特效 Profile。“显示与性能”页选择并
  显示 Host 的逐屏实际状态，提供默认关闭的全局 HDR 请求、默认关闭的自适应 Active-FX ROI 实验开关，
  并在选中显示器下展示 primary/recording-rebuild 的近 5 秒路径、原因、分阶段像素、矩形和 GPU 工程面板，以及跟随
  显示器、固定 `60/120/144 FPS`、无限制五种帧率策略；具有稳定标识的显示器还可
  独立控制特效、HDR 请求和帧率策略。“系统”页提供随 Windows 启动、启动时最小化和关闭时隐藏到托盘，
  以及“清理诊断日志”按钮；确认后会显示删除文件数、释放字节数和失败文件数。“版本与更新”区域显示
  Control Center、Host、安装状态和最新公开版本，并提供手动检查与固定官方 Release 页面入口。启用随
  Windows 启动后，登录时由 Control Center 复用正常激活路径启动 Host。
  所有改动会通过本地 Named Pipe 在下一帧应用到正在运行的 Host；
  “重置默认”经确认后恢复全部持久化设置，但保留当前暂停或运行状态。
- 每次输入消费/呈现更新只为按压 FX 使用一份帧边界当前位置，并以同一 `renderTime` 按
  Down→Held→Up 处理普通路径；普通 Up-only 释放帧的 Held 为 false，不应用该帧的按住移动。输入采样率 `0` 不额外
  限频；`1..1000 Hz` 只按消息分派 QPC 推进可选位置采样相位，不影响边沿或模拟时间。
- Raw Input 的同帧多边沿仍按原序无损保留，仅用于诊断和 native 扩展；严格效果路径将其归约为
  Down/Held/Up 布尔帧态并按 Down→Held→Up 执行，Cancel 最后作为 native 硬边界处理。Unity
  `2021.3.45f1` Player 已确认 `Down-Up-Down` 的聚合帧三态同时为 true；其他边沿排列及游戏所用
  Unity `2021.3.56f2` 仍未验证。含任一边沿的帧不会从尾随 Move 重启常驻拖尾。
- 拖尾常驻是桌面版原生增强，不生成点击 burst。Unity TrailRenderer 的空间参数仍保持
  `m_MinVertexDistance=0.01`。
- D3D11 硬件设备；硬件设备创建失败时尝试 WARP 软件设备。
- 当前验证范围为普通 SDR 桌面合成路径。
- Spout2 发送器名称固定为 `ba-click-fx-desktop`，输出合同为
  `BGRA8 + SDR byte-domain rolloff + extended premultiplied alpha + FX-only v5`。空闲帧严格透明；圆盘保留
  Cross2 coverage Alpha，纯加法圆环、碎片、拖尾和 Bloom 只使用一个 BGRA8 Alpha 步进
  防止接收端清除，仍允许 `RGB > Alpha`。RGB 使用共享峰值 rolloff，避免 OBS 在编码游戏画面上
  直接叠加时再次抬亮 Bloom 暗部并使中心过早削顶；输出不混入或依赖 WGC 桌面背景；
  WGC 不可用或失败时仍能输出点击和拖尾。
  OBS 单独捕获游戏/桌面并置底，`Spout2 Capture` 源置顶，Composite Mode 必须选择
  `Premultiplied Alpha`，来源混合方式保持 `Default`、混合模式保持 `Normal`，再执行
  `Transform -> Fit to Screen`。
  旧的 `Default`/`Opaque` 或来源级 `Add` 设置需要显式迁移。`Add` 虽会掩盖旧 v3 对纯加法层
  的背景衰减，但也会破坏圆盘的 source-over coverage。Control Center 只读检查发送状态、
  `win-spout.dll` 版本/位数和 OBS
  实际加载状态，不会下载插件或修改场景。完整设置、迁移脚本和验收边界见
  [`docs/OBS_SPOUT2.md`](docs/OBS_SPOUT2.md)。旧的 `--spout2` 参数仍可用于诊断时临时开启，
  但不改变持久化开关。
- 支持报告保留主协调屏摘要，并按稳定顺序为每个显示会话记录角色、边界、DPI、显示/捕获刷新率、
  DisplayConfig 身份、请求/实际 GPU、HDR/Advanced Color、最终输出策略、WGC 状态和渲染故障；
  这些只是当前运行快照，不能据此宣称 HDR、多显示器、Advanced Color 或物理 nits 输出已经受支持。
  驱动未提供有效亮度时会记录 `luminance-unknown`。
- `GetDisplayState` schema 4 通过本地 IPC 返回全局拓扑、配置/应用代次、权威离线 override、逐屏来源身份、
  物理/捕获刷新率、DRR、GPU、颜色查询 HRESULT、SDR white level、已应用特效/HDR/帧率策略、
  请求/解析/实际输出、cadence/output fallback、WGC、故障状态和 Active-FX ROI 工程快照。每个 session
  固定提供开关状态、样本窗/年龄/末帧，以及 primary 与 recording-rebuild 的实际路径、决策原因、
  帧/像素累计值、guard/phase、dirty/aligned rect、prefilter/downsample/upsample/resolve 的
  full/candidate/drawn/cleared 像素、阶段 GPU p50/p95 和原因计数。Control Center 严格拒绝旧 schema 以及
  未知、重复、缺失或超限字段；未知布尔能力使用 `null`，不会把全局 HDR 请求或当前配置冒充为实际支持状态。
  schema 4 不提供 schema 3 兼容层。完整拓扑下可删除未连接显示器的遗留 override；该条目不会显示
  伪造的 HDR、刷新率或 ROI 运行状态。
- `GetState.productVersion` 使用规范 `MAJOR.MINOR.PATCH` 标识 Host 版本。只有 Host 与 Control Center
  完全同版本时设置控件才可写；字段缺失、格式错误或版本不一致时 fail-closed，设置保持禁用，但 Host
  启动和关闭入口继续可用。该产品版本门不改变配置 schema，0.2.7 仍使用 schema 19。
- WGC FP16 scRGB 背景使用独立的背景 reference white 转入 Unity 相对工作空间；Unity authored color、粒子、
  材质、Trail 和 Bloom 仍在线性 FP16 中计算，最终呈现阶段才使用输出 reference white 选择 SDR/HDR 映射。
  HDR/WCG 下背景白点未知时 WGC 可保持预热，但该背景不得进入合成，当前画面回退 FX-only。
- 主副屏的最终输出重协商都最多尝试三次，后续尝试按一秒显示维护节拍执行。预算耗尽时，只有实际
  transport 已满足保守 SDR 才接受安全回退；仍为 scRGB、未知或其他不满足 SDR 合同的输出会 fail-closed。
  副屏立即隐藏并锁存 `Display.Session[n].OutputContractFaulted=true`，普通 Bloom 或输入配置成功不会解除；
  只有实际输出重新满足当前策略、完整拓扑重建或资源恢复才能重新显示。协调屏会先隐藏，再终止 Host，
  防止旧 HDR 表面继续驻留。`Display.Output.RenegotiationExhausted` 会记录请求/实际映射和最终处置。
- 首次生成的完整 schema 19 配置默认为 `background.mode=background-aware`、
  `background.allowSystemBorder=true`、`input.trailOnlyWhilePressed=true`、
  `input.samplingRateHz=0`、`display.hdrEnabled=false`、`performance.framePacing=match-display` 和
  `performance.activeFxRoiEnabled=false`。
  `match-display` 使用目标显示器的精确刷新率，刷新率缺失或无效时回退到 60 FPS；`unlimited` 才保留
  不设置额外最小帧周期的行为。字段完整的 schema 14 至 18 只按固定迁移链升级到 schema 19；其他版本、
  缺失或未知字段以及枚举别名均被拒绝。Host 保留无效原文件并以内存中的当前默认值继续运行，不猜测或
  部分套用无效配置。背景感知授权、排除或会话失败时回退内部 FX-only transport；其余模式不启用 WGC。
- Active-FX ROI 当前可在纯特效 primary 及实际执行的录制/Spout2 纯特效重建中裁剪 prefilter 和完整
  down/up 金字塔。规划器为每个 pass 生成独立矩形，并为最终 resolve 生成逻辑有效区；resolve 与最终
  场景合成仍是一次全屏 draw，shader 在有效区外采样精确零 Bloom。每个实际 down/up 目标维护初始化、
  上一写入矩形、全屏写入状态和最后 writer；首次进入、全屏转 ROI、resize 或资源恢复时
  完整清理，稳态矩形移动或 writer 改变时用 Context1 `ClearView` 清理旧区；同一 writer 连续覆盖
  相同矩形时跳过冗余清理。
- 一帧内只允许完整 ROI 或完整全屏 Bloom。pass 计划、Context1、资源身份、相位或状态任一不满足约束时，
  所有 Bloom pass 同帧回退全屏；不会把局部 prefilter 与全屏后续 pass 混用。primary 与
  recording-rebuild 分别统计，但共享物理资源只维护一份真实写入状态。
- 默认 `background-aware` primary Differential Bloom、最终场景合成、WGC、Spout2 格式转换、交换链和
  Present 仍保持全屏。工程面板的像素处理比例不是 GPU 节省百分比；该开关存在也不代表端到端性能或
  硬件矩阵已经通过验收。0.2.7 的 RTX 4060、4K 170 Hz、SDR 正式 ABBA 已执行但整机门槛失败，
  因此该版本未发布，不能据局部 GPU 收益声明端到端性能通过。
- Host 为每个显示会话维护 5 秒滚动窗，每 500 ms 发布不可变快照；Control Center 只在“显示与性能”
  页可见时每秒轮询，离页停止，样本年龄超过 3 秒标记 stale。IPC 失败只让诊断保持旧值/显示错误，
  不会修改配置或改变 Host 渲染路径。
- portable 运行时把 `BAFX.config.json`、`fx-profiles`、`ba-click-fx-desktop-support.log` 和支持报告写入
  EXE 所在目录；Identity 安装版写入该目录下的 `data` 子目录。命令行支持报告即使传入绝对路径，也只采用文件名，
  不会写入 `%LOCALAPPDATA%`、当前工作目录或其他用户目录。
- 支持日志 schema 2 为每条记录写入会话 ID、单调时间、序号、进程/线程、级别和事件名；当前文件达到
  8 MiB 后轮转，最多保留 `.log.1`、`.log.2`、`.log.3` 三份备份，总预算约 32 MiB。控制中心的
  `ClearLogs` 清理会删除当前文件及遗留备份，再写入一条清理结果事件；正常运行每 10 秒写一条
  `Performance.Interval`，退出时刷新最后一个未满窗口；它包含输入队列年龄、消息/Move 收敛、WGC
  callback/accepted、背景样本年龄、CPU 提交阶段、Present 调用、输入到 Present 返回，以及 WGC/copy、
  背景快照、FX 材质和 Bloom/最终复合的异步 D3D11 GPU 时间戳 `p50/p95/p99/max`。0.2.7 还分别记录
  primary 与 recording-rebuild 的 Prefilter、Pyramid、FinalComposite p50/p95，并保留原 Bloom 总耗时。
  GPU 分析器使用
  固定 8 槽查询环，每个渲染帧最多无阻塞轮询一次，不调用 `Flush` 或等待查询完成；日志会另外记录
  pending、环满跳过、disjoint、查询失败和取消回收数量。未取得 GPU 样本时相应指标保持
  `Available=false`，不会用 `0` 伪装结果；WGC、背景快照和 FX 阶段只统计原帧实际适用的样本。
  `WGC.DrainPolicy=visible-every-frame-idle-sensor-only-max-20hz` 表示可见帧每帧尝试 drain，暂停或空闲时
  最多每 `50 ms` 做一次 sensor-only 保鲜；`WGC.MaintenanceCycles` 统计这种不创建批次快照、不执行
  Bloom、不 Present 的维护轮询，它不增加 `Window.FrameCount`。
  进入暂停时，主协调屏和每个副屏都只有在当前 WGC 背景可用时才允许提交保留帧；没有当前样本时不会
  把旧桌面快照固化到可长期驻留的 DirectComposition 表面。FX-only 模式不受该背景时效门槛影响。
  CPU/API 时间不代表 GPU 执行，GPU 时间戳也不包含 Present、DWM 合成、扫描输出或物理上屏；异步完成的
  样本还可能属于较早的报告窗口，日志中会保留对应 semantic 字段。
  帧等待另外记录 `FramePacing.DeviceRemovedWakes`；非零表示 D3D 设备移除通知直接唤醒过 Host，并会把该
  性能窗提升为 Warning。它只说明通知路径被触发，不等同于恢复已经成功。
  排障时请同时提供 `BAFX.config.json`、当前 `.log` 和仍存在的三个轮转备份。无需制作一键诊断包；
  若用户主动清理过日志，请保留清理后的新日志并说明清理时间。
- 每个 `BackgroundCapture.Transaction.End` 后会追加累计的
  `WGC.ResourceLedger.*` 记录，包含 Frame/FramePool/Session、两类事件注册的
  created/closed/live 计数、recreate 次数和 `Failures`/`AllReleased`；它覆盖会话停止、
  ContentSize 重建、item.Closed 和失败回退，即使 sensor 对象已经销毁也保留本次进程的账本。
  Action 抛异常、状态机拒绝或超出固定 action budget 时会分别记录
  `Phase=action-failed|transition-rejected|budget-exceeded`；正常退出记录 `Phase=shutdown`。
  账本格式化失败只写固定的 `Reason=formatter-failed` 降级事件，不会改变渲染事务结果。
- WGC Sensor 活跃时，Host 最多每秒只读回查一次覆盖层是否仍为 `WDA_EXCLUDEFROMCAPTURE`。成功次数汇总到
  十秒 `Performance.Interval` 的 `WGC.CaptureExclusion.HealthChecks/HealthFailures`，不逐次写盘；查询失败或
  观察值变化时立即写 `WGC.CaptureExclusion.HealthFailed`，保留控制代次、待处理事务、期望值、观察值和
  Win32 错误，然后按 `StopSensor -> WDA_NONE -> FX-only` 回退。若无边框权限请求仍在等待，会先取消该请求，
  再执行同一回退；稳定配置不会在渲染循环中自动重启 WGC。该防护不等同于无边框 WGC 已通过跨版本验收。
- 每次 WGC stop 还会记录 `WGC.Stop.SensorPresent/Completed/DeferredReport`、FrameArrived/Closed 两类事件
  退订耗时、`SessionCloseUs`、`FramePoolCloseUs` 和 `TotalUs`。渲染阶段已经停止 sensor、随后清理事务再次
  执行无 sensor stop 时，真实耗时会保留到首次日志消费并标记 `DeferredReport=true`，不会被零值覆盖；消费
  后不会污染下一次 stop。四个同步调用前后还会各写一条 `BackgroundCapture.StopProgress`，其中
  `WGC.Stop.Stage/StageState` 指出当前阶段与 `begin|succeeded|failed`，owner/caller 线程字段用于发现跨线程
  误用。stop watchdog 会在 `Stop/begin` 日志前启动，整个 stop 共用默认 `10 s` 的单一硬截止，后续阶段只
  继承剩余时间；达到截止时间时 Host 会以退出码 `124` 强制结束。watchdog 不能取消已经阻塞的 WinRT
  调用，因此超时路径不会继续执行 `WDA_NONE`、FX-only 回退、事务结束或资源账本汇总，避免在旧 Session
  状态未知时重新捕获覆盖层。需完全重启 Host，并连同当前日志和轮转日志一起排查；只有已成功写入的
  四个阶段级 `begin` 能定位具体调用，`Stop/begin` 只表示 watchdog 已启动。
  四个 `*Failed` 字段分别对应 FrameArrived/item.Closed 退订、Session Close 和 FramePool Close；
  对于能够返回的 stop，`OwnerThreadMismatch=true` 或任一阶段失败时 `OverallSucceeded=false`，仍执行
  included/FX-only 回退，并把 `SensorStopFailed` 保留到控制事务。
  为避免旧 WinRT 资源被重新使用，本进程之后永久阻止 WGC 重启，必须完全重启 Host 才能再次尝试。
- Host 会尝试通过 D3D11.4 注册 device-removed event。启动及每次成功资源恢复后，
  `Graphics.DeviceRemovalNotification.Status` 记录 `Phase`、`Available` 和 `RegistrationHRESULT`；接口
  不可用或注册失败时，活跃渲染等待仍以每 `250 ms` 一次的 device-removed reason 查询兜底，不会把注册
  失败当作渲染失败。通知可用时，Host 暂停期间也等待该事件；信号会请求一个恢复帧，但不会推进冻结的
  特效模拟时间。渲染/Present、Bloom 和 FramePool 路径都会在可能失败的调用前锁存 WGC 状态；设备恢复
  只会为故障前实际活跃的 WGC Sensor 安排一次重试。WDA 丢失、权限拒绝或
  Session 停止形成的 FX-only 终态不会因无关的设备恢复而获得新 `retryToken`。
- WGC 只由 `background-aware` 模式使用。portable EXE 没有 package identity，也不会自行声明
  `graphicsCaptureWithoutBorder` capability。新配置默认允许 Windows 显示捕获边框；可见边框状态记录为
  `system-border=visible-allowed`。用户可在 Control Center 中取消勾选“允许黄色捕获边框”；关闭后会在
  任何 stop、WDA/profile 变更或 `StartCapture` 前确认无边框会话。权限请求按帧非阻塞轮询，默认用户
  提示截止时间为 `120 s`；等待期间 Host 继续处理输入、渲染、IPC 和退出。接口缺失、权限不足、超时
  或系统仍要求边框时直接报告
  `Support.WGC=fallback-fx-only`，并把当前渲染批次回退到内部 FX-only transport，不会先启动带黄色
  边框的会话。每次权限结论由 `WGC.BorderlessAccess.Checked` 结构化记录控制代次、事务动作序号、
  `AllowSystemBorder`、状态、HRESULT、`AsyncStatus`、`ElapsedMs`、`CancelRequested` 和 Allowed；配置、
  resize、device recovery 或退出覆盖等待请求时，`BackgroundCapture.Transaction.Cancel` 记录旧控制代次、
  动作序号和原因，终态与回滚各只记录一次；被新意图覆盖后，相同捕获配置可在新代次重新申请。broker
  拒绝、错误或超时仍是稳定终态，不会由渲染循环自动重复弹出权限 UI。portable 身份的预期拒绝状态是
  `not-packaged`，不能与
  packaged 用户/系统拒绝混为一谈。切换到 `recording-compatible` 或 `light-background` 会关闭 WGC。日志中的
  `BackgroundComposite.Participated` 才是背景样本进入最终 pass 的结构化判据；它记录已应用控制代次、
  成功 Present 的帧号以及 WGC/快照 epoch/generation。旧文本
  `WGC background sample entered the final desktop composite` 仅为既有验收工具保留。
  `BackgroundSnapshot.Invalidated` 只在原快照有效时产生一次，记录失效原因和失效时两侧身份；帧号 `0`
  表示发生在 render frame 外的生命周期事务。正常 WGC generation 刷新不会写该事件，避免逐帧刷盘。
- 主显示器拓扑通知由渲染所有者串行处理。`Display.Topology.Observed` 记录已应用和新观察到的
  monitor/device/bounds、上次已应用 DPI、通知时窗口有效 DPI 和 DPI 变化标记；
  `BackgroundCapture.Transaction.Begin` 固定本次事务目标；只有目标 resize
  真正完成后才写 `Display.Topology.Applied` 并更新支持报告。等待无边框权限时若又出现更新目标，旧事务
  会先完成 WGC/可见性清理，但不会重放旧目标的 resize；最新目标随后以自己的事务提交。仅配置代次、
  device recovery 或会话故障取消且没有新几何替代时，会保留已消费的 resize，避免输出永久停留在旧尺寸。
- RecordingCompatible 按 Web 版截图的透明覆盖层、`visual-max`、`bright-core`、`0.90` Alpha 上限、
  `source-over` 和未知透明背景设置拟合；LightBackground 使用同一策略，但将 Alpha 上限收紧为
  `0.85`。原生 DirectComposition 没有 DOM 背景表面的逐像素等价物，因此这两种模式都不读取桌面，
  也不是任意桌面像素的逐点捕获。`recording-compatible` 只有在用户主动选择且 Windows build
  `>=28000` 时才尝试，实际路径按 `SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly`
  顺序回退；低版本或版本探测失败时拒绝请求并保持安全模式。该入口是测试模式，不构成 Session-local
  WGC 或外部录屏兼容性的正式支持声明。
- Release Host 静态链接 Visual C++ 运行库；仍使用 Windows 自带的 D3D11、DirectComposition 和
  D3DCompiler 系统组件。四张纹理以 raw LZ4 字节编译进 EXE，运行时不读取图片，也不使用 WIC；
  仅开发用的 GPU 捕获工具使用 WIC 写出验证 PNG。

直接运行 `ba-click-fx-desktop.exe` 后，窗口保持鼠标穿透。右键通知区域中的程序图标并选择
`Exit` 可退出；也可按 `Ctrl+Alt+F12` 或备用的 `Ctrl+Shift+F12`。即使系统热键注册被占用，
程序仍会轮询同一组合键作为兜底。需要调整效果时，先启动 Host，再从同一目录启动
`BAFX.ControlCenter.exe`；Control Center 与 Host 是独立进程，关闭或退出控制窗口不会停止 Host。启用托盘隐藏后，
通知区域图标可重新打开或单独退出 Control Center；Explorer 重启后会自动恢复该入口。

可用下列命令生成完整便携包。脚本会构建 Release Host 与 Control Center，并验证 ZIP 中的文件清单、
校验和、可执行文件依赖和 Control Center 启动；输出包位于
`artifacts\local\ba-click-fx-desktop-<version>-Portable-windows-x64.zip`：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\package-test-bundle.ps1
```

普通安装版和便携版默认包含 Spout2；需要不含 Spout2 的精简版时，在上述脚本以及 Host-only
或安装器脚本后追加 `-Slim`。精简版包名带有 `-slim`，控制中心不会显示 Spout2 输出开关，
但核心点击、圆环、碎片和拖尾特效保持不变。官方 Release 只发布 Full 版的便携 ZIP、ZIP 哈希、
安装器和安装器哈希四个资产；Slim 版仅保留源码构建和本地验证，不提供预编译 Release 下载。

解压便携包时必须保留其完整目录结构。Control Center 不携带 Windows App SDK 运行时，只有在需要
通过按钮启动 Host 时才要求与 Host EXE 位于同一目录。

## 普通用户安装

Release 页面中的 `*-setup-windows-x64.exe` 是面向普通用户的单文件安装器。运行时不需要 Windows SDK、Visual
Studio、Inno Setup 或旁置 Windows App SDK；安装器会在一次 UAC 确认后完成程序文件部署、当前用户的 Sparse
Package 注册和 Control Center 快捷方式创建。安装完成后打开 Control Center，点击“启动 Host”即可开始使用。

安装器使用目标机生成的本机证书签名 Package，因此 SmartScreen 可能显示“Unknown Publisher”。Release 不提供
可单独安装的证书、MSIX、私钥或 SDK 工具；Setup 内部携带的是未签名模板和约束到 `LocalMachine\My` 的原生
签名器。公钥只导入 `LocalMachine\TrustedPeople`，签名后使用 `-DeleteKey` 删除私钥。卸载可从开始菜单或
Windows“已安装的应用”执行，默认保留安装目录
下的 `data` 用户配置；卸载会按安装用户 SID 删除本程序自己的 `BAFX Control Center` 开机启动值，
不删除 Run 键或其他程序的启动项。需要无管理员权限时可改用 portable ZIP，但它没有 Package Identity。

系统页显示“安装版”表示安装状态完整、产品版本和 Package 版本一致且匹配当前 Control Center，或已
从同样有效的备份成功恢复；主状态和备份都不存在时显示“便携版”；状态损坏、版本冲突、部分升级或
只剩备份时显示“安装状态异常”。异常不会被当作便携版，应使用当前版本安装器修复。0.2.7 不提升
配置 schema，也不迁移或删除现有 `data`、主配置、显示器 override 与 effects-only `fx-profiles`。

更新检查严格由用户点击触发。Control Center 不会在启动、连接 Host 或托盘恢复时自动联网，也不会
自动下载、替换文件或执行安装器；“打开 Release”只前往固定的
[官方最新 Release 页面](https://github.com/CialloKing/ba-click-fx-desktop/releases/latest)。0.2.4 及更早的
Control Center 不具备该入口，因此首次升级到 0.2.5 仍需用户手动下载。

安装或卸载失败时，错误框会显示失败阶段、步骤、HRESULT、脚本行号和 Inno 日志的完整路径。反馈问题时请
提供该日志文件，不要只提供错误框截图。日志中的 `BAFX_INSTALL_FAILURE:` 是便于人工定位的单行摘要，
`BAFX_INSTALL_DIAGNOSTIC_JSON:` 保存 PowerShell 版本、Windows 版本、异常类型、ErrorDetails、调用位置、
内部异常和回滚/清理关联错误。日志可能包含本机用户名与目录路径，公开上传前应先检查这些信息。

## 尚未支持或尚未验证

- HDR、Advanced Color 和物理 nits 输出声明。
- Active-FX ROI 的 AMD、Intel、HDR、Windows 11、多显示器与跨适配器真实硬件矩阵。当前开关默认关闭，
  完整金字塔 ROI 仍不代表 Differential Bloom、桌面 ROI、捕获 ROI 或 dirty Present 已经受支持。
  0.2.6 与 0.2.7 的 RTX 4060 4K 170 Hz SDR A/B 门槛均失败且未发布；后续候选在相同阈值下通过并
  归档前不发布性能收益声明，也不开始 Differential Bloom ROI。
- WGC 背景感知的外部录屏兼容性、会话长时间压力与 packaged 权限允许/拒绝矩阵。Control Center 中三种模式和
  “允许黄色捕获边框”仍是实验入口；“显示与性能”页的 HDR 开关、逐屏状态和帧率策略同样只是生产代码入口与
  诊断视图。本版不将 HDR、多显示器、混合 DPI/刷新率、跨适配器、真实 device lost 或 Session-local WGC
  作为可依赖的效果路径。
  `background-aware` 启动失败或会话中止后回退内部 FX-only transport，并撤销窗口捕获排除，避免
  回退画面被录屏器隐藏；`recording-compatible` 和 `light-background` 始终关闭 WGC。
- 当前 Windows 10 19045 portable 实测中，允许可见系统边框时 WGC 会话和背景参与正常；关闭黄色边框后，
  当前跨帧 package identity 预检以 `not-packaged / 0x80073D54 / not-started` 在 stop、WDA 变化和新
  Session/FramePool 创建前拒绝，随后回退 FX-only、恢复 `WDA_NONE`，且该控制代次没有背景参与。
  重新允许边框后 WGC 和背景参与可以恢复。
  原始证据见
  [`artifacts/spikes/spk-002/rtx4060-win10-19045-portable-borderless-async-current-head-2026-08-15`](artifacts/spikes/spk-002/rtx4060-win10-19045-portable-borderless-async-current-head-2026-08-15/README.md)。
  该结果不证明 packaged 权限拒绝或无边框成功，也不覆盖 Windows 11 权限 UI 的 Pending、取消或超时路径。
- 无论 WGC 是否可用，都不能移除 Layered/Transparent 样式来换取背景采样；这会破坏跨进程按钮点击。
- Host 已实现主显示器变化的事务化重绑定和负虚拟桌面坐标处理，但真实多显示器、跨显示器输入、
  混合 DPI/刷新率、多适配器和热插拔矩阵仍为 `Not Run`。同分辨率换屏的确定性测试只证明目标身份
  不会被尺寸相等掩盖，不等同于真实硬件验收。
- 协调屏拓扑补全会在同一轮原子应用目标、`displayKey`、override、HDR 和帧率策略；不完整快照继续保留
  最后有效资源域。Advanced Color 只有在专用颜色通知、DisplayInformation generation、显示恢复或物理
  路径证据改善时重开一次三次有限查询窗口，普通 DPI/刷新率通知不会无限重置预算。混合克隆刷新率冲突时
  WGC producer 和背景时效明确回退 `60 Hz`，Present 仍由交换链 waitable 驱动。这些是生产逻辑，相关
  真实硬件单元格仍为 `Not Run`。
- device removed/reset 后已有事件通知、一次性重建实现和主动探针，但当前只证明通知注册及主动重建后的
  重新注册；真实 GPU reset、热插拔、跨适配器以及 device-lost 下 WGC 同步关闭仍未完成硬件验收，
  因此不属于本版的支持范围。
- 自动检查、自动下载、自动安装、公有代码签名，以及无边框 WGC 的跨版本稳定性。从 0.2.5 起仅支持用户
  主动点击后的版本查询和固定 Release 页面入口；方案 C 安装器仍需用户手动运行，其背景感知能力受
  Windows 版本、权限和显卡环境影响；portable ZIP 继续作为无安装权限的备选。

这些能力即使存在实验代码或架构文档，也不属于本版的支持合同。

## 测试入口

- `ba-click-fx-desktop.exe --demo-click`：在主屏中心生成一次可见点击后继续运行。
- `ba-click-fx-desktop.exe --spout2 --demo-click`：启用透明预乘 Spout2 FX-only 输出并生成
  一次可见点击；OBS 需先安装兼容插件，把 `Spout2 捕获` 源置顶并选择
  `Premultiplied Alpha`。
- `ba-click-fx-desktop.exe --smoke-test`：执行有界的 D3D11/DirectComposition 中心像素检查并退出；
  成功退出码为 `0`。
- `ba-click-fx-desktop.exe --quit-after-ms=1000`：运行正常消息/渲染循环并在约一秒后退出，用于验证
  退出清理路径。
- `ba-click-fx-desktop.exe --device-recovery-probe`：主动重建 D3D/DComp 资源域并重渲染同一快照；只验证
  恢复实现，不制造真实 device-lost。
- `ba-click-fx-desktop.exe --frame-pacing-stall-probe --quit-after-ms=250`：内部回归入口，以永久不信号句柄
  验证运行截止检查不会被帧等待 timeout 绕过。
- `BAFX.ControlCenter.exe`：在 Host 已运行时打开 Win32 设置窗口，通过本地 IPC 读取并调整三种
  渲染模式、FX 参数、分层开关、特效 Profile、Active-FX ROI、HDR 请求和帧率策略，并用
  `GetDisplayState` 查看逐屏实际状态；系统页显示双方版本、安装状态并提供手动更新检查。它不是独立渲染器。

smoke 只证明当前 Windows 会话中的基本渲染链路可用。运行日志中的
`Support.WGC=active` 表示本次背景感知会话成功创建了 WGC 路径；随后出现背景合成日志才表示样本已
参与；`fallback-fx-only` 表示已安全降级到内部 FX-only transport。RecordingCompatible 和
LightBackground 不创建 WGC，但仍可进行人工视觉审核；这些状态都不替代 HDR、多显示器、
录屏兼容性或其他硬件矩阵验证。
