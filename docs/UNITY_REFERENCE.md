# Unity/游戏视觉真值

## 1. 资产边界

外部游戏/Unity 参考树只用于本地研究、参数审计和视觉回归，不整体进入本仓库。仓库保存路径无关的
相对文件名、长度、SHA-256、数学契约和比较工具；不会提交 mesh、prefab、材质、Unity 截图或这些
外部工程的完整二进制副本。

生产运行时需要的四组 RGBA8 texel 是唯一例外：它们经 raw LZ4 Block 无损压缩为 C 字节串并编译
进 EXE，不保存 PNG 容器、Base64 文本或独立图片文件。其源容器哈希由维护生成器锁定，解码哈希由
生成数据和 `packed_fx_textures` 测试共同锁定。

默认本地根目录为：

```text
运行/截图工程:
D:\WebProjects\BA鼠标输入与点击特效系统\UnityMouseFxLab\UnityMouseFxLab

原始提取证据:
D:\WebProjects\BA鼠标输入与点击特效系统\提取资产2\BA_FX_Touch_UnityProject
```

其他机器可设置 `BAFX_UNITY_RUNTIME_ROOT` 和 `BAFX_UNITY_EXTRACTION_ROOT`，或向验证脚本传参。

## 2. 证据优先级

必须分别标记以下三类证据，不能合并为一个“Unity 参数”：

1. **OriginalSerialized**：游戏 `resources.assets`、原始 prefab、材质 JSON、编译 shader/机器码；决定原始值、
   blend、render state 和行为。
2. **ReconstructionChoice**：Unity 2021.3.45f1 URP 工程中的 shader/C# 还原；用于重放和生成中间 buffer，
   但不能覆盖 OriginalSerialized。
3. **RuntimeObservation**：固定种子截图、游戏截图、Player 输入探针、结构化日志、径向量化和人工观察；
   用于验证实现是否落入正确时间包络与运行时行为边界。

`ba-click-fx` 只提供行为/配置兼容参考，不是像素真值。

## 3. 已核对基线

- 游戏资源由 Unity `2021.3.56f2` 构建；可运行重建工程是 Unity `2021.3.45f1`。
- 当前运行工程与历史提取工程的 `Assets/Imported/FX_Touch` 均为 25 个文件、423,045 字节，逐文件
  相对路径、长度和 SHA-256 相同。
- 当前 Golden 使用 `1950x1097`、固定 UI `Ortho(-aspect, aspect, -1, 1)`、ARGBHalf/Linear、MSAA 1。
- 固定时间片为 50、100、110、120、130、140、150、180、250、450 ms。
- Bloom 从 `975x548` 开始，共 6 层，最小 mip 为 `30x17`，sample scale 为 `1.42925835`。
- 拖拽诊断使用 `140 ms` 内水平移动 `432 px`：先在起点静止推进一帧，再分 12 段移动；
  `WithTrail` 与 `NoTrail` 使用相同种子和粒子轨迹，只切换 TrailRenderer。生成脚本与两张诊断图均由
  `reference/unity-reference.json` 锁定，供后续拖尾差分门禁使用。
- `TrailOnly_20px` 诊断关闭全部 ParticleSystem，只保留中心两侧各 `10 px` 的两点 TrailRenderer；
  它同样由 manifest 锁定，专门比较拖尾几何、材质与 Bloom，避免 Unity/原生不同随机粒子流经过
  sRGB 量化和饱和后污染 `WithTrail - NoTrail` 的弱光尾部。
- Unity 工程使用 Linear active color space；Prefab Gradient 中的浮点键已是该活动色彩空间的数值，
  不再执行 sRGB 解码。Web 参考实现同样把 Gradient 与 sRGB 纹理解码分开处理；原生只对贴图 SRV
  使用 sRGB 解码，直接传递 Gradient 键值。

上述“截图一致”只证明重建路径在该矩阵中的观察结果；它不自动证明游戏所有 render queue、pause、
slow-motion、录屏、HDR 显示或桌面合成行为一致。

### Windows Player 连续帧时序

2026-08-12 使用 Unity `2021.3.45f1` 重建工程构建 WindowsPlayer/D3D11 探针。每档刷新率均固定
随机种子、预热 30 帧、关闭 VSync、启用 `Application.runInBackground`，并在严格串行的独立 Player
进程中通过 `WaitForEndOfFrame` 逐帧采样。Unity 2021.3 没有公开 `Particle.time`，日志中的粒子年龄为
`startLifetime - remainingLifetime`。

