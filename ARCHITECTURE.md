# ba-click-fx-desktop Architecture v0.3

- Status: **Proposed**
- Platform: Windows desktop
- Language/API: C++20, Win32, D3D11, HLSL, DirectComposition

## 1. 目标

项目提供低延迟、跨显示器、可在 SDR/HDR 环境降级运行的原生桌面点击特效。基础效果必须在
桌面捕获不可用、被拒绝或主动关闭时继续工作。视觉校准以提取的 Unity/游戏证据为真值，
不会把 Web 版本像素或 Unity 艺术参数直接解释为物理显示亮度。

首个版本的工程目标如下：

1. 输入接收、模拟、材质求值、Bloom、合成和呈现具有明确边界。
2. 每个 DXGI Adapter 内部拥有独立 D3D11 资源域。
3. WGC 仅提供带时间戳的背景样本；渲染器能够拒绝过期样本。
4. 透明输出在数值上保留 `RGB > Alpha` 的扩展预乘语义。
5. 任何能力失败都沿明确的降级路径退回 FX-only，而不是停止点击反馈。
6. Host 是唯一的运行时配置写入者；控制客户端通过本地 IPC 提交经过校验的命令。

## 2. 非目标

- 不复用现有 JavaScript/WebGL/WebGPU 渲染实现。
- 不承诺在普通 SDR 的纯白背景上，仅靠非负加法光仍然清晰可见。
- 不把 `CreateFreeThreaded`、Agile WinRT 对象或 D3D11 multithread protection 当作自由并发许可。
- 不承诺 WGC 在 Windows 11 build 26100 之前能从生产端精确节流。
- 不在没有对应显示器、录屏器和多适配器证据时宣称完整支持。

## 3. 核心不变量

- `ARCH-INV-001`：桌面始终由 DWM 显示，程序不得捕获并重放完整桌面。
- `ARCH-INV-002`：Background Sensor 只提供条件输入；失效时必须退化到 FX-only。
- `ARCH-INV-003`：一个 immediate context 同一时刻只能由一个 Render Owner 使用。
- `ARCH-INV-004`：WGC frame/surface 是借用资源，复制后必须及时关闭。
- `ARCH-INV-005`：coverage 已预乘；emission/seed 必须有限、非负且不重复计能量。
- `ARCH-INV-006`：背景路径是二值合同；Differential Bloom 与最终背景反解必须同时启用或同时禁用。
- `ARCH-INV-007`：PreserveDesktop 禁止降低桌面像素；HighVisibility 明确放弃该约束。
- `ARCH-INV-008`：RecordingCompatible 是 best effort，不是通用录屏保证。
- `ARCH-INV-009`：跨 GPU 的 FP16 结果使用容差比较，不承诺 hash 或 bit-identical。

### 3.1 Render Owner

每个 Adapter Context 恰有一个 Render Owner。它串行拥有并调用：

- D3D11 immediate context；
- 交换链 resize/present；
- DirectComposition visual/commit；
- 背景纹理 checkout/copy/release；
- capture session 的 Recreate、Close、退订和 device-lost 转移。

其他线程只能提交不可变命令、代次号、事件或 CPU 数据。D3D11 deferred context 只有在单独测量后
才可引入，不能改变资源域所有权。

### 3.2 Adapter Resource Domain

Adapter LUID 是资源域键。Monitor Context、交换链、背景纹理、Bloom 金字塔和 shader resource
必须属于同一个 Adapter Context。跨适配器移动显示器时，优先重建资源；跨适配器共享只能作为
显式、可测量且可关闭的优化。

### 3.3 可选背景

`IBackgroundSensor` 是可替换输入。Null、Static 和 Test 实现与 WGC 具有相同契约。
任何无效、过期、尺寸不匹配或色彩编码未知的样本都必须被拒绝，渲染继续使用 FX-only Bloom。

### 3.4 非负背景交互

PreserveDesktop 模式只允许非负背景交互。对同一线性 scRGB 空间中的背景 `B` 和发光输入 `F`：

```text
X0 = max(B, 0)
X1 = max(B + F, 0)
D  = max(H(X1) - H(X0), 0)
E  = K(D)
```

