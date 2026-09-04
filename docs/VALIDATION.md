# 验证策略

## 1. 证据分层

| Tier | 用途 | 可以决定什么 |
| --- | --- | --- |
| Unity/游戏证据 | 视觉与运行时真值 | 时序、输入帧态、形状、材质、层顺序、Bloom 观感 |
| ba-click-fx | 产品行为参考 | 配置命名、兼容接口、native/Web 增强 |
| 原生中间 buffer | 实现验证 | 定位数值、滤波、合成和回归 |

低层证据不能推翻高层视觉真值。若 Web 与 Unity 不一致，先记录差异，再以 Unity/游戏证据校准。

## 2. 自动化层级

### L0：纯函数

- intensity 语义与 output policy；
- background binary validity、路径单向锁存、饱和时间差与边界；
- WGC stop 四阶段调用前后事件、能够返回的异常继续清理、owner/caller 线程一致性、`OverallSucceeded`、
  失败后的 sticky 重启阻断、retry token 不可绕过，以及 included/FX-only 回退；watchdog 可注入处理器覆盖
  arm/disarm、单次触发与重新启动；背景快照失效单槽邮箱必须保留首个未消费原因和完整身份；
- WDA 运行期健康检查必须在 Sensor 活跃后延迟首次查询、最多每秒一次、时钟回退或停用后重置；排除丢失
  必须先 stop Sensor，再恢复 `WDA_NONE` 和 FX-only profile，同一稳定请求不得被循环自动重启；device
  recovery 只可重试故障调用前锁存为实际活跃的 Sensor，不得让异常展开期间的 Closed 回调覆盖该事实，
  也不得给已有 FX-only 终态注入新的 `retryToken`；
- `allowSystemBorder` 必须进入 capture request identity；`true -> false/Start 失败 -> true` 往返应执行
  完整 stop/fallback/restart 动作，普通 Start 失败不得错误触发 sticky 重启阻断；
- 无边框授权是资源动作前的独立 `RequestBorderlessAccess`。`Pending` 不推进动作、不消耗固定 action
  budget，并保持原 effective path；拒绝、超时或所有者取消只生成一次 FX-only 回滚，含 resize 的最长
  成功/失败序列不得超过 8 个动作。broker 拒绝、错误或超时保留当前事务的 resize 并对稳定请求保持终态；
  owner cancel 清除旧请求身份，使相同捕获配置可在新控制代次重新进入权限动作。新输出/显示目标或
  shutdown 必须丢弃旧 resize；仅配置代次、恢复或会话故障取消且没有新几何替代时必须保留它；
- ROI alignment/guard 与自适应状态机：独立 oracle 使用固定 seed 覆盖至少 10,000 组矩形、整数溢出、
  负屏幕原点、奇偶目标尺寸和 diffusion `4/6/7/10`；完整计划 candidate/full 比例不超过 50% 才进入、
  已进入后超过 65% 才退出，接近全屏、触边和无效计划的正确性原因必须先于收益判断；
- effects-only Profile codec/store：启动目录必须固定包含“Unity 原版”“轻量”“纯点击”“纯拖尾”四个内置项；
  每个自定义项使用 `fx-profiles/<名称>.json` 单文件保存完整、严格的平面 `EffectsConfig`，合法文件可跨实例
  save/reload/delete，损坏或超限文件只能被忽略，不能移除或污染内置项；
- 全局快捷键 codec：schema 20 必须包含 `togglePause`、`toggleAlwaysOnTrail`、`nextFxProfile`、`shutdown`
  四项，默认均为 `null`；字段完整的 schema 14 至 19 只按固定链迁移，schema 19 只补充四项空绑定。
  解析拒绝未知/重复/缺失字段、非法修饰位、F12、仅修饰键和重复组合，序列化往返保持动作顺序和键值；
- finite sanitize、component-wise non-negative 与 isotonic test vectors；
- fixed-step simulation 和 deterministic random；
- `FXTouch` 释放后的 `1 s` 仿真寿命、桌面暂停冻结、边界帧呈现后回池，以及
  `60/120/144/240 Hz` 下不随 Present 频率提前回收；
- PointerFrameAdapter 的跨帧 held、Raw 边沿原序保留、普通 Up-only 帧不移动和边沿后尾随 Move 抑制；
  PointerFrameDispatch 的 Down/Held/Up 帧态归约、Down→Held→Up 固定顺序、三态同时为 true 时各执行
  一次、统一帧位置、统一 `renderTime`、QPC 输入相位隔离，以及 native Cancel 最终硬边界。
- 录屏兼容测试模式的版本能力判定与 Host 门禁：build `19045`、`26100` 和 `27999` 拒绝，
  `28000`、`28001`、`29000` 及更高 build 接受，版本探测失败拒绝；拒绝请求不增加 generation、
  不写配置文件，旧配置在启动时回退到 `light-background`，回退保存失败仍保持内存安全模式。
  `recording-compatible` wire value 和 schema 不变，`LightBackground` 的 `0.85` Alpha 合同与测试
  模式的 `0.90` Alpha 合同分别验证。该测试仅覆盖透明 overlay 的外部录屏观察，不作为 Session-local
  exclusion 能力证据。
- `performance.effectsMode=core` 的低配测试合同：保留中心圆盘、圆环、点击/拖拽碎片和拖尾，仅跳过
  Bloom 与 WGC/背景捕获，固定保守 SDR 和 60 FPS；切换进入或离开 core 时清理旧几何，日志记录请求档位与
  实际 FX-only 路径。该模式只评估低性能机器的流畅度，不作为完整背景合成、HDR 或 Session-local
  exclusion 证据。
- Raw Input 基线报告器的 fixture contract：验证 paired manifest、Host SHA-256、两种背景模式、
  接收窗口几何、光标精确恢复、Down/Up 计数、延迟样本，以及 `passed`、`unsupported` 和配对能力
  不一致三种结果。`unsupported` 是环境能力结果，不得写成输入延迟通过。
- 诊断日志留存合同：当前文件最大 `8 MiB`，最多三个轮转备份，总预算约 `32 MiB`；轮转、遗留备份清理、
  超大记录压缩标记和 `ClearLogs` 返回的删除统计均保持可审计。清理失败只报告失败文件数，不改变运行配置。

### L1：GPU 离屏

- shader compile/reflection；
- MaterialOutputs MRT；
- DifferentialPrefilter FP32 差值；
- 浅色/深色背景下点击与拖尾跨获取/保留边界及恢复序列的逐像素稳定性；
- 近白到纯白背景的连续 FP16 采样步进，确认背景变化不会在点击或拖尾上产生透明/不透明跳变；
- Bloom mip/down/up；
- FinalOverlay FP16 readback；
- Active-FX ROI WARP 覆盖完整 prefilter/down/up 和 resolve 掩码，以及移动与不相交矩形、内容消失、
  开关切换、全屏/ROI 往返、四边四角、奇数 resize、点击、拖尾、负 scRGB、HDR 极值、Spout2、共享
  Bloom 目标和 Context1 fallback。确定性同适配器的全部 FP16 Bloom 层与最终输出必须逐元素 bit-exact，
  且不得出现 NaN/Inf；有/无背景时，primary 与 recording-rebuild 不得互相误记。

### L2：窗口与 API 集成

- PMv2 坐标、Raw Input 到单一帧边界当前位置的映射、click-through overlay；受控 Down/Up
  采集的真实 Windows/输入设备 smoke test 仍需单独执行，fixture contract 不替代该硬件检查；
- 输入消费/呈现边界上的 Raw 边沿保留、Down/Held/Up 帧态归约、普通 Up-only 帧不移动、常驻拖尾分流，
  以及 QPC 只影响可选采样相位；
- `FxTrailTimeScale` 的逐 Update parking 状态机已在参考层验证；`SimulationRuntime` 已按当前游戏
  审计复现 `SyncComponentPool<FXTouch>` 的 FIFO 失活对象复用，并由 L0 测试锁定最早归还对象及其
  相邻组件状态的再次取回。桌面 Host 仍无游戏 `Time.timeScale` 来源，因此生产路径尚未调用 parking 入口；
- DComp visual/swap chain resize/present；
- 可选 D3D11.4 device-removed event 对 frame-latency 和暂停态 WGC 背景帧的等待优先级、异常可选句柄、
  错误锁存，以及已确认 device-lost 到一次性 render/Present 恢复边界的路由；通知不可用时保留连续
  timeout 轮询，`desktop_frame_pacing_stall` 必须在自身截止时间内退出；
- WGC session state machine、每个不可取消 stop 调用前可独立观察的阶段检查点，以及渲染阶段真实 stop
  诊断跨无 sensor 清理动作的一次性交接；生产 watchdog 必须在 stop 首条日志前启动，正常完成时撤销，
  默认 `10 s` 到期时调用进程终止处理器。独立子进程探针必须通过生产默认处理器以精确退出码 `124` 结束；
  超时不再继续 WDA/profile 动作。只有已成功写入的四个阶段级 `begin` 能定位具体阻塞调用，`Stop/begin`
  只证明 watchdog 已启动；这些证据不表示 WinRT Close 可取消或真实 device-lost 已通过；
- 活跃 WGC 的 WDA 只读回查成功只进入性能窗计数；失败必须产生一次含控制代次、事务状态、期望/观察 affinity
  和 Win32 错误的结构化事件，并在下一次 Present 前完成 WGC stop 与 FX-only 回退；
