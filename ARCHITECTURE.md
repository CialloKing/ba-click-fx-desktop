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
- `ARCH-INV-006`：背景无效、编码不兼容或过期时禁止 Differential Bloom。
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

三角碎片可写 `coverage` 和用于保留清晰 HDR 核心的 `directEmission`，但必须保持
`bloomSeed = 0`。这里的 crisp direct energy 不经过模糊金字塔，因此不产生三角光晕。

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
enum class CaptureInteractionMode
{
    BackgroundAware,
    RecordingCompatible
};
```

- `BackgroundAware`：启用 WGC 并请求 `WDA_EXCLUDEFROMCAPTURE`，优先避免自反馈；外部录屏可能不包含 FX。
- `RecordingCompatible`：关闭背景感知并关闭 WDA，使用 FX-only 渲染；只提高兼容性，不保证所有录屏器可见。

## 6. 线程与帧时序

### 6.1 输入

窗口线程接收 Raw Input/Pointer 消息，记录接收时 QPC，并写入单生产者命令队列。Render Owner
消费命令并执行固定步长模拟。视觉位置可用绝对指针坐标锚定，时间不得使用消息处理完成后的时间代替。

### 6.2 WGC

`Direct3D11CaptureFramePool::CreateFreeThreaded` 的 `FrameArrived` 回调只增加 generation 并设置事件。
Render Owner 醒来后串行调用 `TryGetNextFrame`，丢弃旧帧、保留最新帧，检查 `ContentSize`，复制到
自有纹理，然后立即关闭/释放所有 frame。Agile 表示可跨 apartment 使用，不表示对象方法可并发调用。

### 6.3 背景时效

根据 `SystemRelativeTime` 转换后的 QPC 计算样本年龄。首版保守策略以目标显示器刷新周期为单位：

- 不超过 1 个刷新周期：权重 1；
- 1 到 3 个刷新周期：连续衰减到 0；
- 超过 3 个刷新周期、时间倒退或时间戳无效：权重 0。

权重只影响 Background-aware Differential Bloom；直接特效和 FX-only Bloom 不受影响。阈值属于
ADR-007 的 Proposed 参数，真实 cadence/VRR 证据可收窄它，但不能取消 stale cutoff。

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

全屏 Bloom 是规范参考，ROI 在 ADR-006 接受和 VAL-ROI 通过前默认关闭。ROI 原点必须对齐最粗层
`2^L` 网格，或显式携带与全屏相同的 pixel-center phase。guard band 是算子图
最长 receptive-field 路径上各 pass footprint 换算到 mip0 后的和；不能用单一“半径 × mip 数”近似。
dirty rect 必须 union 前后帧区域再扩张，以清除上一帧残留。全屏与 ROI 使用相同 mip 数、UV、border mode
和奇数尺寸规则。

## 10. 失败与降级

| 失败 | 必须行为 |
| --- | --- |
| WGC 不可用/拒绝/关闭 | Null sensor，继续 FX-only |
| 捕获帧过期 | 背景差分 Bloom 衰减为 0 |
| ContentSize 改变 | 串行重建捕获资源，当前帧 FX-only |
| HDR/色彩元数据未知 | 使用保守 SDR 输出策略并记录诊断 |
| Monitor 跨 Adapter | 在目标资源域重建，不隐式共享 |
| Device removed/reset | 停止提交，销毁域内资源，重建后恢复 |
| ROI 不满足相位/guard | 回退全屏路径 |
| NaN/Inf | 在产生边界 sanitize，并增加诊断计数 |

## 11. 决策与验收

产品控制面的边界和首个协议切片记录在
[`docs/adr/0008-product-control-plane.md`](docs/adr/0008-product-control-plane.md)。该 ADR
仍为 Proposed；在 Control Center 和安装/启动项完成前，不把产品层能力标记为完整支持。

架构只有在以下文件中的七个 ADR 均为 Accepted、四个 P0 Spike 均有可复现结论，并且没有未决 P0
问题后才能从 Proposed 升级。Spike 主假设可以失败，但必须已有 Accepted fallback 且同步收窄能力矩阵；
架构 Accepted 不等于产品或全部可选能力完成。

- `docs/adr/0001-final-composition.md`
- `docs/adr/0002-intensity-and-output-mapping.md`
- `docs/adr/0003-wgc-capability-and-power.md`
- `docs/adr/0004-self-exclusion-and-recording.md`
- `docs/adr/0005-golden-oracle.md`
- `docs/adr/0006-roi-and-mip-phase.md`
- `docs/adr/0007-background-temporal-validity.md`
- `docs/SPIKES.md`
- `docs/VALIDATION.md`

状态升级必须使用独立评审提交，不能只修改一行 `Status`。
