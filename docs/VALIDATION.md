# 验证策略

## 1. 证据分层

| Tier | 用途 | 可以决定什么 |
| --- | --- | --- |
| Unity/游戏证据 | 视觉真值 | 时序、形状、材质、层顺序、Bloom 观感 |
| ba-click-fx | 行为参考 | 输入语义、配置命名、兼容接口 |
| 原生中间 buffer | 实现验证 | 定位数值、滤波、合成和回归 |

低层证据不能推翻高层视觉真值。若 Web 与 Unity 不一致，先记录差异，再以 Unity/游戏证据校准。

## 2. 自动化层级

### L0：纯函数

- intensity 语义与 output policy；
- background binary validity、路径单向锁存、饱和时间差与边界；
- ROI alignment/guard；
- finite sanitize、component-wise non-negative 与 isotonic test vectors；
- fixed-step simulation 和 deterministic random；
- PointerFrameAdapter 的跨帧 held、Down→Held→Up、释放帧不移动、边沿后尾随 Move 抑制，以及同帧
  多边沿原序归约；PointerFrameDispatch 的统一帧位置、统一 `renderTime` 与 QPC 输入相位隔离。

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
- 输入消费/呈现边界上的 Down→Held→Up、释放帧不移动、常驻拖尾分流，以及 QPC 只影响可选采样相位；
- `FxTrailTimeScale` 的逐 Update parking 状态机目前只在参考层验证；桌面 Host 尚无游戏
  `Time.timeScale` 来源，`SimulationRuntime` 也尚未复现 `SyncComponentPool<FXTouch>` 的失活对象复用；
- DComp visual/swap chain resize/present；
- WGC session state machine；
- monitor/adapter rebuild。

### L3：硬件/视觉

- 四个 Spike；
- Unity Golden 时间序列；
- SDR/HDR 与混合刷新率矩阵；
- 外部录屏观察。

## 3. Golden case 契约

每个 case 固定：

- commit、shader hash、配置 schema；
- OS build、adapter LUID、driver、显示色彩模式；
- viewport、DPI、背景编码与参考白；
- QPC frequency、消息分派 QPC、统一 `renderTime`、固定模拟步长、随机种子；
- 呈现边界、锁存位置、Down/Held/Up 序列，以及拖尾常驻开关、出界、重入和边沿后尾随 Move 边界。

显式种子的 C++ 模拟坐标只属于实现内确定性回归，不等同于 Unity Golden。Unity 为每个
ParticleSystem 使用独立的引擎随机流；在尚未导出初始粒子状态 fixture 或复现该随机流前，跨实现
比较必须使用与随机布局无关的数量、包络、径向能量和感知指标，禁止把 C++ 粒子坐标称为 Unity 像素真值。

导出以下命名层：

```text
Coverage
DirectEmission
BloomSeed
DifferentialPrefilter
BloomMip0..N
BloomResult
FinalOverlay
```

Tri2 三角碎片应同时出现在 Coverage/DirectEmission/BloomSeed：它使用游戏
`FX_SHADER_Additive_0`，在 UI HDR 缓冲绘制后进入全场景 Bloom。对 FP16 使用每通道绝对/相对容差，并在最终图上补充感知误差；
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
该诊断验证距离发射、两点 Trail 几何/材质与 Bloom，不冒充真实逐帧 TrailRenderer 采样时序验证。
它也不验证 OS 消息到 Unity Legacy Input 帧态的聚合。原生同帧多边沿无损扩展已有确定性测试，但 Unity
如何聚合一个渲染帧内的多次按下/释放仍为 Not Verified；需由真实 Player 黑盒夹具同时记录
`GetMouseButtonDown`、`GetMouseButton`、`GetMouseButtonUp` 和 `Input.mousePosition` 后才能声明 parity。
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