| 目标刷新率 | 捕获帧数 | 总时长 | 平均帧间隔 | 约 50 ms 时 `ring` 年龄 |
| ---: | ---: | ---: | ---: | ---: |
| 60 Hz | 57 | 0.9520113 s | 0.016701958 s | 0.03333393 s |
| 120 Hz | 114 | 0.9510710 s | 0.008342729 s | 0.04166920 s |
| 240 Hz | 224 | 0.9515688 s | 0.004248075 s | 0.04575591 s |

`ring` 与 `Ring (3)` 均在第一个采样帧出生且年龄约为零，随后按真实 Player 帧间隔推进；这排除了
统一减去固定 `25 ms` 的模型。`Ring (3)` 的四个点击碎片在三档运行中的首帧年龄均为零。

`Ring (4)` 的距离发射粒子分别拥有独立年龄。探针以 `2 world/s` 匀速移动，Prefab 的
`rateOverDistance=5` 因此每移动 `0.2 world` 发射一粒；反推出的首粒系统出生时刻分别约为
`0.1166662 s`、`0.1083339 s`、`0.1041670 s`（60/120/240 Hz），其余粒子约每 `0.1 s` 出生。
这项证据要求拖拽碎片使用距离交点内插的自身出生时刻，不能复用点击 Burst 或根对象年龄。

本地原始证据及 SHA-256：

```text
unity-player-particle-timing-60hz.log
AF7A155B561AF959E65B26DE6BCBA7EF35B87075FA0078D146B46299277CEA00
unity-player-particle-timing-120hz.log
9C87096C93ECB24FEAA2C62384FD56A21DC52C429740070C2ADD6C376FB5DEE3
unity-player-particle-timing-240hz.log
2CA2F1D2518933CBCFA7C239EC1B179616458C06C8FE7AF91DD8F28A510AD90D
unity-player-particle-timing-summary.md
3856E22A116EC1CCD2B6F72C79766183D1CE5F3CCE80876ECD3A6F73BED39552
unity-player-probe-build.log
07C2CE10BC692C7AD0F7FB8DC40BA639673FB7D2FBE19DED8F750A781C1B2FFE
```

这些日志证明的是重建工程 Windows Player 的粒子时钟，不是商业游戏进程。探针使用合成直线路径而非
OS 指针输入；首次观察位置包含 shape offset 和观察前已累积的运动；240 Hz 运行中存在一次
`9.9739 ms` 调度离群，但连续采样和完成标记均完整。

## 4. 原始材质与 Bloom 契约

| 材质 | Game queue | 原始 shader | 纹理/通道 | 艺术强度 |
| --- | ---: | --- | --- | ---: |
| Cross2 | `4499` | `FX_SHADER_AlphaBlend_Add` | Circle R 控制 alpha | `2.0` |
| Tri 2 | `4550` | `FX_SHADER_Additive_0` | Triangle RGBA | `5.992157` |
| Tri3 | `4499` | `FX_SHADER_Dissolve_GachaGauge_P` | Ring + white default mask | `5.992157` |
| Trail | `4499` | `FX_SHADER_Additive_0` | Trail RGBA | `23.968628` |

这些数值全部标记为 `ArtisticRelative`，不得显示或转换成物理 nits。原始 shader 契约为：

- Additive：`Blend One One`，RGB 乘 texture/material/particle/alpha/intensity，目标 alpha 固定为 1。
- AlphaBlendAdd：`Blend One OneMinusSrcAlpha`，Cross2 使用纹理 R 通道作为 alpha。
- Dissolve：`Blend SrcAlpha One, One One`，执行
  `clip(main.a * mask.a * particle.a - Custom1.x)`；Custom1.x 是硬裁剪阈值。
- 原始 DSFX pass 参数要求 `Cull Back`、`ZTest LEqual`、`ZWrite Off`；游戏 UIRenderPass 又施加有效的
  `ZTest Always` override。原生桌面 overlay 没有可用的游戏 scene depth，因此使用 depth disabled/Always，
  但 manifest 仍分别保留材质序列化状态和有效 UI pass 状态。

游戏 Bloom 使用 `Hidden/MXFinalBloom`：Intensity `1.7`、Threshold `1`、Soft Knee `0`、Clamp
`65472`、Diffusion `7`、Anamorphic `0`、白 tint。合成倍率是
`exp2(1.7 / 10) - 1`，不是直接乘 `1.7`。

## 5. 原生实现的映射

- 四张运行时纹理逐张执行有界 LZ4 解压并直接创建 immutable RGBA8/sRGB GPU 资源；上传后释放
  CPU texel。Host 不初始化 WIC，开发用 Capture 工具仍可用 WIC 输出验证 PNG。
