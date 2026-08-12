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
- MeshTri 的 Custom1 溶解相位按重建工程 `Maximum Particle Timestep=0.03` 的 float32 子步求值：
  Burst 在首个子步末生成，Custom1 在下一子步上传；不能退化成固定 50/60 ms 延迟。
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