- WGC Session 专属排除 Spike 必须在 `WDA_NONE` 下运行，并对 capture Session 运行时 QI
  `IDisplayGraphicsCaptureSession`、`IGraphicsCaptureSession7`，对 frame 运行时 QI
  `IDirect3D11CaptureFrame3`。每个 QI 和 Set/Get 排除列表调用都记录原始 HRESULT；SDK 头文件可见性
  不得替代运行时能力。Set/Get 成功还必须回读完全相同的 Overlay WindowId，且 Set 返回的
  configuration iteration 与后续 frame iteration 建立可重复、可判定的关系；关系不稳定时只能报告
  `NotVerified`，不得发布新的背景快照或宣称生产可用；
- Session 排除离线 verifier 必须从 baseline/excluded/restored 原始 `.rgba16f` 重算 Overlay ROI、远端
  control ROI、哈希、最大误差和改变像素数，并拒绝非有限 FP16、黑色保护面伪证、旧 frame、错误
  iteration、跳序、WindowId 不匹配、放宽阈值、重复字段、伪造汇总和输出目录路径逃逸。Frame、
  FramePool、Session、FrameArrived 与 Item.Closed 的创建/释放 ledger 必须全部归零，watchdog 不得超时；
- 无边框权限预检必须在 stop、WDA/profile 变更和新 Session/FramePool 之前开始；等待期间 Host 继续
  消费消息、Raw Input、呈现和 IPC。`WGC.BorderlessAccess.Checked` 记录原始 `Control.Generation`、事务
  动作序号、`AllowSystemBorder`、状态、HRESULT、`AsyncStatus`、`ElapsedMs`、`CancelRequested` 和
  Allowed；配置变化、resize、device recovery 或退出必须显式取消旧请求并只回滚一次。拒绝后必须恢复
  `WDA_NONE`、保持请求模式为 `background-aware`、实际路径回退 FX-only，合法请求变化后仍可恢复 WGC；
- 可见内容每帧 drain；暂停或空闲时的 sensor-only maintenance 只规定最高 `20 Hz`，不要求每秒精确
  20 次，且不得创建批次快照、执行 Bloom/Present 或计作呈现帧；
- 只有原快照有效时才产生一次 `BackgroundSnapshot.Invalidated`。参与和失效事件必须携带已应用的
  `Control.Generation`、`Frame.Id`、WGC epoch/generation 与 snapshot epoch/generation；参与证据只能来自
  成功 Present 后的帧诊断，模式切换后旧 snapshot identity 不得再次进入最终复合；
- 显示目标身份必须包含 monitor handle、设备名和物理边界，同尺寸换屏不得归约为 no-op；拓扑失效只由
  渲染所有者转换为 `StopSensor -> ResizeOutput -> StartSensor` 事务。`A -> B -> C` 权限取消不得执行 B 的
  resize，shutdown cancel 不得移动窗口；`Display.Topology.Observed/Applied` 必须区分观察目标与已应用目标，
  同屏 DPI-only 通知必须同时记录已应用 DPI、窗口有效 DPI 和变化标记；
- `GetDisplayState` schema 4 必须拒绝旧版本以及未知、重复、缺失和非法字段，并能解析 Host 真实生成的
  完整会话。每个 session 的 `activeFxRoi` 必须包含固定字段的 primary/recording-rebuild 路径和
  prefilter/downsample/upsample/resolve 分阶段像素，Control
  Center 只在页面可见时按 1 秒轮询、离页停止，样本超过 3 秒标记 stale；Host 退出或 IPC 失败不得
  修改配置或伪造新快照。工程面板还需覆盖 96/144/192 DPI 和键盘 Tab 顺序；
  离线 override 只在全局拓扑完整时具有权威性。Windows SDK 19041/22621/26100 Actions 均构建 Host、
  Control Center 和 Identity Signer 的完整目标，并记录 runner 实际安装的 SDK 清单；不以当前运行系统
  缺少 Windows 11 API 为测试失败条件。该 SDK 编译覆盖不替代 build `28000+` 的真实运行时或 WGC 证据；
- monitor/adapter rebuild。

### 全局快捷键控制合同

快捷键验证必须同时覆盖 Win32 注册、Host 配置事务、IPC 严格解析、Control Center 草稿和进程边界：

- Host 使用 `RegisterHotKey`/`WM_HOTKEY`，注册标志必须包含 `MOD_NOREPEAT`。进程边界测试应占用一个组合，
  证明启动注册失败不会阻断 Host 或其他绑定；释放占用后 `RetryHotkeys` 只恢复已保存组合，不写配置；
- `SetHotkeys <generation> <hotkeys-json>` 必须先保留旧注册并准备全部新组合，再原子写入主配置，最后发布
  新动作映射并清理旧注册。准备失败、generation 冲突和写盘失败保持旧配置、旧注册及代次；成功恰好推进
  一次控制 generation，但不推进只面向渲染配置的 generation。提交后激活无法确认或清理失败必须返回
  可诊断错误，并以已保存配置为权威要求重启；
- 四项动作分别覆盖暂停／恢复、切换常驻拖尾、下一个特效预设和退出 Host。重新绑定后排队中的旧
  `WM_HOTKEY` 不得被解释为新动作；清除绑定必须释放系统注册。`MOD_NOREPEAT` 的长按行为仍需真实 Win32
  人工检查，单元测试不能替代系统消息行为；
- 录制期间旧注册保持占有但动作被抑制，候选只进入草稿。只有匹配的非零 capture token 查询续期；
  失焦、取消、30 秒总时限或 5 秒未续期结束录制并恢复动作。空闲状态查询不得发送 token `0` 作为续期；
- Control Center 必须在快捷键保存前提交待处理的普通配置 patch，拒绝重复绑定，保留离页草稿，离开页面、
  断线和关闭时结束录制。外部客户端修改绑定后禁用保存，直到撤销或基于权威状态重新录制；Host 已保存
  状态与草稿相同时必须收敛 generation 和 dirty 状态。关闭时的保存/丢弃/取消、离线丢弃提示，以及
  “重置默认”保留已保存快捷键均需人工检查；
- `GetHotkeyState` 的完整状态组必须严格解析；支持报告固定验证 `Hotkeys.StateScope=startup`、注册掩码、
  四项 `Registered/Error`、`CleanupError` 和 `Exit.PollingFallback=disabled`。该组是启动快照，不得用来
  宣称导出报告时的实时注册状态；生产代码和文档不得再提供固定 F12 或轮询退出合同。

### Host-owned effects-only Profile 合同

Profile 自动化必须同时覆盖存储、IPC、Host 事务和 Control Center 状态解析，不能只验证 JSON 往返：

- `GetState` 必须发布可严格解析的 `fxProfileCatalog`、`activeFxProfile` 与 `fxProfileWarning`，并区分四个内置项和自定义项；
  当前 effects 与任一 Profile 不完全相等时，活动名称必须是“自定义”；
  损坏、冲突或不可读文件必须被跳过并产生非空 warning，不能静默从控制中心消失；
  与其他现有项 effects 完全相同的保存必须拒绝；同名项可以幂等重存或覆盖，且启动扫描必须使用确定性顺序，
  避免活动 Profile 身份随目录枚举顺序漂移；
- `SaveFxProfile <generation> <name>`、`ApplyFxProfile <generation> <name>` 和
  `DeleteFxProfile <generation> <name>` 必须接受含空格的 UTF-8 名称，并先比较同一个 Host generation。
  过期请求返回 `generation_conflict`，不写文件、不改变配置/目录、不增加 generation；
- 保存通过临时文件、flush 和替换发布单个自定义文件；应用通过主配置原子写发布只替换 `effects` 的候选；
  删除只允许自定义项，并以单文件移除作为提交点。三种操作失败时均保持旧的可观察状态；成功时恰好增加
  一次控制 generation，只有应用同时增加一次独立配置 generation。纯目录保存/删除的配置 generation
  必须保持不变，避免触发渲染配置重应用；覆盖或删除四个内置项必须拒绝；
- Profile round-trip 必须证明 `background`、`display`、`input`、`performance` 和 `system` 均保持逐字段
  不变。`performance.activeFxRoiEnabled` 属于明确的负向断言：自定义文件中不得出现，应用 Profile 也不得
  改变 ROI 开关。

### L3：硬件/视觉

- 四个 Spike；
- Session 专属排除能力与像素证据必须分层记录。`capability.status` 仅允许
  `Unavailable | Available | Rejected | NotVerified`，`evidence.result` 仅允许
  `Passed | Failed | Not Run`；QI 明确不支持归 `Unavailable`，接口存在但 Set/Get 被系统拒绝归
  `Rejected`，只有 Set/Get 往返成功才可归 `Available`，而 `Passed` 还要求 iteration、三阶段像素和
  资源清理全部通过。Spike 失败只报告能力，不改变产品运行状态；
- 生产接入前，Session 专属排除必须在真实目标系统达到 `Available + Passed` 并补齐所需硬件矩阵。
  接入顺序固定为 `SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly`：先保持 Overlay
  `WDA_NONE`，创建 Session 后设置 WindowId 排除列表，并在对应 configuration iteration 的 frame 到达前
  禁止发布新的 `BackgroundSnapshot`；Session-local 失败才回退旧 WDA，再失败才进入 FX-only，诊断必须
  区分三条实际路径；
- 当前代码已将上述顺序接入用户主动选择的 `recording-compatible` 测试模式：build `>= 28000` 时
  请求 Session-local WGC，失败时回退旧 WDA/FX-only；默认 `BackgroundAware` 仍保持旧 WDA 路径。
  该接入只证明状态机、日志和回退契约已闭合，不把本机旧系统的 `Unavailable` 结果升级为能力通过。