- 中心 disk 和圆环分别按原纹理通道、硬裁剪及 draw order 求值。
- Prefab 中 `ring`、`MeshTri`、`Ring (3)`、`Ring (4)` 是四个独立 ParticleSystem，分别对应中心
  disk、溶解环、点击碎片和距离发射拖拽碎片。原生实现分别保存前三个 Burst 系统的步进状态，并让
  每个拖拽碎片保存距离交点内插的出生时刻；这些状态不能合并成根对象的统一年龄。
- Unity schema 2 粒子 Fixture 额外导出 50/100/120 ms 的 7 粒子与 250/450 ms 的 6 粒子最终状态。
  启用 Texture Sheet 的三角系统从 BakeMesh UV 解析 atlas 帧，其他系统固定为 0；`bafx::reference`
  按年龄将这些观察值生成 Capture-only
  `FrameSnapshot`，用于坐标和逐像素诊断；它不写入
  生产 `Simulation`，不消费或替换原生 RNG，也不证明 Unity/native 的后续随机序列相同。
- MeshTri 的 Custom1 溶解相位按重建工程 `Maximum Particle Timestep=0.03` 的 float32 子步求值：
  Burst 在首个子步末生成，后续每个子步先从上一粒子年龄上传 Custom1、再推进年龄。这是重建工程
  与 Golden 的 ReconstructionChoice；上述 Player 日志未直接采样 MeshTri。MeshTri 的尺寸、颜色、
  旋转和可见寿命使用同一个逐步粒子年龄，不能从绝对时间回算或退化成固定 25/50/60 ms 延迟。
- Web 参考实现同样把 `ClickWave` 与点击/拖拽 `ShardParticle` 分开记时，并按碎片类型消费 click 或
  trail 虚拟时钟；这支持原生的状态归属划分，但仍只属于行为参考，不能覆盖 Unity 视觉真值。
- 点击/拖拽碎片保留几何、时间、颜色、Unity HDR 核心和清晰边缘；Tri2 使用
  `FX_SHADER_Additive_0`，因此同时写入 DirectEmission 和 BloomSeed，随 UI HDR 缓冲进入
  游戏的全场景 `MXFinalBloom`。原生实现保留锐利核心并恢复对应的三角光晕。
- Trail、圆环和三角碎片均可写非负的 DirectEmission/BloomSeed；写入前先从 ArtisticRelative
  经过版本化校准。
- Trail 保持 Prefab 的 `time=0.3`、`widthMultiplier=0.005` 和 `m_MinVertexDistance=0.01`。真实 Player
  验证表明该距离只过滤每帧 Transform 样本：单帧移动 `0.9 world` 仍只有首尾两点，不会沿线自动补点。
  原脚本的 `Update` 按 Down→Held→Up 查询 Legacy Input，并只使用该帧的同一份 `Input.mousePosition`；
  普通 Up-only 释放帧的 `GetMouseButton(0)` 为 false，因此不会在 `GetMouseButtonUp(0)` 前再移动
  根对象。原生按压路径据此在每次输入消费/呈现更新中只应用一份帧边界当前位置，并让所有模拟动作共享 `renderTime`。
  消息分派 QPC 只服务可选 `input.samplingRateHz` 输入相位，不属于 Unity 序列化美术参数。
- `FxTrailTimeScale` 脚本构造默认值是 `killUnderTimeScale=0.3333`，但游戏中 Trail 实例的 MonoBehaviour
  原始数据在字段偏移 `0x20` 序列化为 `5C 8F 42 3E`，即 `0.19f`；运行时以该实例覆盖值为准。
  `Time.timeScale <= 0.19` 首次进入 parking 时读取全部 Trail 顶点，丢弃索引 0 并缓存剩余后缀；后续每个
  `Update` 先把缓存写回 Renderer，再删除缓存头点。缓存不足两点时直接 `Clear()` 并禁用 Renderer，
  因而 `N=0..4` 的可见点数序列分别为 `0`、`1→0`、`2→0`、`3→2→0`、`4→3→2→0`。
  倍率恢复到阈值以上时只清缓存并重新启用 Renderer，不清除当前可见后缀。
