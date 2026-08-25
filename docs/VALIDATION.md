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

这些用例证明 0.2.7 已接入的纯特效 prefilter/down/up 和 resolve 掩码正确性，不证明真实 GPU 性能。
最终场景合成、WGC、Spout2 格式转换、交换链、Present 和默认 `background-aware` Differential Bloom
仍为全屏，不能从 WARP 结果外推。

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

### 5.2 Active-FX ROI v0.2.7 专用发布门

0.2.7 继续使用 5 个 ABBA 块、20 次采集、每次预热 5 秒并采样 30 秒的同机配对合同。collector
manifest 必须是 schema 3，报告必须是 schema 2；同一 EXE、输入、显示和 schema 19 配置只能切换
`performance.activeFxRoiEnabled`。原始日志必须重新锁定 Hardware D3D11、RTX 4060、Adapter LUID/驱动、
`3840x2160`、`170/1 Hz`、SDR、`conservative-sdr`、Host SHA-256、配置 SHA-256 和配对顺序。

除 0.2.6 已预注册的全部门槛外，0.2.7 还必须同时满足：

- `Applied/Requested >= 95%`，Prefilter 绘制比例不超过 45%，Prefilter GPU p95 至少降低 25%；
- 金字塔聚合 drawn/full 像素比例不超过 45%，Pyramid GPU p95 至少降低 25%；
- Bloom/final p95 至少降低 `max(5%, 100 us)`，10 组配对中至少 8 组 ROI 不慢于全屏；
- FPS 降幅不超过 1%，CPU、Present 与 p99 恶化不超过 5%；
- GPU pending 最大值不超过 1，查询、节流、状态和其他错误计数均为 0；
- 触边、面积回退及无 Spout2 的 `background-aware` 场景 100% 命中预期原因并保持 FP16 exact，性能
  恶化不超过 `max(3%, 100 us)`。

2026-08-25 的最新有效正式复验位于
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
明显顺序漂移。现有日志因此既不能把尾部差异归因为一个可修复的 ROI 产品缺陷，也不能证明它只是更早
到达 Present 后的等待补偿。门槛仍按失败处理，不实施推测性修复，也不以重复采集替代原因定位。
再次开展性能诊断前，必须先分别观测主循环 `FramePacing.Wait` 和 FX render 返回到 Present 调用入口的
`Cpu.PrePresent`，再执行顺序平衡的非发布短矩阵；该诊断不能替代正式 20-run 门禁，也不改变 schema
19、`GetDisplayState` schema 4 或当前失败结论。

证据索引 SHA-256：

- `capture.json`: `ad756c2f3eb26a38d90a69ee6c448a5c088546ed147de5ef1f6721e97d5937a2`
- `summary.json`: `616032aa17612233ea7d6a9d2121b83bcdab5d07898f41c2b0664fb629ee8ce3`
- `summary.md`: `c3cfc34639ae654f497cae12d5d494d4fd7bba4f400839fcd705e1a748f841e7`

因此阈值不放宽，0.2.7 不发布，也不执行 Full/Slim 发布 workflow、SDK `19041/22621/26100` 发布 CI、
正式打包、tag 或远端复核。官方 Release 的 Full 四资产策略保持不变，Slim 仍只保留源码构建验证。

0.2.8 的 Differential Bloom ROI 必须在 0.2.7 独立通过并交付后开始，另行增加背景有效性和捕获代次
回退门；当前保持阻塞，不得用 0.2.7 的局部 GPU 收益替代整机失败结果。

## 6. 需求追踪

| 合同 | ADR | Spike | Validation suite |
| --- | --- | --- | --- |
| Final composition | ADR-001 | SPK-001, SPK-003 | VAL-COMP, VAL-COLOR |
| Intensity/output mapping | ADR-002 | SPK-003 | VAL-COLOR |
| Sensor/lifecycle/power | ADR-003 | SPK-002, SPK-004 | VAL-CAPTURE, VAL-SOAK |
| Self-exclusion/recording | ADR-004 | SPK-002 | VAL-RECORDING |
| Golden/numerics | ADR-005 | all | all suites |
| ROI/mip phase | ADR-006 | opt-in full Bloom pyramid ROI + per-frame full-screen fallback | VAL-ROI |
| Temporal validity | ADR-007 | SPK-002, SPK-004 | VAL-TEMPORAL |
| Host effects-only Profile | ADR-008 | 不适用 | config/fx-profile-store/host-control/host-state tests |

比较类型固定为：整数/状态机 exact；确定性 CPU simulation exact；FP32 abs/rel epsilon；FP16 GPU
max/mean/p99.9 error；最终视觉使用感知指标加人工评审。具体阈值必须在首次执行前单独提交，失败后不得
通过放宽断言来掩盖回归。