- portable `not-packaged`、packaged 权限拒绝和无边框成功必须作为三个独立单元格记录，不能互相替代；
- 真实 device-lost 下 WGC stop 的阻塞阶段、退出码 `124` 和重启恢复必须作为独立单元格；当前保持
  `Not Run`，不能用子进程终止探针代替；
- 真实多显示器、混合 DPI/刷新率、热插拔与跨适配器重绑定保持 `Not Run`；同屏通知探针和纯状态测试
  不能替代这些单元格；
- Unity Golden 时间序列；
- SDR/HDR 与混合刷新率矩阵；
- 外部录屏观察。

当前首个目标机单元格已执行并由离线 verifier 接受，证据目录为
`artifacts\local\spikes\spk-002-session-exclusion\DESKTOP-AE81VOU-1c7bd07\`；其能力结果为
`capability.status=Unavailable`、`evidence.result=Not Run`，三个必需接口 QI 均返回 `E_NOINTERFACE`。
这只证明旧系统能启动 collector 并审计“不支持”，不构成 Session-local 能力通过；真实目标支持矩阵和
默认路径提升门禁仍保持 `Not Run`。外部录屏/OBS、HDR、多显示器、真实 device lost 和 packaged 权限矩阵也保持
`Not Run`；离线 verifier、编译成功或单机 API 调用成功均不能替代这些硬件/权限证据。

### Session-local 后续门禁

后续验证按以下顺序推进，后一阶段不得用前一阶段的模拟或离线结果替代：

| 阶段 | 必须产出 | 通过条件 | 未通过处理 |
| --- | --- | --- | --- |
| 合同冻结 | schema、阈值、verifier 测试 | 离线合同全通过，默认真实 CTest 关闭 | 只修合同/测试，不改产品路径 |
| 目标机单机 | `session-exclusion.json`、原始 FP16、预览、日志、`verification.json` | `Available + Passed`、三阶段 ROI、iteration、ledger、watchdog 全通过 | 标记 `Unavailable`/`Rejected`/`NotVerified`/`Failed`，不得接入生产 |
| 硬件/权限矩阵 | 每个 OS/GPU/显示器/色彩/身份独立目录 | 目标支持矩阵所需单元格逐格通过 | 缺失单元格保持 `Not Run`，收窄支持声明 |
| 生产接入评审 | 状态机设计、故障注入、回退诊断和真实回归 | Session-local、旧 WDA、FX-only 三条路径可区分且顺序正确 | 保持旧 WDA 默认路径，禁止发布新能力 |

生产接入前必须证明 snapshot 发布边界：Set 成功后，只有收到并验证对应 configuration iteration 的
frame 才能产生新的 `BackgroundSnapshot`；旧 iteration、跳序事件和清理阶段的 frame 只能被丢弃并记录
原因。任何 `NotVerified` 都是证据不足，不得折算为 `Rejected` 或 `Passed`。

目标机运行脚本拒绝文件路径和非空输出目录，确保 collector/verifier 重跑不会覆盖既有 JSON、原始帧
或诊断日志；重跑必须使用新的 revision 或显式指定新的输出目录。父进程超时还必须比 collector
采集超时至少多 `5000 ms`，否则脚本会拒绝启动，避免父进程先于 collector watchdog 终止而丢失失败证据。

## 3. Golden case 契约

每个 case 固定：

- commit、shader hash、配置 schema；
- OS build、adapter LUID、driver、显示色彩模式；
- viewport、DPI、背景编码与参考白；
- QPC frequency、消息分派 QPC、统一 `renderTime`、固定模拟步长、随机种子；
- 呈现边界、锁存位置、Raw 边沿、归约后的 Down/Held/Up 帧态，以及拖尾常驻开关、出界、重入和
  边沿后尾随 Move 边界。

显式种子的生产 C++ 模拟坐标只属于实现内确定性回归，不等同于 Unity Golden。Unity 为每个
ParticleSystem 使用独立的引擎随机流；原生尚未复现这些随机流，因此普通点击/拖拽捕获仍必须使用
与随机布局无关的数量、包络、径向能量和感知指标。只有下述由 Unity 观察值直接构造的
50/100/120/250/450 ms 诊断快照可以逐坐标、逐像素比较；它们不能进入生产 `Simulation`，
也不能据此宣称随机流等价。

Unity 粒子状态观察夹具固定为 `FX_Touch_{0050,0100,0120,0250,0450}ms_particle-state-v2.json`。
它们使用 schema 2、`1950x1097`、`seedBase + index * seedStride`（`seedStride=7919`），
保留 `GetComponentsInChildren(true)` 的系统顺序和 `GetParticles` 的粒子顺序，并导出局部/世界坐标、
投影像素、速度、寿命、尺寸、旋转、颜色、Custom1 及从 `ParticleSystemRenderer.BakeMesh`
最终 UV 解析的 `atlasFrame`。只有启用 Texture Sheet 的三角系统使用 BakeMesh；其他系统固定为 0。
导出器必须将烘焙四边形与 `GetParticles` 世界坐标唯一匹配，不得用 `randomSeed` 反推帧号。
Unity 导出器在同一批处理中独立生成两次，
要求 UTF-8 JSON 字节完全一致（`deterministic.runs=2`、`byteIdentical=true`）；native 校验器只验证
该可复现序列化合同，不把它升级为 native 随机流等价声明：

```powershell
$particleRoot = "D:\WebProjects\BA鼠标输入与点击特效系统\UnityMouseFxLab\UnityMouseFxLab\Reference\Diagnostics\ParticleStates"
$fixtures = @(50, 100, 120, 250, 450 | ForEach-Object {
  Join-Path $particleRoot ("FX_Touch_{0:D4}ms_particle-state-v2.json" -f $_)
})
foreach ($fixture in $fixtures) {
  python -B tools\verify-unity-particle-fixture.py $fixture
}
python -B tools\generate-unity-particle-fixture.py @fixtures --check
```

生成数据只编入 `bafx::reference` 与 Capture 工具。映射固定为：`ring -> CenterDisk`、
`MeshTri -> DissolveRing`、`Ring (3)/(4) -> Triangle`；根系统不绘制。Unity `projectedPixel`
使用左下原点，转换到 native 顶部原点时执行 `y = height - y`。粒子当前尺寸乘系统 XY 缩放和
`height / 2`，MeshTri 再乘 `Cylinder002` 的完整直径 `2.127337`。RGB 从 sRGB 转为线性，Alpha
保持原值；MeshTri `Custom1.x` 映射到硬溶解阈值，三角 `atlasFrame` 来自 BakeMesh UV。
材质强度、render queue 与 Bloom 归属仍由 Prefab/Shader 合同提供，而不是由 Fixture 猜测。

固定粒子 GPU case 使用 manifest contract v2 按年龄锁定 source path/SHA/particleCount，
并用相同 WARP/FP16 渲染器检查五张 Unity Golden：

```powershell
$revision = git rev-parse HEAD
$fixtureRoot = "artifacts\local\gpu-captures\$revision-unity-particle-fixture"
build\x64\src\capture\Release\ba-click-fx-gpu-capture.exe `
  "--output=$fixtureRoot" `
  "--case=unity-particle-fixture" `
  "--all-layers" `
  "--revision=$revision"
python -B tools\verify-golden-metrics.py `
  "--native-root=$fixtureRoot" `
  --require-layers
```

该 case 的统计容差为能量/覆盖各 `2%`、径向直方图 L1 `0.02`、色度 L1 `0.01`；50 ms
质心距离容差为 `0.25 px`，动态时间片为 `1.25 px`。50 ms 尚无可见溶解 Mesh，保留近字节级像素门禁：
最大 8-bit 误差 `16`、平均误差 `0.01`，最大通道误差大于 `1`/`2` 的像素不超过 `128`/`64`。
其余时间片包含 Unity 硬件 D3D 与 WARP 对硬裁剪动态边缘的栅格覆盖差异，统一限制平均误差 `0.04`，
最大通道误差大于 `1`/`2`/`32` 的像素不超过 `18000`/`7000`/`128`；合成硬边 1 px 位移回归必须失败。
PNG 仍须先与同次 FP16 `FinalOverlay` 重建结果一致，并继续执行全部中间层合同。

导出以下命名层：

```text
DirectSurface
BloomSeed
Prefilter_Down00
Down01..Down05
Up00..Up04
BloomResult
FinalOverlay
```

`Coverage` 与 `DirectEmission` 是组合进 `DirectSurface` 的 MRT 语义，不是独立捕获文件。Tri2 三角碎片
应同时贡献 `DirectSurface` 和 `BloomSeed`：它使用游戏 `FX_SHADER_Additive_0`，在 UI HDR 缓冲绘制后
进入全场景 Bloom。对 FP16 使用每通道绝对/相对容差，并在最终图上补充感知误差；
不得用 PNG hash 代替数值比较。

`DirectSurface` 保存 Coverage 与 DirectEmission 的组合结果。`BloomSeed` 保存允许进入 Bloom
的材质（包括 Tri2），因此逐像素必须满足 `BloomSeed.a <= DirectSurface.a`；只有明确标记为
非 Bloom 的材质才可以让两者在同一像素明显不同。
Bloom 传播可以扩张最终传输覆盖范围，但不能抹掉已有 Coverage，所以还必须满足
`DirectSurface.a <= FinalOverlay.a`。这两项使用与其他 FP16 层相同的 `0.002` 数值容差检查单向关系，
不要求三个 Alpha 通道相等。

捕获 manifest 从 schema 2 起为每个层记录 `alphaSemantic`，它是桌面诊断合同而非 Unity 原生字段：
`DirectSurface=authored-coverage-union`、`BloomSeed=bloom-source-coverage`、Down/Up
`=bloom-transport-energy`、`BloomResult=bloom-transport-coverage`、`FinalOverlay=coverage-union`。
`BloomResult` 是最终 Bloom 金字塔经过全分辨率四点采样、曝光和 Alpha 饱和后的独立 FP16 层，尺寸与
`DirectSurface` 相同；它与 `FinalOverlay` 在同一次捕获专用 MRT 调用中写出，普通桌面呈现路径不增加
额外 pass。当前层语义只验证 native shader 自洽，不宣称存在 Unity 对应字段。

schema 3 同时锁定 `captureProfile=fx-only` 和
`compositeFormula=direct-plus-bloom-result-max-alpha-v1`。验证器逐像素重建
`FinalOverlay.rgb = DirectSurface.rgb + BloomResult.rgb` 与
`FinalOverlay.a = max(DirectSurface.a, BloomResult.a)`。RGB 使用
`0.002 + 0.001 * max(abs(actual), abs(expected))` 的 FP16 绝对加相对容差，Alpha 使用 `0.002`
绝对容差；报告最大/平均误差、最大容差占用和首个越界坐标。该门禁取代旧的全图 Bloom 能量比近似，
Down/Up 的能量守恒与单调传播检查继续保留。背景感知和录屏拟合有不同复合公式，不得套用此 profile。

原生层级捕获使用固定 WARP、`1950x1097`、中心点击、种子 `20260716`，直接读取
`Present` 前的 FP16 资源，不依赖会漏掉 DComp visual 的 PrintScreen：

```powershell
cmake --build --preset release --target ba_fx_gpu_capture
$revision = git rev-parse HEAD
build\x64\src\capture\Release\ba-click-fx-gpu-capture.exe `
  "--output=artifacts\local\gpu-captures\$revision" `
  "--all-layers" `
  "--revision=$revision"
python -B tools\verify-golden-metrics.py `
  "--native-root=artifacts\local\gpu-captures\$revision" `
  "--require-layers"
```