- `FXTouch.Stop()` 只清 Trail 并停止粒子，不重置相邻 `FxTrailTimeScale` 的 parking 标志、Renderer 启用态
  或未完成缓存；这些组件状态会随池化对象保留。`Reference/Diagnostics/Pool/FXTouch_ComponentPool_FIFO.md`
  锁定的当前 Steam Build `24542715` 审计表明，`ComponentPool<T>` 以 `Queue<T>` 保存对象：`AddObject`
  从队尾 `Enqueue`，`GetObject` 从队首 `Dequeue`，`SyncComponentPool<T>` 不改变该顺序，因此
  `SyncComponentPool<FXTouch>` 是 FIFO。原生 `SimulationRuntime` 已复现初始预热一个对象、池空时创建、
  失活后归还、优先复用最早归还对象以及 `FxTrailTimeScale` 组件状态保留；每次激活只重新分配 native
  确定性随机流，不据此宣称 Unity ParticleSystem 随机流等价。Web/native 常驻拖尾不进入该池，并使用
  独立随机域。`updateUnityTrailTimeScale` 仍只提供逐游戏 Update 的显式入口；桌面 Host 没有真实
  `Time.timeScale` 来源，因此生产路径尚未调用它，也不能由桌面“暂停特效”代替。桌面暂停继续冻结
  独立仿真时间轴。
- `FXTouch.Duration` 来自根 ParticleSystem；Prefab 的根 `duration=1 s` 且 `useUnscaledTime=1`。
  `TouchEffectCreater.CoRestoreClickEffect` 的字面实现是在释放后等待 `Duration * 60` 次
  `WaitForEndOfFrame`，即 60 次呈现，在 `60 Hz` 下等价于 `1 s`。原生将此解释为游戏按 60 Hz 换算
  一秒美术寿命，并明确采用刷新率无关的 `1 s` 仿真时间作为 **ReconstructionChoice**：边界帧呈现后
  才停止并归还对象，桌面暂停期间不消费这段时间。若字面按 60 次 Present 回收，`120/144/240 Hz`
  会分别约在 `500/417/250 ms` 截断最长 `600-700 ms` 的碎片，因此不作为当前桌面合同。
  Web 风格的 `disk.lifetimeMs` 与 `rings.lifetimeMs` 只调整各子粒子的归一化年龄，不延长这段固定
  `1 s` 根对象回收期限；较长寿命或较慢 `clickTimeScale` 仍可能在释放后被根合同截断。
- Web 版的 coalesced events 与未按键常驻拖尾只作为 native/Web 产品增强，不是 Unity 路径真值。原生会
  按原序无损保留同帧多个 Raw Input 边沿，仅用于诊断和 native 扩展，且含边沿帧不会从尾随 Move
  重启常驻段；严格效果路径将边沿归约为 Down/Held/Up 布尔帧态并按脚本的 Down→Held→Up 顺序执行，
  Cancel 最后作为 native 硬边界处理。Unity `2021.3.45f1` Windows Player 黑盒已确认
  `Reference/Diagnostics/Input/FXTouch_LegacyInput_DownUpDown.{md,json}` 的 `Valid=true`，并记录
  `Down-Up-Down` 在第 4 帧得到 Down=true、Held=true、Up=true；清理最终按下态后，第 5 帧为
  Down=false、Held=false、Up=true。该证据不覆盖其他边沿排列，也不证明游戏所用 Unity
  `2021.3.56f2` 行为相同。`30 Hz` 是人工视觉审核建议，不是游戏硬编码真值。
- 按住期间即使没有新的 OS Move，Unity 粒子更新仍会观察当前根 Transform。原生每次 `advance`
  因此只推进距离发射的静止时间基线，不追加 Trail 顶点，也不推进独立的输入限频相位；下一次位移的
  距离粒子出生时刻只在最近两个仿真更新之间内插，不会跨越更早的静止区间。
- Unity 全场景 Bloom 是 FX-only Golden；桌面 Background-aware Differential Bloom 只改变背景交互，
  不能反向调节原粒子参数来追逐一张桌面截图。
- 50 ms 主要是中心 disk；100–180 ms 是环和近中程辉光包络；250/450 ms 验证消散和碎片/弧段尾部。

## 6. 验证

```powershell
pwsh -NoProfile -File tools\verify-unity-reference.ps1
```

脚本会验证 manifest 中的关键原始证据、重建实现、十张 Golden、五个粒子 Fixture 及其生成式 C++ 数据，
以及完整 Imported tree 的数量/字节数。
任何哈希变化都必须先判断是游戏更新、Unity 重建变更还是 baseline 更新，禁止直接刷新 manifest 让测试变绿。
原生拖拽与纯拖尾门禁命令、指标和证据边界见 `docs/VALIDATION.md`；它读取上述已锁定诊断图，
不会以 Web 像素或原生粒子随机坐标替代 Unity Golden。