其中 `F` 必须有限且逐分量非负；`H` 在非负正交域上满足严格偏序的 isotonic 条件；`K` 是固定、
零偏、线性且权重非负的滤波算子。两次 `H` 必须在同一个 FP32 shader invocation 中计算，差值在
写入 FP16 Bloom 金字塔前完成并进行 finite sanitize。

## 4. 顶层模块

```text
Platform / Window
  -> Input
  -> DisplayTopology
      -> AdapterContext [1..N]
          -> MonitorContext [1..N]
              -> BackgroundSensor (optional)
              -> FxSimulation
              -> MaterialEvaluation
                   Coverage | DirectEmission | BloomSeed
              -> BackgroundAwareDifferentialBloom
              -> OutputPolicy
              -> FinalComposition
              -> DirectComposition Present

Product Control Plane
  -> Versioned JSON Configuration
  -> Immutable Runtime Snapshot
  -> Named Pipe IPC (control client)
  -> Host single-instance lifetime
```

模块保持单向依赖：Platform 不理解材质；Simulation 不持有 GPU 资源；BackgroundSensor 不参与
效果模拟；FinalComposition 不决定捕获策略。

## 5. 主要数据契约

### 5.1 材质输出

```cpp
struct MaterialOutputs
{
    float4 coverage;        // premultiplied RGB + alpha
    float3 directEmission;  // finite and component-wise non-negative
    float3 bloomSeed;       // finite and component-wise non-negative
};
```

三角碎片可写 `coverage`、用于保留清晰 HDR 核心的 `directEmission`，以及进入全场景
Bloom 的 `bloomSeed`。Tri2 使用游戏的 `FX_SHADER_Additive_0`，其 HDR 输出在 UI 缓冲
绘制后统一进入 `MXFinalBloom`；清晰直接能量与模糊光晕是同一材质的两个输出，不应互相替代。

### 5.2 背景样本

```cpp
struct BackgroundSample
{
    TextureHandle texture;
    int64_t qpcTimestamp;
    uint64_t generation;
    PixelSize size;
    PixelFormat format;
    ColorEncoding encoding;
    bool valid;
};
```

纹理句柄必须指向 Render Owner 已复制并拥有的纹理。WGC frame/surface 不能越过 checkout 生命周期。

### 5.3 强度语义

```cpp
enum class IntensitySemantics
{
    ArtisticRelative,
    ReferenceWhiteRelative,
    AbsoluteNits
};
```

Authoring value、校准、reference-linear energy、output mapping 和 monitor encoding 是五个不同阶段。
Unity emission 值默认属于 `ArtisticRelative`；只有显式标记的值才能进入 `AbsoluteNits`。

### 5.4 产品模式

```cpp
enum class BackgroundMode
{
    BackgroundAware,
    RecordingCompatible,
    LightBackground
};

enum class EffectsMode
{
    Full,
    Core
};
```

产品模式分为正交的背景输入与特效成本两个轴：

- `BackgroundAware`：启用 WGC 并请求 `WDA_EXCLUDEFROMCAPTURE`，优先避免自反馈；外部录屏可能不包含 FX。
- `RecordingCompatible`：关闭背景感知并关闭 WDA，使用 FX-only 渲染；只提高兼容性，不保证所有录屏器可见。
- `LightBackground`：关闭 WGC，使用针对浅色背景的保守 FX-only 拟合，不把拟合结果宣称为桌面像素还原。
- `Full`：保留用户选择的背景轴、Bloom、输出与帧节奏策略。
- `Core`：保留中心圆盘、圆环、点击/拖拽碎片和拖尾，仅跳过 Bloom 与 WGC/背景捕获，并将实际运行策略锁定为
  FX-only、保守 SDR 和 60 FPS。它不修改用户保存的 `background.mode` 或 `performance.framePacing`，退出
  Core 后继续解析原请求。

## 6. 线程与帧时序

### 6.1 输入