默认十个时间片只写 `FinalOverlay.rgba16f` 和黑底 sRGB PNG；指定 `--all-layers` 时再写
`DirectSurface`、`BloomSeed`、全部 Down/Up mip、`BloomResult`。`.rgba16f` 是顶部原点、little-endian RGBA
half 数值证据；PNG 不执行 unpremultiply、强制不透明黑底，仅用于与 Unity PNG 观察和感知比较。
指标门禁必须同时通过十个时间片及 FP16 分层检查；失败后先解释实现或参考证据，不得放宽阈值。

拖拽/Trail 使用独立的配对诊断：`140 ms` 内从 `(759, 548.5)` 水平移动到 `(1191, 548.5)`，
先静止推进一段，再以 12 段等距移动；WithTrail 与 NoTrail 共享同一份粒子快照，Trail 固定为 Unity
诊断所用的两个端点。原生捕获还额外生成不含粒子的 `20 px` 两点 Trail，避免不同引擎的粒子随机流
经过 sRGB 量化、饱和与 BrightPass 后污染弱光尾部比较：

```powershell
$dragRoot = "artifacts\local\gpu-captures\$revision-drag-trail"
build\x64\src\capture\Release\ba-click-fx-gpu-capture.exe `
  "--output=$dragRoot" `
  "--case=drag-trail" `
  "--all-layers" `
  "--revision=$revision"
python -B tools\verify-golden-metrics.py `
  "--native-root=$dragRoot" `
  "--require-layers"
```

门禁分成两条互补合同：

- `WithTrail - NoTrail` 仍要求全图逐通道负差为零；其能量、能量质心、色度、覆盖和包围盒都只在
  `max(channel delta) > 24` 的高信号主体内阻断。`> 2` 的弱光尾部仍输出诊断，但不作为跨随机流
  失败条件。高信号主体容差依次为能量 `5%`、覆盖 `3%`、质心 `x/y = 2/3 px`、色度 L1
  `0.01`、包围盒每边 `4 px`。
- `FinalOverlay_TrailOnly20px` 相对纯黑比较，独立锁定 Trail 材质、几何与 Bloom：能量 `15%`、
  覆盖 `12%`、质心每轴 `1 px`、色度 L1 `0.03`、包围盒每边 `12 px`。

拖拽 case 的 manifest 必须精确声明两个比较帧及三张 Unity 参考图；验证器同时核对 FP16 文件长度。
`case.contractVersion=1` 独立版本化这套新增夹具；Golden 比较前会从 `.rgba16f` 按捕获端相同的
linear-to-sRGB 规则重建并核对 PNG，避免陈旧预览与数值层错配。
该诊断验证距离发射、两点 Trail 几何/材质与 Bloom，不冒充真实逐帧 TrailRenderer 采样时序验证，也不
验证 OS 消息到 Unity Legacy Input 帧态；该输入合同由独立 Player 黑盒证据
`Reference/Diagnostics/Input/FXTouch_LegacyInput_DownUpDown.{md,json}` 锁定：Unity `2021.3.45f1`
Windows Player 在完整前台、命中测试和焦点门禁下接收 `Down-Up-Down`，下一聚合帧记录到
`GetMouseButtonDown(0)=true`、`GetMouseButton(0)=true`、`GetMouseButtonUp(0)=true`。原生确定性测试
同时锁定 Raw 边沿原序保留、布尔帧态归约和 Down→Held→Up 派发。该证据只覆盖这一边沿排列，不证明
其他排列，也不证明游戏使用的 Unity `2021.3.56f2` 行为相同。
`--json` 输出固定的 schema v1 envelope；退出码 `0/1/2` 分别表示通过、指标失败和输入/参数错误，
参数错误也不会混入人类可读文本。

## 4. Differential Bloom 属性测试

随机输入覆盖普通值、零、负 scRGB、HDR 极值、NaN 和 Inf。验证：

1. 有限非负 `F` 下，prefilter 输出有限且非负；
2. 对所有 `x <= y`，候选 `H` 满足 `H(x) <= H(y)`；
3. `F=0` 时差值为零（容差内）；
4. K 的所有 down/blur/up/combine 权重非负且零偏；
5. ROI 与全屏在声明的 exact/error bound 内一致。

Lanczos 或带负瓣 bicubic 不得进入等价性路径。

v0.2.7 的纯函数规划器测试覆盖 10,000 个固定种子随机矩形、移动 dirty rect、四边四角、奇偶目标尺寸、
diffusion `4/6/7/10`、空区域、非法矩形和有符号坐标膨胀溢出。每级 down/up 与 resolve 矩形既要覆盖
布尔前向非零支持，也要满足反向采样依赖；奇数尺寸的逐像素 oracle 锁定不能只依赖旧
`basePlan.bloomOutput`。

`gpu_pipeline_warp` 对同一 FP16 场景比较全屏参考、全清预热和稳态局部金字塔，要求 prefilter、所有
Down/Up、BloomResult 和 FinalOverlay 在同一 WARP 适配器上逐元素 FP16 精确一致。稳态同时覆盖同一
writer、同一矩形直接覆盖且 `clearedPixels=0`，以及矩形移动时 `ClearView + scissor` 清理旧区。
矩阵还覆盖点击、拖尾、移动/不相交 ROI、负 scRGB、HDR 极值、四边四角、Spout2 FX-only、resize 后
奇数尺寸、空帧重启、Context1 缺失及 `D3D11_OPTIONS.ClearView=false`。背景差分 primary 必须保持全屏；
共享目标同帧已全屏写入时，recording-rebuild 只能报告全清预热/共享写入原因。

这些用例证明 0.2.7 已接入的纯特效 prefilter/down/up 正确性，不证明真实 GPU 性能。本版的
稳态纯特效 ROI 会在逐字段验证后的 dirty rect 内清理并写入最终输出，再由 `Present1` 提交同一矩形；
预热、背景感知、录制重建及任一合同不满足的帧仍完整输出。WARP 还覆盖前后帧区域并集、FP16、BGRA8
和双 RTV/录制路径，完整 CTest 为 `45/45`；这些确定性结果不能外推 DWM 可见正确性、整机收益或功耗。

## 5. 发布门槛

一个 release candidate 至少满足：

- clean configure/build/test；
- L0/L1 全通过；
- 当前支持矩阵内的 L2 全通过；
- ADR 状态与实际 Spike 证据一致；
- 无 `Passed` 项依赖人工口头结论；
- WGC 失败、背景过期和普通 SDR 白底均有可见且有文档的降级；
- Unity Golden 的关键时点无未解释回归。

未具备的硬件场景保留 `Not Run`，并从该版本的支持声明中排除。

### 5.1 Active-FX ROI v0.2.6 专用发布门

专用 collector/reporter 必须使用当前 schema 19 配置。每个场景使用同一 EXE、同一输入/显示条件，唯一
配置差异必须是 `performance.activeFxRoiEnabled`；固定执行 5 个 ABBA 块，共 20 次采集，每次先预热
5 秒，再采样 30 秒。原始采集、Host SHA-256、完整配置和配对顺序必须进入 manifest，测试不得依赖
GitHub 网络或在失败后改变阈值。

RTX 4060、4K 170 Hz、SDR 的发布结果必须同时满足：

- `Applied/Requested >= 95%`，首级绘制像素比例不超过 45%；
- Prefilter GPU p95 至少降低 25%；Bloom/final p95 至少降低 `max(5%, 100 us)`；
- 10 组配对中至少 8 组 ROI 不慢于全屏；
- FPS 降幅不超过 1%，CPU、Present 与 p99 恶化不超过 5%；
- GPU pending 最大值不超过 1，查询、节流和状态错误均为 0。

