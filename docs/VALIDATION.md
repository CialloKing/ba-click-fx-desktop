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
- fixed-step simulation 和 deterministic random。

### L1：GPU 离屏

- shader compile/reflection；
- MaterialOutputs MRT；
- DifferentialPrefilter FP32 差值；
- 浅色/深色背景下点击与拖尾跨获取/保留边界及恢复序列的逐像素稳定性；
- 近白到纯白背景的连续 FP16 采样步进，确认背景变化不会在点击或拖尾上产生透明/不透明跳变；
- Bloom mip/down/up；
- FinalOverlay FP16 readback。

### L2：窗口与 API 集成

- PMv2 坐标、Raw Input、click-through overlay；
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
- QPC frequency、事件时间、固定模拟步长、随机种子；
- 输入位置/按下/移动/释放序列。

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

三角碎片只能出现在 Coverage/DirectEmission，BloomSeed 必须为零。对 FP16 使用每通道绝对/相对容差，
并在最终图上补充感知误差；
不得用 PNG hash 代替数值比较。

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
`DirectSurface`、`BloomSeed`、全部 Down/Up mip。`.rgba16f` 是顶部原点、little-endian RGBA
half 数值证据；PNG 不执行 unpremultiply、强制不透明黑底，仅用于与 Unity PNG 观察和感知比较。
指标门禁必须同时通过十个时间片及 FP16 分层检查；失败后先解释实现或参考证据，不得放宽阈值。

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