窗口线程接收 Raw Input/Pointer 消息，记录消息分派时 QPC，并写入单生产者命令队列；另存的
`GetMessageTime` 只用于诊断 Win32 排队时间。Render Owner 在有待消费位置的呈现更新中锁存一份
帧边界绝对指针位置，按压 FX 的普通调用顺序为 Down→Held→Up；普通 Up-only 释放帧的 Held 为 false，
不提交位置移动。
本轮所有模拟动作统一使用 `renderTime`，消息分派 QPC 只推进可选 `input.samplingRateHz` 输入相位，
不能冒充模拟或呈现时间。`PointerFrameSnapshot` 按原序无损保留同帧 Raw Input 边沿，仅用于诊断和
native 扩展；严格效果路径将边沿归约为 Down/Held/Up 布尔帧态并按 Down→Held→Up 执行，Cancel 最后
作为 native 硬边界处理。Unity `2021.3.45f1` Player 黑盒已确认 `Down-Up-Down` 在聚合帧中得到
Down=true、Held=true、Up=true；其他边沿排列及游戏所用 Unity `2021.3.56f2` 仍未验证。

默认输入合同只在真实按住期间推进拖尾。用户开启“拖尾常驻”后，未按键 Move 由独立的纯拖尾实例
消费；这是 native/Web 产品增强，首个样本只建立锚点。含任一边沿的帧不得从尾随 Move 重启常驻段；
真实按下、出界、暂停或关闭相关开关也必须结束该段，避免双重拖尾或跨状态假连接，且纯拖尾实例不得
生成任何点击 burst。按住但没有新位置时不要求输入层读取光标；模拟 `advance` 只推进距离发射的静止
时间基线，不追加 Trail 顶点或推进输入采样相位。

### 6.2 WGC

`Direct3D11CaptureFramePool::CreateFreeThreaded` 的 `FrameArrived` 回调只增加 generation 并设置事件。
Render Owner 醒来后串行调用 `TryGetNextFrame`，丢弃旧帧、保留最新帧，检查 `ContentSize`，复制到
自有纹理，然后立即关闭/释放所有 frame。Agile 表示可跨 apartment 使用，不表示对象方法可并发调用。

### 6.3 背景时效

根据 `SystemRelativeTime` 转换后的 QPC 计算样本年龄；时间差使用饱和减法，损坏或极端时间戳只能
落入 Stale/Future 状态，不能触发有符号溢出。首版窗口以背景采样 cadence 为单位，高刷新率显示器
至少按 60 Hz 周期计算：

- 普通动画仅在 `-3T <= age < max(6T, 100ms)` 时为新一批可见特效获取背景路径；
- 已进入背景感知的批次只用 `-3T <= age < max(12T, 250ms)` 的新 generation 刷新快照；已有快照
  在短暂缺帧时保留该路径，只有首次快照尚未建立时才允许单向降级；
- FX-only 批次在画面完全透明前保持 FX-only；透明帧解除锁存，下一批重新选择；
- 暂停后的背景刷新仅接受 `-T <= age < T`；已有快照保持路径并等待下一次 WGC 帧事件；
- epoch、尺寸、encoding 或排除合同错误：立即不可用。

可用性是二值合同。可用纹理始终同时驱动完整 Differential Bloom 与同一套 background-aware
source-over payload；不可用时两者同时切回 FX-only。sample age 不得映射为 Bloom、Alpha 或其他视觉
能量权重，避免捕获 cadence 在浅色背景上调制点击和拖尾亮度。窗口参数属于 ADR-007 的 Proposed
值；新批次必须先通过 acquire freshness，Render Owner 随后将可用 WGC 帧复制到独立快照。路径在
整个可见批次内保持不变，但每个通过校验的新 generation 都原子刷新快照；短暂丢帧时才沿用上一张。
这样 Differential Bloom 和最终 payload 在每次 present 中读取同一背景，又不会把首次点击时的浅色
桌面永久烘焙进移动拖尾。暂停只冻结 authored simulation，仍由 WGC 帧事件驱动静止特效重合成。
快照尚未建立的新批次才按 retain 窗口回退，真实 cadence/VRR 证据可收窄该窗口。背景 scRGB 不执行
全域分箱量化；只把 `1.0` 参考白周围的 `1/1024` 捕获噪声平台收敛到参考白，再于 `3/1024` 内
连续退出，抑制相邻 FP16 捕获值抖动而不让普通浅色渐变产生阶跃。最终 Alpha 固定来自该帧
authored Coverage/Bloom 传输容量，不由捕获 RGB 的近白分母反解；DirectEmission 与 Bloom RGB
保持独立加法能量，避免拖尾衰减或背景采样步进
放大为透明度闪烁。这个误差预算只属于已知背景的传输层，不能用于放宽 Unity Golden 的源渲染比较。

## 7. 捕获功耗状态

