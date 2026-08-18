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
- ROI alignment/guard；
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

### L1：GPU 离屏

- shader compile/reflection；
- MaterialOutputs MRT；
- DifferentialPrefilter FP32 差值；
- 浅色/深色背景下点击与拖尾跨获取/保留边界及恢复序列的逐像素稳定性；
- 近白到纯白背景的连续 FP16 采样步进，确认背景变化不会在点击或拖尾上产生透明/不透明跳变；
- Bloom mip/down/up；
- FinalOverlay FP16 readback。

### L2：窗口与 API 集成

- PMv2 坐标、Raw Input 到单一帧边界当前位置的映射、click-through overlay；
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
- `GetDisplayState` schema 2 必须拒绝旧版本以及未知、重复和缺失字段，并能解析 Host 真实生成的完整会话；
  离线 override 只在全局拓扑完整时具有权威性。Windows SDK 19041/26100 Actions 均构建 Host、
  Control Center 和 Identity Signer 的完整目标，不以当前运行系统缺少 Windows 11 API 为测试失败条件；
- monitor/adapter rebuild。

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
这只证明旧系统能启动 collector 并审计“不支持”，不构成 Session-local 能力通过；支持矩阵和生产门禁
仍保持 `Not Run`。外部录屏/OBS、HDR、多显示器、真实 device lost 和 packaged 权限矩阵也保持
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
build\alpha-x64\src\capture\Release\ba-click-fx-gpu-capture.exe `
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
cmake --build --preset alpha-release --target ba_fx_gpu_capture
$revision = git rev-parse HEAD
build\alpha-x64\src\capture\Release\ba-click-fx-gpu-capture.exe `
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
build\alpha-x64\src\capture\Release\ba-click-fx-gpu-capture.exe `
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

## 6. 需求追踪

| 合同 | ADR | Spike | Validation suite |
| --- | --- | --- | --- |
| Final composition | ADR-001 | SPK-001, SPK-003 | VAL-COMP, VAL-COLOR |
| Intensity/output mapping | ADR-002 | SPK-003 | VAL-COLOR |
| Sensor/lifecycle/power | ADR-003 | SPK-002, SPK-004 | VAL-CAPTURE, VAL-SOAK |
| Self-exclusion/recording | ADR-004 | SPK-002 | VAL-RECORDING |
| Golden/numerics | ADR-005 | all | all suites |
| ROI/mip phase | ADR-006 | full-screen fallback | VAL-ROI |
| Temporal validity | ADR-007 | SPK-002, SPK-004 | VAL-TEMPORAL |

比较类型固定为：整数/状态机 exact；确定性 CPU simulation exact；FP32 abs/rel epsilon；FP16 GPU
max/mean/p99.9 error；最终视觉使用感知指标加人工评审。具体阈值必须在首次执行前单独提交，失败后不得
通过放宽断言来掩盖回归。