触边、面积回退及无 Spout2 的 `background-aware` 场景必须 100% 命中预期原因并保持输出 exact，性能
恶化不得超过 `max(3%, 100 us)`。若任一端到端门槛未通过，不得放宽阈值、发布性能声明或发布
v0.2.6；下一轮直接转入完整 Bloom down/up/resolve ROI。

2026-08-24 的正式执行使用：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass `
  -File tools\collect-active-fx-roi-ab.ps1 `
  -Executable build\x64\src\desktop\Release\ba-click-fx-desktop.exe `
  -Configuration artifacts\performance\active-fx-roi-v0.2.6-base-config.json `
  -OutputDirectory artifacts\performance\active-fx-roi-v026-rtx4060-4k170-sdr-center-click-20260824-r2 `
  -Scenario center-click `
  -MeasurementPath primary `
  -ExpectedDecisionReason applied
python -B tools\report-active-fx-roi-ab.py `
  artifacts\performance\active-fx-roi-v026-rtx4060-4k170-sdr-center-click-20260824-r2 `
  --json artifacts\performance\active-fx-roi-v026-rtx4060-4k170-sdr-center-click-20260824-r2\report.json `
  --markdown artifacts\performance\active-fx-roi-v026-rtx4060-4k170-sdr-center-click-20260824-r2\report.md
