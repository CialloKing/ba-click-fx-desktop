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
3. **RuntimeObservation**：固定种子截图、游戏截图、径向量化和人工观察；用于验证实现是否落入正确时间包络。

`ba-click-fx` 只提供行为/配置兼容参考，不是像素真值。

## 3. 已核对基线

- 游戏资源由 Unity `2021.3.56f2` 构建；可运行重建工程是 Unity `2021.3.45f1`。
- 当前运行工程与历史提取工程的 `Assets/Imported/FX_Touch` 均为 25 个文件、423,045 字节，逐文件
  相对路径、长度和 SHA-256 相同。
- 当前 Golden 使用 `1950x1097`、固定 UI `Ortho(-aspect, aspect, -1, 1)`、ARGBHalf/Linear、MSAA 1。
- 固定时间片为 50、100、110、120、130、140、150、180、250、450 ms。
- Bloom 从 `975x548` 开始，共 6 层，最小 mip 为 `30x17`，sample scale 为 `1.42925835`。

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
- MeshTri 的 Custom1 溶解相位按重建工程 `Maximum Particle Timestep=0.03` 的 float32 子步求值：
  Burst 在首个子步末生成，后续每个子步先从上一粒子年龄上传 Custom1、再推进年龄。这是重建工程
  与 Golden 的 ReconstructionChoice；上述 Player 日志未直接采样 MeshTri。MeshTri 的尺寸、颜色、
  旋转和可见寿命使用同一个逐步粒子年龄，不能从绝对时间回算或退化成固定 25/50/60 ms 延迟。
- Web 参考实现同样把 `ClickWave` 与点击/拖拽 `ShardParticle` 分开记时，并按碎片类型消费 click 或
  trail 虚拟时钟；这支持原生的状态归属划分，但仍只属于行为参考，不能覆盖 Unity 视觉真值。
- 点击/拖拽碎片保留几何、时间、颜色、Unity HDR 核心和清晰边缘；它们可写 DirectEmission，
  但必须 `BloomSeed=0`，因此不会产生模糊三角光晕。
- Trail 与圆环可写非负的 DirectEmission/BloomSeed；写入前先从 ArtisticRelative 经过版本化校准。
- Trail 保持 Prefab 的 `time=0.3`、`widthMultiplier=0.005` 和 `m_MinVertexDistance=0.01`。真实 Player
  验证表明该距离只过滤每帧 Transform 样本：单帧移动 `0.9 world` 仍只有首尾两点，不会沿线自动补点。产品设置
  `input.samplingRateHz` 只近似客户端每帧提交触点位置的频率，不属于 Unity 序列化美术参数；`30 Hz`
  是人工视觉审核建议，不是游戏硬编码真值。
- Unity 全场景 Bloom 是 FX-only Golden；桌面 Background-aware Differential Bloom 只改变背景交互，
  不能反向调节原粒子参数来追逐一张桌面截图。
- 50 ms 主要是中心 disk；100–180 ms 是环和近中程辉光包络；250/450 ms 验证消散和碎片/弧段尾部。

## 6. 验证

```powershell
pwsh -NoProfile -File tools\verify-unity-reference.ps1
```

脚本会验证 manifest 中的关键原始证据、重建实现、十张 Golden 以及完整 Imported tree 的数量/字节数。
任何哈希变化都必须先判断是游戏更新、Unity 重建变更还是 baseline 更新，禁止直接刷新 manifest 让测试变绿。