- `HOT`：按当前目标刷新率消费背景样本。
- `WARM`：build 26100+ 可请求 `MinUpdateInterval`；旧系统只能降低消费、copy、Bloom 和 present 频率，
  不宣称降低 WGC producer 成本。
- `COLD`：关闭 session 和 frame pool；恢复时完整重建，因为 WGC 没有 Pause 契约。

所有能力通过运行时 API presence/capability 探测，编译时使用 26100 SDK 不代表运行系统支持。

## 8. 色彩与输出

### 8.1 工作空间

中间渲染使用线性 BT.709/scRGB 数值。背景可能含负 scRGB；只有 Background-aware Bloom 的输入按定义
截到非负域，不能对捕获纹理做全局破坏性 clamp。

### 8.2 FP16 扩展预乘

第一版候选层序是 Coverage 后叠加 DirectEmission 和 Bloom：

```text
surface.rgb = coverage.premultipliedRgb + directEmission + bloom
surface.a   = coverage.alpha
```

最终 shader 直接写 FP16 surface，之后禁止 unpremultiply、canonicalize 或把 RGB clamp 到 alpha。
这只是 ADR-001 的 Proposed 顺序；在 Unity Golden 和 DirectComposition Spike 通过前不能标记 Accepted。

### 8.3 输出策略

- `PreserveDesktop`：只增加非负能量，不修改桌面；普通 SDR 白底可能不可见。
- `PreserveHueWithHeadroom`：在可用 SDR headroom 内柔和 rolloff；仍无法解决纯白无余量。
- `HighVisibility`：允许覆盖/对比度/压暗层，因此不再满足“桌面亮度只增不减”。

HDR active scene-referred scRGB 中可按约定使用 1.0 = 80 nits 的编码换算，但 `12.5` 只表示名义
1000 nits 数值，不保证显示器实测亮度。Advanced Color SDR 和普通 SDR 必须走各自的显示映射路径。

## 9. ROI 与 Bloom 金字塔

全屏 Bloom 是规范参考，Active-FX ROI 保持默认关闭。ROI 原点必须对齐最粗层 `2^L` 网格，或显式携带
与全屏相同的 pixel-center phase。guard band 是算子图最长 receptive-field 路径上各 pass footprint
换算到 mip0 后的和；不能用单一“半径 × mip 数”近似。dirty rect 必须 union 前后帧区域再扩张，
全屏与 ROI 使用相同 mip 数、UV、border mode 和奇数尺寸规则。

0.2.7 将纯特效 Bloom 的 prefilter 和完整 down/up 金字塔局部化。规划器为每个 pass 产生目标本地的
half-open 矩形，并保留全屏参考路径的 mip 数、UV、奇数尺寸、border mode 和 pixel-center phase。
前向非零支持与反向贡献依赖相交，避免把只依赖旧 `basePlan.bloomOutput` 的保守诊断边界误作正确性边界。
resolve 也有独立逻辑矩形；它同时是稳态 pure-FX primary 最终输出唯一可声明的损伤范围。
只有实际路径为 `RoiPyramid`、决策为 `Applied`、非 warmup，且 renderer 已验证局部最终输出时，
交换链表面的清理与最终 draw 才共用该 `resolveRect` scissor，并由 `Present1` 提交同一 dirty rect。
选择器还会重验证矩形边界以及 resolve 的 candidate/drawn/cleared 像素一致性；未通过时不得声明局部 Present。

- 每个实际 down/up 目标维护 `initialized`、上一写入矩形、全屏写入状态和最后 writer。首次 ROI、全屏
  转 ROI、resize、设备恢复或资源重建使用完整清理并报告预热帧；稳态矩形移动或 writer 改变时通过
  Context1 `ClearView` 清理上一写入矩形，同一 writer 连续覆盖相同矩形时直接跳过冗余清理，再按本
  pass scissor 绘制。`ClearRenderTargetView` 不能作为局部清理，因为它会清理整个资源；
- 一帧内采用全有或全无策略。规划器结果、Context1、资源身份、phase 或目标状态任一无效时，整条 Bloom
  同帧执行完整全屏路径，禁止把局部 prefilter/down 与全屏 up/resolve 混成不完整结果；