```

collector schema 2 从每份原始日志重新锁定 Hardware D3D11、RTX 4060、Adapter LUID/驱动、
`3840x2160`、`170/1 Hz`、SDR 和 `conservative-sdr`，并验证运行前后配置 SHA-256 未改变。最终
报告为 `FAIL`，因此按预注册门槛停止 0.2.6 发布流程。核心结果为：像素比例 `46.8%`、Prefilter
p95 降低 `53.5%`、Bloom/final p95 恶化 `15.2%`、FPS 下降 `7.2%`、不慢配对 `6/10`，且两臂均有
非零 `FramePacing.Timeouts`。0.2.6 未执行后续发布 workflow、SDK CI、打包或 Release。

证据索引 SHA-256：

- `capture.json`: `f0367227ee02e0a0f88a4cacd3657de701b5c043f174246102cb0fa269bdb13c`
- `report.json`: `441cb3986a36d72cac994f67559c1c6bc6d2894645df00b10881e6ae1b40cf81`
- `report.md`: `791c0c5cd9decb0990ccf25f457f76b7faed0307f99628bd25b99f0c26f5cade`

AMD、Intel、HDR、Windows 11、多显示器和跨适配器 ROI 单元格在真实执行前保持 `Not Run`，不能用
WARP、SDK 编译矩阵或 RTX 4060 SDR 结果外推。

### 5.2 Active-FX ROI v0.2.7 专用硬件晋级门

0.2.7 继续使用 5 个 ABBA 块、20 次采集、每次预热 5 秒并采样 30 秒的同机配对合同。collector Host
寿命为 45 秒；报告仍只丢弃第一个完整 10 秒窗并选择随后三个完整 10 秒窗，额外寿命只为进程初始化
保留余量，不扩大测量窗口。当前正式 collector manifest 与 report 必须均为 schema 4；同一 EXE、
输入、显示和 schema 19 配置只能切换
`performance.activeFxRoiEnabled`。原始日志必须重新锁定 Hardware D3D11、RTX 4060、Adapter LUID/驱动、
`3840x2160`、`170/1 Hz`、SDR、`conservative-sdr`、Host SHA-256、配置 SHA-256 和配对顺序。

提交 `d898a56` 规定采集完整性前置门：创建输出目录前及每个 ABBA/BAAB 块开始前，collector 使用
`GetSystemTimes` 取得 5 个一秒系统 CPU busy 样本；中位值超过 10% 时 fail-closed。边界值 10% 接受，
少数瞬时尖峰不能单独拒绝，持续多数超限必须拒绝。该门不枚举或终止具体进程，不修改 manifest/report
schema、ABBA 顺序、选窗或以下性能阈值。它只能拦截持续的粗粒度系统 CPU 负载，不证明 GPU、存储、
DPC 或单核已经空闲，因此通过前置门也不能替代块外环境检查。

当前 schema 4 硬门包括：

- `Applied/Requested >= 95%`，Prefilter 绘制比例不超过 45%，Prefilter GPU p95 至少降低 25%；
- 金字塔聚合 drawn/full 像素比例不超过 45%，Pyramid GPU p95 至少降低 25%；
- Bloom/final p95 至少降低 `max(5%, 100 us)`，10 组配对中至少 8 组 ROI 不慢于全屏；
- FPS 降幅不超过 1%，GPU command p99 恶化不超过 5%；
- GPU pending 最大值不超过 1，查询、节流、状态和其他错误计数均为 0；
- ROI-off 不得使用 dirty Present；稳态 primary ROI-on 的 dirty Present 帧数必须精确覆盖应用帧减预热帧，
  且 dirty pixels 非零；录制重建和 fallback 不得使用 dirty Present；
- 触边、面积回退及无 Spout2 的 `background-aware` 场景 100% 命中预期原因并保持 FP16 exact，性能
  门仅检查 Prefilter/Pyramid/Bloom GPU p95 与 GPU command p99，恶化不超过 `max(3%, 100 us)`。

`Cpu.FrameTotal` 与 `Cpu.PresentCall` 的 p95/p99 保留原 `5%` 参考线，但在 schema 4 report 中是
non-blocking advisories：它们包含 DXGI/帧节奏墙钟等待且彼此高度重叠，不能单独否决发布。该调整不
删除指标，也不把 GPU command、FPS、错误、pending、ROI、dirty-present 或收益门降级。

#### 历史正式证据（旧合同，不追溯改判）

以下 capture schema 2/3 与对应旧报告及其 `FAIL` 均按生成时合同保留，不用当前 schema 4 重新判定。

2026-08-25 的历史正式复验位于
`artifacts/performance/active-fx-roi-v027-rtx4060-4k170-sdr-center-click-20260825-r5`。manifest 为
schema 3 且 `captureStatus=captured`，报告为 schema 2；采集开始时间为 `2026-08-25T04:48:45.546Z`
（Asia/Shanghai 2026-08-25），revision 为 `ac4d214fa636ce5907f5f9bf73cf481adce84b16`。Host EXE
SHA-256 为 `e974b25759cb6a4727d85d77a7b5de1da751cf39514dcd409cdb0dde25106c0e`，基础配置 SHA-256 为
`4238b5055ea35ee8288b09b4b64e64fbec7d5affc428acdc00c3ba6c585cff0c`。

采集重新锁定 `NVIDIA GeForce RTX 4060 Laptop GPU`、LUID `00000000:00011C77`、驱动
`32.0.16.1088`、`3840x2160`、`170/1 Hz`、SDR 和 `conservative-sdr`。manifest 包含 5 个 ABBA 块和
20 次运行；两臂各聚合 10 次、约 30 秒的三个完整窗口，ROI off/on 累计呈现帧数分别为 `50661` 和
`50864`，无 `PowerUnavailable`、查询、节流、状态或帧 pacing 错误。此前无后缀、`-r2` 和 `-r3`
目录因显示电源不可用而 fail-closed；`-r4` 是清理省略修复前的有效历史失败基线。它们均不参与本次
正式统计，原始证据继续保留。

正式报告仍为 `FAIL`。Prefilter/金字塔绘制比例、Prefilter/Pyramid/Bloom-final p95、FPS、配对、
pending、GPU command p99 和错误计数门均通过；只剩 CPU frame p95/p99 与 Present p95/p99 恶化门
失败。Prefilter 绘制比例为 `0.99%`、p95 降低 `87.9%`；金字塔绘制比例为 `13.8%`、Pyramid p95
降低 `55.4%`；Bloom/final p95 从 `1760.0 us` 降至 `692.5 us`，降低 `60.7%`；`9/10` 配对不慢，
FPS 提升 `0.404%`，GPU command p99 从 `2825.0 us` 降至 `2372.0 us`，改善 `16.0%`。CPU frame
p95/p99 从 `1012.0/2240.0 us` 增至 `1171.5/2422.0 us`，恶化 `15.8%/8.1%`；Present p95/p99
从 `964.0/2189.5 us` 增至 `1099.0/2358.5 us`，恶化 `14.0%/7.7%`。

逐轮复核复用正式报告器的 `_load_events`、`_intervals` 和 `_run_metrics` 解析每轮三个采样窗。Present
p95 在 `7/10` 相邻对变慢，配对差中位为 `+298.5 us`；p99 在 `8/10` 对变慢，中位为
`+284.5 us`，而 p50 配对差中位仅 `-1.5 us`。Frame 与 Present 的逐轮 p95/p99 相关系数分别为
`0.9999/1.0000`，两者尾差中位仅 `58 us`，说明 CPU frame 失败主要由 Present 主导。ROI 的
Fx/Bloom submit p95 每对都增加，中位分别为 `24.5/27.5 us`；同时 A->B 与 B->A 以及运行序号呈现
明显顺序漂移。正式 r5 没有 NVIDIA 遥测，因此不能从该组日志单独证明产品原因，门槛仍按失败处理。

后续非发布因果矩阵位于
`artifacts/performance/active-fx-roi-v027-causal-timing-diagnostic-20260825-r1`。capture schema 2、诊断
report 已用提交 `015a10e` 的 schema 4 报告器重放，原 schema 3 文件继续保留。capture revision 为
`52a464c14d14a086e318ca9777b718e11581640e`，Host EXE SHA-256 为
`448c14c308bdcdf3b2a6cfaba7465ffe8d041266fc62f4e590d5972084819b26`；RTX 4060、4K 170 Hz、SDR 的
`ABBA+BAAB` 共 8 次运行全部完成。`Cpu.PrePresent` p95/p99 两臂均为 `0/1 us`；
`FramePacing.Wait` p95/p99 为 `5408/5683.5 us -> 5495/5754 us`，两臂 `>=8000 us` 均为 0，FPS
都约为 170。该诊断未观察到支持 Present 前 ROI 诊断、GPU query、Spout/readback 收尾或长等待失稳
是这组差异主因的证据。

只统计三个实际 Performance.Interval UTC 窗口后，ROI off/on 分别纳入 `566/564` 个 NVIDIA 样本。
SM 时钟的四 run 中位数为 `750 -> 502.5 MHz`，显存时钟均为 `810 MHz`，瞬时功耗 run-mean 中位数
为 `11.474 -> 10.567 W`；P-state 窗口中位数为 off `P5 100%`，on `P5 98.582% / P8 1.418%`。
同时 FinalComposite p95 为 `1431 -> 1918.5 us`，GPU command p99 为 `2783 -> 3723 us`，Present p95
为 `3091 -> 3311.5 us`。这些 arm 级聚合值同时出现，r1 单独不能确定关联方向或因果。NVIDIA 数据按
200 ms 采样并描述设备整体状态，不能归因到单帧、单进程或 BAFX 独占能耗；该 r1 也早于缓存提交。
由于正式 r5 未采集显卡遥测，不能把 r1 的状态观测追溯宣称为 r5 的已证实原因。诊断矩阵不能替代
正式 20-run 门禁，也不改变 schema 19、`GetDisplayState` schema 4 或当前失败结论。

提交 `d9ef2fe` 增加一项低风险产品修正：只缓存规划器生成且已逐字段验证的 ROI pass plan，命中时避免
重复执行完整规划器，但仍核对 monitor、完整 Bloom plan 和实际消费字段，miss 重新规划；resize、
diffusion/intensity 更新和显式重建会清空缓存，Context1、资源状态、共享写入所有权及自适应门仍逐帧
检查。Release 构建和 `gpu_pipeline_warp` 通过，包含篡改候选的全屏 fallback 与 FP16 bit-exact；
这些结果只证明正确性，不证明真实 CPU 或整机收益。

缓存后的非发布复测位于
`artifacts/performance/active-fx-roi-v027-plan-cache-diagnostic-20260825-r2`。此前 `-r1` 在第一个 run
因当前附着的 `DISPLAY20=3840x2160@144/1 Hz` 与合同不符而 fail-closed，manifest 为
`captureStatus=diagnostic-failed` 且 `runs=[]`。恢复 NVIDIA `DISPLAY1@170/1 Hz` 后，r2 的 capture
schema 2、revision 为 `c78376d6cbd0e848ba38337785b029bcb07049cb`，Host EXE SHA-256 为
`670af52d7c56a8d96529672bb462855a2cb2501d803a7d1e2c4ff63ee7c105b2`，RTX 4060、4K 170 Hz、SDR 的
`ABBA+BAAB` 8 次运行全部完成。提交 `015a10e` 的诊断 report schema 4 新增每次 run 中三个实际
Performance.Interval 窗口的 CPU submit 严格汇总；正式 collector/report schema 3/2、配置 schema 19
和 `GetDisplayState` schema 4 均未改变。

r2 的 ROI off/on `Cpu.FxTotalSubmit` p95/p99 为 `74/103.5 -> 67.5/98.5 us`，
`Cpu.BloomAndCompositeSubmit` 为 `30.5/42.5 -> 29/41 us`，`Cpu.FxMaterialsSubmit` 为
`40.5/59.5 -> 36.5/52 us`。该短矩阵未再观察到 ROI-on submit 尾部开销，与缓存命中时避免重复执行
完整规划器的预期一致；但缓存前 r1 与缓存后 r2 是不同时间、不同 GPU 工作状态的独立矩阵，不能把两者
的绝对值相减并宣称为缓存的因果收益。

Bloom/final p95 为 `1242.5 -> 610 us`，FinalComposite p95 为 `573.5 -> 494.5 us`，GPU command p99
为 `1849.5 -> 1519 us`；但 CPU frame p95/p99 为 `795.5/1648 -> 976.5/2165 us`，Present p95/p99
为 `747/1588.5 -> 918.5/2105 us`。CPU frame 分别恶化 `22.8%/31.4%`，Present 分别恶化
`23.0%/32.5%`，两类均超过 `5%` 门。FPS 为 `169.607 -> 169.394`，pending 最大值为 1，错误计数
为 0。r2 每臂只有 4 次，不能给出正式的 10 组配对结论，也不能替代 5 ABBA、20-run 发布门。

r2 中 ROI-on 的 SM 时钟与瞬时设备功耗仍较低，分别为 `1822.5 -> 1481.25 MHz` 和
`27.048 -> 22.199 W`，但全屏 FinalComposite 同时更快，未复现 r1 的 FinalComposite 变慢。因此 r1
只保留为一次状态相关观测，较低设备时钟/功耗本身不足以解释正式 r5 或稳定预测 FinalComposite。NVIDIA
数据按 200 ms 采样并描述设备整体状态，不能归因到单帧、单进程或 BAFX 独占能耗；r1 的
`P5/810 MHz` 与 r2 的 `P0/8001 MHz` 也禁止跨矩阵比较绝对耗时。

缓存后的正式复验位于
`artifacts/performance/active-fx-roi-v027-rtx4060-4k170-sdr-center-click-20260828-r1`。capture schema 3、
report schema 2，采集开始时间为 `2026-08-27T16:45:00.567Z`（Asia/Shanghai 2026-08-28），revision
为 `ef8bf97e3c7861bec19ad2a48c7e8f3369de2e24`。Host EXE SHA-256 仍为
`670af52d7c56a8d96529672bb462855a2cb2501d803a7d1e2c4ff63ee7c105b2`，基础配置 SHA-256 仍为
`4238b5055ea35ee8288b09b4b64e64fbec7d5affc428acdc00c3ba6c585cff0c`；环境重新锁定同一 RTX 4060、
驱动 `32.0.16.1088`、4K `170/1 Hz`、SDR 和 `conservative-sdr`，20 次运行及错误计数合同全部有效。

正式报告仍为 `FAIL`。Prefilter/金字塔 drawn/full 比例为 `0.96%/13.68%`，GPU p95 分别降低
`82.95%/42.82%`；Bloom/final p95 从 `1942.5 us` 降至 `1690.5 us`，降低 `12.97%`，`10/10`
相邻配对不慢，FPS 只下降 `0.0016%`。CPU frame p95/p99 从 `2549/3541 us` 增至
`2889/3838.5 us`，恶化 `13.34%/8.40%`；Present p95/p99 从 `2509.5/3504 us` 增至
`2853/3798.5 us`，恶化 `13.69%/8.40%`；GPU command p99 从 `2745.5 us` 增至 `3681 us`，
恶化 `34.07%`。后五项均超过预注册 `5%` 门，故不执行发布 workflow、SDK CI、打包、tag 或 Release。

逐轮重放显示 ROI-on 的 `Cpu.FxTotalSubmit` 与 `Cpu.BloomAndCompositeSubmit` p95 配对差中位仅为
`+3/+2 us`；CPU frame 与 Present p95 在 `8/10` 对变慢，配对差中位为 `+190/+190.5 us`。
GPU Bloom/final p95 和 RenderCommandSpan p95 在 `10/10` 对变快，但 RenderCommandSpan p99 也在
`10/10` 对变慢，配对差中位为 `+938.5 us`，尾差集中于全屏 FinalComposite 阶段。

同 revision 的非发布状态诊断位于
`artifacts/performance/active-fx-roi-v027-post-cache-pstate-diagnostic-20260828-r1`。capture schema 2、
诊断 report 已用提交 `625f0e1` 的 schema 5 报告器重放；`ABBA+BAAB` 8 次运行及三个实际采样窗的
NVIDIA 遥测均通过严格校验。schema 5 将每个区块两组相邻跨臂运行统一归一为 `ROI on - ROI off`，
同时保留原始采集顺序；这些是独立 run 汇总差，不是同帧因果测量。
ROI off/on 的 SM 时钟中位为 `1417.5 -> 1200 MHz`，显存时钟均为 `8001 MHz`，设备瞬时功耗
run-mean 中位为 `23.851 -> 20.824 W`；GPU command p99 为 `1299.5 -> 2093 us`，CPU frame p95
为 `450.5 -> 600 us`，Present p95 为 `379.5 -> 530.5 us`。GPU command p99 配对中位差为 `+431.5 us`、
`2/4` 组 ROI-on 不慢；CPU frame/Present p95 配对中位差为 `+149.5/+151 us`，两者均只有
`1/4` 组 ROI-on 不慢。SM 时钟与瞬时功耗 run-mean 的配对中位差为 `-82.5 MHz/-2.138 W`，
每个区块首轮的 CPU/Present 尖峰又分别落在 off 与 on。该短矩阵只能确认
低负载设备状态、运行顺序与尾延迟同时变化，不能确定关联方向或单帧因果，也不能替代正式失败结论。

提交 `866dea5` 针对上述全屏 FinalComposite 尾差，只在纯特效 ROI 实际应用且 pass plan 的
`resolveRect` 已逐字段验证时，先把一个或两个最终 RTV 全屏清为透明，再把最终传输/合成 shader
scissor 到该矩形；背景感知和任一 fallback 继续全屏。Release 构建和完整 `gpu_pipeline_warp` 55 项
通过，包含最终输出 FP16 逐元素 bit-exact、双 RTV、移动/不相交 ROI、resize、Context1 缺失和
`ClearView=false`。提交 `f00226f` 仅把这组 WARP CTest 超时从 30 秒调整为 90 秒；直接执行耗时约
35--48 秒且 55 项全部通过，没有放宽任何像素判定。

首个正式采集目录
`artifacts/performance/active-fx-roi-v027-final-scissor-rtx4060-4k170-sdr-center-click-20260828-r1`
使用 40.5 秒 Host 寿命，run 1 只形成 3 个完整 Performance.Interval，因合同要求 4 个完整窗口而
fail-closed，未产生任何有效 run。提交 `a86ce5e` 把 Host 寿命增加到 45 秒后，30 秒选窗、ABBA 顺序、
输入场景、配置差异和所有门槛保持不变。

以下两轮均存在采集前已启动、采集后仍运行的外部 WinRAR 工作负载；这里只记录报告输出，不用它们
判断最终合成裁剪候选的实机收益或回归。

最终合成裁剪候选的 8-run 非发布诊断位于
`artifacts/performance/active-fx-roi-v027-final-scissor-diagnostic-20260828-r1`。其聚合 Bloom/final p95
为 `4073.5 -> 1693.5 us`，FinalComposite p95 为 `3466.5 -> 1306 us`；CPU frame p95 为
`2486.5 -> 2680.5 us`，Present p95 为 `2352.5 -> 2542 us`。相邻配对中位差均显示主要 GPU、CPU 和
Present 指标变快，但短矩阵每臂只有 4 次，本来就不能替代发布门。

调整寿命后的 20-run 正式目录为
`artifacts/performance/active-fx-roi-v027-final-scissor-rtx4060-4k170-sdr-center-click-20260828-r2`，严格
报告仍为 `FAIL`：Prefilter/Pyramid p95 分别降低 `93.04%/69.24%`，但 Bloom/final p95 从
`3045 -> 3091.5 us`，只有 `5/10` 配对不慢；CPU frame p95、Present p95 和 GPU command p99 也超过
`5%` 门。两臂 FPS 只有 `152.890/158.160`，并分别记录 `3/4` 个 `FramePacing.Timeouts`。

同一 WinRAR 进程从本地时间 2026-08-28 16:28 起创建 `General.rar`，早于两轮采集启动并在采集结束后
仍在运行。结束后的观测为约 23--24 GiB working set、约 26--28 GiB private memory，并在 3 秒采样中
使用约 25 CPU 秒。这确认存在未受控并发负载，但不证明该进程的逐帧影响，也不证明候选的产品因果。
本次未终止该用户进程；该 revision 当时没有执行发布 workflow、SDK CI、打包、tag 或 Release，后续
证据必须使用新目录，不能覆盖这两轮历史结果。

2026-08-29 的独立干净正式证据位于
`artifacts/performance/active-fx-roi-v027-final-scissor-clean-rtx4060-4k170-sdr-center-click-20260829-r1`。
capture schema 3、report schema 2，revision 为 `11fba7243faf4dee1eb18a7d740c3c3b6f7a1479`；环境为同一
RTX 4060、4K `170/1 Hz`、SDR。Bloom/final p95 为 `1671 -> 822 us`，GPU command p99 为
`2085 -> 1075 us`，FPS 为 `170.043 -> 170.036`，`8/10` 相邻配对不慢，pending 最大值为 `1`，错误
计数为零。CPU FrameTotal p95/p99 为 `432/585.5 -> 1210/1657 us`，PresentCall p95/p99 为
`382/540 -> 1162/1612.5 us`，因此旧报告按当时合同保持 `FAIL`。原始窗口的解释性复核显示每帧
`FrameTotal + FramePacing.Wait` 为 `5801.52 -> 5802.70 us`，与 FPS 基本不变一致；固定年龄场景明确
禁用 Raw Input，故该复核不能证明输入延迟无回归。

证据索引 SHA-256：

- `capture.json`: `ad756c2f3eb26a38d90a69ee6c448a5c088546ed147de5ef1f6721e97d5937a2`
- `summary.json`: `616032aa17612233ea7d6a9d2121b83bcdab5d07898f41c2b0664fb629ee8ce3`
- `summary.md`: `c3cfc34639ae654f497cae12d5d494d4fd7bba4f400839fcd705e1a748f841e7`
- 因果诊断 `capture.json`: `7dd35433a49cc7d97b8143c3760bd1eb3a9a12a6c72071a907b98426e470ece3`
- 因果诊断 `diagnostic-summary-schema3.json`:
  `2ac598386498894342b00b21f435daeaf3dc98a81d8e5fbd888b392ada0c07c3`
- 因果诊断 `diagnostic-summary-schema3.md`:
  `79b6d2c48f6f1e4d99a2ee75a0fe5735d38fe107355562aa125bacba5085156e`
- 因果诊断 `diagnostic-summary-schema4.json`:
  `60e88aafc66de991ba0874bcfbc8f54755d04ec9cb1afb7a7a67b386bcdb1c99`
- 因果诊断 `diagnostic-summary-schema4.md`:
  `0d15171808508c013797cbf24193718830a7ee94a3803dc8dacb7ed0b38f3c05`
- 缓存复测失败 manifest `capture.json`:
  `0ce9a25be6e6aa2cc3a97c7bc84863d1bc27e4b6422cf071bd36d2c331ef1767`
- 缓存复测失败 run-01 Host 日志 `ba-click-fx-desktop-support.log`:
  `28265930044e5af25e9bdc6a9d220c8fa635e11ed595c4054647c4c90cbc7b60`
- 缓存复测 r2 `capture.json`:
  `6b9c5d42c6ec26fa4caa8f9e0a0335467554b515f3e413995c9686e98830495c`
- 缓存复测 r2 `diagnostic-summary-schema4.json`:
  `0569ffb41cb646bb4d465b0fe312ca1a4f78ebad289522410fbcb9c31d715275`
- 缓存复测 r2 `diagnostic-summary-schema4.md`:
  `a765134c17476022c19086143622239ce89158fe49b6ac2449986a7ea524a637`
- 缓存后正式复验 `capture.json`:
  `c8954e36c2f8d3c4f7918803b8c43f71e51adddf2d476eafc1558da558894c64`
- 缓存后正式复验 `summary.json`:
  `7398b779be87a46f6325099d37002ca517664d8e4c74f279652620928a1c998e`
- 缓存后正式复验 `summary.md`:
  `1de0bb94ea13609e73b7c15835f1dd6d46e84e19ede53cb3d7f0a9422e9ddf28`
- 缓存后状态诊断 `capture.json`:
  `3c4721025eda737aecef8b20966eca2bd31e0a825836ab9b251791a442d4ac72`
- 缓存后状态诊断 `diagnostic-summary.json`:
  `17f9abbc030a487e67cf06acf38191bec22eb635486118552fa31b82cfeda8d4`
- 缓存后状态诊断 `diagnostic-summary.md`:
  `20b9a82c8bbcef2b2ee5af1504f4a6fd979194060a06ddb68fa42e3c594a74a1`
- 最终合成裁剪短诊断 `capture.json`:
  `bb75612a6f1250dea1ca7119187a7b344dbe1c6b9fef097b475ed39770949164`
- 最终合成裁剪短诊断 `diagnostic-summary.json`:
  `1bd2e55c0510b8b455acecfd4d9e957fc89ec7fb2a47e55520ed006a122bd3f7`
- 最终合成裁剪短诊断 `diagnostic-summary.md`:
  `678b87d7b166b0b97a4d4f5a3e7e77a025ff74c16f03dde92503538e5df71dc6`
- 最终合成裁剪首次 fail-closed `capture.json`:
  `a80b17a4e1f75a5978396cb12999bd94104490fd5abe57f4501c402426652ca5`
- 最终合成裁剪正式 r2 `capture.json`:
  `accc0d8a258e5ae377680a4ff83d17c0af29633a7ad8da88ed8877416608ff84`
- 最终合成裁剪正式 r2 `summary.json`:
  `5ec4bc38749bf04dbbf1d79d7c7a7c5f50741b8590bb4c0016896bab41f1cdc4`
- 最终合成裁剪正式 r2 `summary.md`:
  `6afcb1cb80604a3cdb51d05e1a48c606bfcad30bd9bcf67919791b42861bcaa6`
- 最终合成裁剪干净正式 r1 `capture.json`:
  `cf2e4247aa72f0ba9b4ac2008fc82232df422f9235fd086e3e3d77cf24df6bc2`
- 最终合成裁剪干净正式 r1 `summary.json`:
  `f09b4af6a74d0b429e305b17406f9358917cb08f2010a013bdc6883f6064dc05`
- 最终合成裁剪干净正式 r1 `summary.md`:
  `33eb3964048058e60acfef87bf055a53cd68645bad9b517fbf71f94ada20a661`

以上缓存后正式 20-run 与受污染 r2 分别保持其原始 `FAIL`，旧 schema 2/3 report 不追溯改判。

#### 当前发布与产品状态（2026-09-01）

0.2.7 已接入局部最终输出与 DXGI dirty `Present1`，正式采集/报告合同为 schema 4/4；
Full `release-verify`、WARP 局部输出合同与 CTest `45/45` 已通过，Slim `slim-release-verify` 为
`44/44`。新的兼容正式 20-run 硬件证据仍为
`Not Run`：首次短诊断
`artifacts/performance/active-fx-roi-v027-dirty-present-diagnostic-20260831-r1` 在 run 1 读取到实际
`2560x1440 @ 165.003 Hz`，不满足预注册 `3840x2160 @ 170/1 Hz`，因此 fail-closed 且未形成有效 run。

ROI 从 0.2.7 起保持默认关闭和实验性，后续版本也不改变该默认值，不据现有结果声明整机性能、功耗或
输入延迟收益。上述硬件晋级 `Not Run` 只阻塞这些声明以及独立的 Differential Bloom ROI 实验扩展，
不阻塞不改变 ROI 默认值的普通版本；普通发布仍须通过本节通用 release candidate 门槛。

### 5.3 v0.2.8 普通体验版发布结果

0.2.8 不修改配置 schema、ROI 渲染路径、默认值或支持声明。2026-09-01 在重新配置并构建 0.2.8
二进制后，本地 Full `release-verify` 通过 `44/44`，总测试时间 `118.73 s`；Slim
`slim-release-verify` 通过 `43/43`，总测试时间 `71.85 s`。Full/Slim 均编译 Host、Control Center 和
Identity Signer，Slim 不生成发布资产。

非发布 Active-FX ROI 诊断报告测试继续保留，但默认不注册；需要时显式配置
`BAFX_ENABLE_ROI_DIAGNOSTIC_TESTS=ON`。4K 170 Hz schema 4 晋级保持 `Not Run`，不参与本节普通体验版
发布门。Full 四资产已在 `artifacts/release-0.2.8-candidate-20260901-r1` 生成并通过本地校验：便携 ZIP
SHA-256 为 `98489A0E1456779904DAA52C1AEEE10D982637F5112D0D06C057B31B81824E37`，安装器 SHA-256 为
`60D403A087D9C3F295EEDCB38B38935F52764EA8292BC2B83017EE3E3B495C59`，两个 `.sha256` 文件均与复算
结果一致。通知区域菜单的完整人工操作验收和 Windows SDK `10.0.19041.0`、`10.0.22621.0`、
`10.0.26100.0` CI 均已通过。正式版本使用 annotated tag，只上传上述 Full 四资产，并在发布后按相同
文件名回下载复核哈希；Slim 不生成或上传发布资产。

### 5.4 v0.2.9 OBS/Spout2 回归修复发布结果

0.2.9 只修复 FX-only 线性能量到 BGRA8 的输出映射，不改变配置 schema、ROI 默认值、
帧率策略、空闲资源优化、WGC 或 Spout2 SDK 生命周期。2026-09-04 在重新配置并构建 0.2.9
二进制后，本地 Full `release-verify` 通过 `44/44`，总测试时间 `102.19 s`；Slim
`slim-release-verify` 通过 `43/43`，总测试时间 `80.44 s`。

OBS `32.2.2` 与 win-spout `1.12.0` 下的隔离 `FixedComposite`/`DynamicLifecycle` 证据均通过，
同时保留其他 OBS/插件版本、HDR、多显示器、跨 GPU 和 Windows 11 为 `Not Run`。Full 四资产
已在 `artifacts/release-0.2.9-candidate-20260904-r1` 生成并通过本地校验：便携 ZIP SHA-256 为
`FF5C82EEF2B6CD25BEC1543FBAFB2F92E3EC7C4AD78D5DE79EC2425EF79D2196`，安装器 SHA-256 为
`81F7FF7CB37D521183C0DCC43CC5FE40840562C77F4008BD6795A732D43E40B2`，两个 sidecar 均与复算结果一致。
Windows SDK `10.0.19041.0`、`10.0.22621.0`、`10.0.26100.0` 三档 CI 均已通过。正式版本
使用 annotated tag，只上传 Full 四资产，并在发布后按相同文件名回下载复核哈希；Slim 只用于
源码验证。

### 5.5 v0.2.10 全局快捷键发布结果

0.2.10 将主配置升级到 schema 20，并增加原生全局快捷键控制。发布候选必须通过配置/IPC/HostState、
Host 事务、Win32 进程边界、支持报告和 Control Center 命令测试；人工验收至少覆盖四项动作、
`MOD_NOREPEAT` 长按、重复/占用组合、录制取消/失焦/30 秒超时、断线、重试、关闭草稿选择，以及重置默认
保留已保存快捷键。任何配置已写入后的激活或清理异常都必须显示重启指引。

2026-09-04 在干净提交 `a90ffc0` 上重新配置并构建候选后，Full `release-verify` 通过 `45/45`，
总测试时间 `104.08 s`；Slim `slim-release-verify` 通过 `44/44`，总测试时间 `110.87 s`。首次 Full
运行准确发现 ROI collector 源码合同测试仍要求固定 schema 19；`a90ffc0` 将其改为验证 19/20 支持集合和
base config 的实际 schema，聚焦测试通过后完整 Full workflow 从头重跑通过。

用户已确认快捷键设置和使用正常；反馈的快捷键页控件拥挤来自状态文本与操作按钮几何重叠，修复后在
144 DPI 的连接与断开状态下均完成整窗捕获检查，最终 Portable 候选复核也没有控件或文本重叠。Full 四资产已在
`artifacts/release-0.2.10-candidate-20260904-r1` 生成并通过脚本与独立 SHA-256 复算：便携 ZIP 为
`F20812B47FCF91E6BCA56D8D1D24A9F625B69C96F6863B040B2892C0CB2F694B`，安装器为
`B3AA6E2AEF61EC315F2603778FC6E9C54040AC6CF8216147A7014613D5F354E0`，两个 sidecar 均与复算结果一致。

Windows SDK `10.0.19041.0`、`10.0.22621.0`、`10.0.26100.0` 三档 CI 均已通过。正式版本使用
annotated tag，只上传 Full 便携 ZIP、ZIP 哈希、安装器和安装器哈希四个资产，并在发布后按相同文件名
回下载复核哈希；Slim 仅做源码构建与本地验证，不生成或上传预编译资产。
本节不改变 0.2.6/0.2.7 的 schema 19 ROI 历史证据或结论。

### 5.6 v0.2.11 项目入口与 Unity 参考同步发布门

0.2.11 的产品改动限于 Control Center 的固定项目仓库入口和 Star 提示；配置 schema、渲染参数与运行时
绘制逻辑均不改变。Unity 重建工程更新后的 3 个材质、3 个 Touch Shader、审计文档和 9 张基线图已同步
到 `reference/unity-reference.json`。`verify-unity-reference.ps1` 必须对 62 个文件和 2 棵资源树通过，
并保留游戏 Unity `2021.3.56f2` 与重建工程 Unity `2021.3.45f1` 的版本边界。

发布候选必须在干净提交上通过 Full `release-verify` 与 Slim `slim-release-verify`。Full 构建随后使用
`-SkipBuild` 生成便携 ZIP、ZIP 哈希、安装器和安装器哈希；Slim 只验证源码构建，不生成或上传正式资产。
推送 `main` 后，Windows SDK `10.0.19041.0`、`10.0.22621.0`、`10.0.26100.0` 三档 CI 必须在同一提交
全绿，才能创建 annotated tag `v0.2.11` 和正式 GitHub Release。发布后按发布文件名回下载四个资产，
逐项复核 SHA-256、tag 指向、Release 状态和资产数量。

2026-09-04 在干净提交 `a80293e` 上完成候选构建。Full `release-verify` 通过 `45/45`，总测试时间
`114.64 s`；Slim `slim-release-verify` 通过 `44/44`，总测试时间 `92.17 s`。同一 Full 候选中的 Host、
Control Center 和安装器均报告产品版本 `0.2.11`。Portable 系统页在 144 DPI 下完成整窗捕获，固定仓库
按钮保持启用，Star 提示与按钮边界不相交；固定 URL 与唯一集中式 Shell 导航路径由更新检查和安装器合同
测试覆盖。

Full 四资产已在 `artifacts/release-0.2.11-candidate-20260904-r1` 生成。便携 ZIP SHA-256 为
`DA08A38B1C73B8B18A30B99E1619A6FB59AD57D127336A0F5C86F35F2673A0D3`，安装器 SHA-256 为
`9241FF8C01718FFBB1CA7301C08C8050FF1D93E8BA76870775FFC05F5BF76E01`；两个 sidecar 均与独立复算一致。
Windows SDK `10.0.19041.0`、`10.0.22621.0`、`10.0.26100.0` 三档 CI 均已通过。正式版本使用
annotated tag，只上传上述 Full 四资产，并在发布后按相同文件名回下载复核哈希。

## 6. 需求追踪

| 合同 | ADR | Spike | Validation suite |
| --- | --- | --- | --- |
| Final composition | ADR-001 | SPK-001, SPK-003 | VAL-COMP, VAL-COLOR |
| Intensity/output mapping | ADR-002 | SPK-003 | VAL-COLOR |
| Sensor/lifecycle/power | ADR-003 | SPK-002, SPK-004 | VAL-CAPTURE, VAL-SOAK |
| Self-exclusion/recording | ADR-004 | SPK-002 | VAL-RECORDING |
| Golden/numerics | ADR-005 | all | all suites |
| ROI/mip phase | ADR-006 | opt-in full Bloom pyramid ROI + verified dirty Present + per-frame full-screen fallback | VAL-ROI |
| Temporal validity | ADR-007 | SPK-002, SPK-004 | VAL-TEMPORAL |
| Host 产品控制面（effects-only Profile / 全局快捷键） | ADR-008 | 不适用 | config/fx-profile-store/hotkey-process/host-control/host-state/runtime-diagnostics tests |

比较类型固定为：整数/状态机 exact；确定性 CPU simulation exact；FP32 abs/rel epsilon；FP16 GPU
max/mean/p99.9 error；最终视觉使用感知指标加人工评审。具体阈值必须在首次执行前单独提交，失败后不得
通过放宽断言来掩盖回归。