- 自适应门按完整计划的 candidate/full 像素计算：未进入时不超过 50% 才进入，已进入后超过 65% 才
  退出。触边、Bloom 关闭、core、背景差分、无有效计划和共享目标全屏写入等正确性门先于面积门；
- primary 与 recording-rebuild 分别维护诊断和自适应状态；Bloom 物理目标的清理与 writer 所有权只有
  一份。若 `background-aware` primary 已全屏写入共享目标，同帧录制重建必须按共享写入执行预热，不能
  误报稳态局部清理；
- background-aware/Differential Bloom、WGC 相关路径、warmup、任何 fallback 与普通调用者都保持完整输出
  及全屏 `Present`。recording-rebuild 和 Spout2 输出目标始终先建立完整表面，不借用交换链的
  dirty-present 保证；同帧 pure-FX primary 若独立满足上述合同，仅交换链目标可以局部提交。
  Differential Bloom ROI 属于独立的后续实验里程碑，不由完整金字塔 ROI 隐式启用；后续版本的 ROI
  产品开关均保持默认关闭；
- 一旦最终目标已局部写入，dirty rect 选择失败或 `Present1` 失败都不得改用全屏 `Present`。
  本次局部输出状态必须失效，且上一可见边界只在 Present 成功后提交；下次尝试从预热的
  完整清理、完整输出和全屏 Present 重建。

每个显示会话在渲染所有者侧维护 5 秒滚动窗，每 500 ms 构造脱离活跃渲染状态的不可变快照。IPC 只
读取发布快照，不得在逐帧路径获取控制面 mutex。schema 4 分别公开 primary/recording-rebuild 的
`roi-pyramid` 路径、原因、帧/像素、矩形、guard/phase，prefilter/downsample/upsample/resolve 的
full/candidate/drawn/cleared 像素，以及 Prefilter/Pyramid/FinalComposite GPU p50/p95。像素比例是工作量
诊断，不是 GPU 节省推导式。dirty-Present 帧/像素遥测只证明生产路径选择，WARP 只证明
D3D 输出表面合同；两者都不能单独证明 DWM 可见正确性、真实功耗收益或跨硬件支持。

## 10. 失败与降级

| 失败 | 必须行为 |
| --- | --- |
| WGC 不可用/拒绝/关闭 | Null sensor，继续 FX-only |
| 捕获帧超过保留窗口 | Differential Bloom 与背景反解同时单向切回 FX-only |
| ContentSize 改变 | 串行重建捕获资源，当前帧 FX-only |
| HDR/色彩元数据未知 | 使用保守 SDR 输出策略并记录诊断 |
| Monitor 跨 Adapter | 在目标资源域重建，不隐式共享 |
| Device removed/reset | 停止提交，销毁域内资源，重建后恢复 |
| ROI 不满足相位/guard | 回退全屏路径 |
| 局部最终输出后 dirty Present 失败 | 不执行全屏 Present；重置 ROI 状态，下帧完整重建 |
| NaN/Inf | 在产生边界 sanitize，并增加诊断计数 |

## 11. 决策与验收

产品控制面的边界和首个协议切片记录在
[`docs/adr/0008-product-control-plane.md`](docs/adr/0008-product-control-plane.md)。该 ADR
仍为 Proposed；虽然原生 Win32 Control Center 已完成首个构建切片，在安装/启动项和完整验收前，
不把产品层能力标记为完整支持。

架构只有在以下文件中的七个核心 ADR 均为 Accepted、产品控制面 ADR 有独立验收、四个 P0 Spike 均有可复现结论，并且没有未决 P0
问题后才能从 Proposed 升级。Spike 主假设可以失败，但必须已有 Accepted fallback 且同步收窄能力矩阵；
架构 Accepted 不等于产品或全部可选能力完成。

- `docs/adr/0001-final-composition.md`
- `docs/adr/0002-intensity-and-output-mapping.md`
- `docs/adr/0003-wgc-capability-and-power.md`
- `docs/adr/0004-self-exclusion-and-recording.md`
- `docs/adr/0005-golden-oracle.md`
- `docs/adr/0006-roi-and-mip-phase.md`
- `docs/adr/0007-background-temporal-validity.md`
- `docs/adr/0008-product-control-plane.md`
- `docs/SPIKES.md`
- `docs/VALIDATION.md`

状态升级必须使用独立评审提交，不能只修改一行 `Status`。
