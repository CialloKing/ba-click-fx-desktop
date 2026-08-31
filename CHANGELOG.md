# 变更记录

## 0.2.7 - 2026-08-31

### 优化

- Active-FX ROI 从 Bloom 首级扩展到完整金字塔。规划器为 prefilter、每级 downsample/upsample 和
  resolve 生成独立矩形，同时保留全屏参考路径的 mip 数、UV、奇数尺寸、border mode 和 pixel-center
  phase；down/up 使用实际 scissor。
- 每个实际 down/up 目标记录初始化、上一写入矩形、全屏写入状态和最后 writer。首次进入、全屏转 ROI、
  resize 或资源恢复时执行完整预热清理；稳态矩形移动或 writer 改变时通过
  `ID3D11DeviceContext1::ClearView` 清理上一写入区域，同一 writer 连续覆盖相同矩形时跳过冗余清理。
  primary 与 recording-rebuild 分别记账，但共享物理资源的写入状态只有一份。
- ROI 按帧执行全有或全无：pass 计划、Context1、资源身份、相位或状态任一无效时，整条 Bloom 在同一帧
  回退全屏，不混用局部与全屏 pass。
- 满足 `RoiPyramid + Applied`、非预热、无背景且最终输出矩形逐字段一致的 steady primary 帧，会只清理
  和绘制当前与上一帧视觉范围的并集，再以 `Present1` dirty rect 发布。选择器拒绝任何不完整合同；局部
  输出后若 Present 失败，不会错误退回 full Present，而是重置 ROI 状态并在下一帧完整重建。
- `GetDisplayState` 升级为严格 schema 4，新增 `roi-pyramid` 路径，并分别报告 prefilter、downsample、
  upsample、resolve 的 full/candidate/drawn/cleared 像素。Control Center 与支持日志同步消费同一合同。

### 兼容性与升级

- 源码产品版本提升到 0.2.7；主配置 schema 保持 19，`performance.activeFxRoiEnabled` 继续默认
  `false`，不增加第二个 ROI 开关或配置迁移。现有配置、显示器 override、effects-only Profile 和
  渲染协议均不重写。
- `GetDisplayState` schema 4 不提供 schema 3 兼容层；混合版本继续由 `GetState.productVersion`
  fail-closed 设置门拒绝写入，Host 启动和关闭入口保持可用。

### 验证与发布状态

- WARP 已覆盖完整金字塔、FP16/BGRA8 最终输出、MRT/录制完整输出、移动视觉范围和局部输出外哨兵，
  Full `release-verify` 为 `45/45`，Slim `slim-release-verify` 为 `44/44`。这些确定性结果不替代真实
  DWM、功耗、输入延迟或跨硬件证据。
- 旧 capture schema 3 / report schema 2 的 RTX 4060、4K 170 Hz、SDR 20-run 继续保留为当时 revision 的
  `FAIL`，不会用新规则追溯改判。复核表明 CPU FrameTotal 与 PresentCall 四项是同一 API 阻塞的重复
  观察，且固定场景禁用 Raw Input；正式 report schema 4 因此把它们保留为非阻塞 advisory，FPS、GPU
  command、pending、错误、ROI 收益和 dirty Present 覆盖继续是硬门。
- 当前实现的 schema 4 正式硬件采集尚未完成。2026-08-31 的短矩阵在首个 run 后因实际输出为
  `2560x1440 @ 165.003 Hz`、不满足预注册的 `3840x2160 @ 170 Hz` 合同而 fail-closed。ROI 保持默认关闭
  且不作整机性能声明；该晋级证据不阻塞 0.2.7 的普通发布，但继续阻塞默认启用和性能宣传。

### 支持边界

- 默认 `background-aware` 的 Differential Bloom、WGC、Spout2 格式转换、recording-rebuild 最终输出和
  所有 fallback 仍完整输出并使用普通 Present。受限 dirty Present 只属于验证后的 steady pure-FX
  primary，不等于桌面捕获 ROI、全交换链局部化或普遍录屏支持。
- ROI 继续是默认关闭的实验项。AMD、Intel、HDR、Windows 11、多显示器和跨适配器硬件矩阵在真实
  执行前保持 `Not Run`，不能由 WARP、计数器或 RTX 4060 的单机结果外推。0.2.8 Differential Bloom
  ROI 仍须在新的性能晋级合同通过后另行开发。

## 0.2.6 - 未发布（2026-08-24 性能门禁失败）

### 新增

- Active-FX ROI 的 Bloom 首级目标新增真实局部清理状态机。首次进入 ROI 或从全屏切回 ROI 时先执行
  一次完整清理并报告预热帧；稳态通过 `ID3D11DeviceContext1::ClearView` 清理上一写入矩形，再以
  scissor 绘制当前矩形，避免 `ClearRenderTargetView` 的全资源清理抵消首级收益。
- ROI 使用内部自适应门：首级实际 scissor 面积不超过 50% 时进入，已进入后超过 65% 才退出；触边、
  Bloom 关闭、core 模式、背景差分、无有效计划和其他正确性门始终优先。Context1 不可用或渲染状态
  异常时，同帧回退完整全屏路径。
- `GetDisplayState` 升级为严格 schema 3。每个显示会话新增近 5 秒的 Active-FX ROI 不可变快照，分别
  报告 primary 与 recording-rebuild 的实际路径、决策原因、帧数、像素、矩形、guard/phase 和分阶段
  GPU p50/p95；快照每 500 ms 发布，不让 IPC 在逐帧路径获取互斥体。
- Control Center 的开关改名为“启用自适应 Active-FX ROI（实验）”，并为选中显示器增加工程面板。
  页面可见时每秒刷新，超过 3 秒标记为 stale；IPC 异常只影响诊断显示，不会改写配置或影响 Host。
- GPU 时间戳遥测拆分 primary 与 recording-rebuild 两条路径的 Prefilter、Pyramid 和 FinalComposite，
  同时保留既有 Bloom 总耗时；查询继续异步、非阻塞，不等待也不强制 `Flush`。
- 增加 Active-FX ROI 专用 A/B 采集与报告合同：同一 EXE、同一场景、schema 19 配置仅切换 ROI 开关，
  固定执行 5 个 ABBA 块、20 次采集，并按发布门槛判定结果。

### 兼容性与升级

- 产品版本提升到 0.2.6；主配置 schema 保持 19，`performance.activeFxRoiEnabled` 仍默认关闭。
  现有配置、显示器 override、effects-only Profile 和渲染协议均不迁移、不重写。
- `GetDisplayState` schema 3 是有意的破坏性协议更新，不提供 schema 2 兼容层；Host 与 Control Center
  必须使用同一产品版本。`GetState.productVersion` 的混合版本 fail-closed 设置门保持不变。

### 支持边界

- 本轮只局部化纯特效 Bloom 首级预滤波，以及需要执行时的录制/Spout2 纯特效重建首级。
  默认 `background-aware` 的 primary Differential Bloom、完整 Bloom down/up/resolve、最终合成、
  WGC、Spout2 格式转换、交换链与 Present 仍为全屏。
- 工程面板中的像素处理比例只描述首级工作量，不等于 GPU 或端到端性能节省。RTX 4060 4K SDR
  A/B 门槛通过前不发布性能声明；AMD、Intel、HDR、Windows 11、多显示器和跨适配器 ROI 矩阵保持
  `Not Run`。

## 0.2.5 - 2026-08-24

### 新增

- Host、Control Center、Windows 文件版本和安装状态改用同一份生成的产品版本来源。`GetState`
  新增 `productVersion`，Control Center 的窗口标题和系统页会同时显示自身版本、Host 版本、安装状态
  与最近一次手动查询到的最新公开版本。
- 系统页新增手动“检查更新”和“打开 Release”。程序不会在启动、连接 Host 或托盘恢复时自动联网，
  也不会自动下载或执行任何资产；Release 按钮只打开固定的
  `https://github.com/CialloKing/ba-click-fx-desktop/releases/latest` 官方页面。

### 兼容性与升级

- Control Center 只有在 Host `productVersion` 与自身版本完全一致时才开放设置写入。版本缺失、格式错误
  或不一致时会禁用设置控件，但仍保留 Host 启动和关闭入口，便于完成升级或恢复。
- 系统页把有效且版本一致的安装状态（包括从有效备份成功恢复）显示为“安装版”，主状态与备份均
  不存在时显示为“便携版”；状态损坏、产品/Package 版本冲突或只完成部分升级时显示
  “安装状态异常”，不会伪装成便携版。
- 配置 schema 保持为 19，特效 Profile 仍是 effects-only 独立 JSON。升级不迁移、不重置主配置、
  自定义 Profile、显示器 override、日志或其他 `data` 数据。旧 Control Center 没有本版检查入口，
  首次升级到 0.2.5 仍需用户从官方 Release 页面手动下载安装包或便携包。

### 优化

- Host 渲染循环改用只含配置、配置代次、暂停和退出状态的轻量快照，不再每帧
  复制特效 Profile 目录或序列化所有候选项来匹配活动 Profile；`GetState` 的完整
  Profile 状态保持不变。

### 修复

- Control Center 的单实例恢复改用固定窗口类名，不再依赖会随产品版本变化的窗口标题。

## 0.2.4 - 2026-08-23

### 新增

- 新增中心圆盘、圆环、点击碎片、拖尾碎片、拖尾线和 Bloom 六个独立分层开关；关闭 Bloom 时
  直接旁路整条 Bloom 金字塔，保留未关闭的直接材质层。
- 新增 Host-owned、effects-only 特效 Profile。Control Center 提供四个内置预设，并支持按名称
  保存、应用、覆盖和删除自定义预设；每个自定义项使用独立原子 JSON 文件保存。

### 优化

- 新增默认关闭的实验性 Active-FX ROI。当前只裁剪纯特效 Bloom 的首级预滤波；边界、面积或
  计划不满足约束时自动回退全屏路径，后续 Bloom、最终合成、WGC 与 Spout2 合同保持不变。

### 支持边界

- Active-FX ROI 尚未扩大硬件支持范围。真实 GPU 性能、边界像素、HDR、多显示器和跨适配器矩阵
  在完成硬件验收前仍为实验项或 `Not Run`。

## 0.2.3 - 2026-08-23

### 优化

- `match-display` 现在使用目标显示器报告的精确刷新率分数限制提交节拍；刷新率缺失或无效时
  保守回退到 60 FPS。新增 `unlimited` 帧率策略，显式保留此前不设置额外帧周期的行为。
- 启用空闲资源优化时，Spout2 在特效结束并提交透明清屏帧后停止完整 FX/Bloom 渲染，改用
  低频纹理心跳维持发送者生命周期，降低 OBS 空闲输出的 GPU 占用。

### 修复

- 修复 Spout2/OBS 把独立 Bloom 做 sRGB 提亮后直接叠加到编码游戏画面，导致光晕暗部被放大、
  中心高光过早削顶的问题。v5 输出改用保色相 SDR rolloff，并保留 v4 的扩展预乘 Alpha 合同。

## 0.2.2 - 2026-08-21

### 修复

- 修复 Host 启动前已经连接 HDMI 采集卡时，采集端的活动 DisplayConfig source 没有
  `HMONITOR`，导致全局拓扑为 `incomplete / ERROR_NOT_FOUND`，内置主屏的 SDR white
  查询被一并阻断，WGC 虽持续取得帧但背景无法参与合成的问题。
- 新增逐显示器的颜色路径完整性门。采集端等无法映射到桌面显示器的独立路径仍保留全局
  `incomplete` 诊断，但不再污染证据完整的内置屏颜色状态。
- 无法归属到具体 source、source 身份不完整或缺少物理 target 时继续 fail-closed，避免在
  HDR、克隆显示或真实不完整路径中误用颜色状态。

### 验证与支持边界

- 已在笔记本内置屏加 HDMI 采集卡的真实冷启动场景确认：全局拓扑保持
  `incomplete / ERROR_NOT_FOUND` 时，内屏仍取得 fresh SDR 颜色快照，WGC 帧和背景合成帧
  持续推进，无需重新热插拔采集卡。
- 本补丁不扩大单主屏 SDR 支持合同。多显示器、HDR/Advanced Color、混合 DPI/刷新率、
  跨适配器和完整热插拔矩阵仍为实验项或 `Not Run`。

## 0.2.1 - 2026-08-21

### 修复

- 修复笔记本内置屏幕使用 `background-aware` 时，HDMI 采集卡引起显示拓扑变化后
  WGC 会话停止但背景捕获没有重启，画面长期回退为 FX-only 的问题。
- 新增一次性、5 秒有界的显示拓扑恢复门，兼容 Win32 拓扑通知与 WGC
  `session-stopped` 的两种到达顺序。恢复只在硬件渲染、配置仍请求背景捕获、
  WGC 允许重启且显示器供电可用时发生。
- 新增 `WGC.DisplayTopologyRecovery.Requested` 结构化日志，记录恢复原因、重试令牌、
  拓扑状态与恢复窗口，便于后续硬件验收。

### 支持边界

- 本补丁仅恢复 HDMI 拓扑变化后的内置主屏 WGC 背景捕获，不扩大 0.2.0 的
  单主屏 SDR 支持合同。多显示器、混合 DPI/刷新率、跨适配器和完整热插拔矩阵
  仍为实验项或 `Not Run`。

## 0.2.0 - 2026-08-21

0.2.0 是首个不带 prerelease 后缀的正式版本。公共版本号从本版起严格使用
`MAJOR.MINOR.PATCH`；Windows 文件和 Package Identity 版本使用对应的
`MAJOR.MINOR.PATCH.0`。

### 当前能力

- Windows 10/11 x64、单主屏 SDR 下的原生点击、拖尾、圆环、碎片和 Bloom 特效。
- `background-aware`、`recording-compatible`、`light-background` 三种渲染模式，以及低配 `core` 模式。
- Host 与原生 Win32 Control Center 的本地配置、暂停/恢复、主题色、逐屏状态、帧率策略和日志清理。
- 标准版 Spout2/OBS v4 透明扩展预乘输出；Slim 版移除 Spout2 依赖和控制项，保留核心特效。
- 便携 ZIP 和目标机自签名单文件安装器；安装器支持安装、升级、卸载和保留 `data` 配置目录。

### 支持边界

- 正式测试范围是 Windows 10/11 x64、单主屏、SDR。便携版没有 Package Identity，不能承诺无边框 WGC。
- HDR、Advanced Color、多显示器、混合 DPI/刷新率、跨适配器、真实 device lost、ROI、外部录屏兼容性和
  Session-local WGC 仍是实验项或 `Not Run`，不属于 0.2.0 支持声明。
- `recording-compatible` 是用户主动选择的测试入口。Windows build `>=28000` 才允许尝试，失败时按
  `SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly` 回退；这不等于 Session-local WGC 已通过验收。

### 安装与升级

- 从 Alpha 版本升级时保留现有配置和 `data` 目录；配置 schema 不匹配继续采用当前安全回退策略，不覆盖原文件。
- 安装器使用目标机自签名证书，Windows SmartScreen 可能显示 `Unknown Publisher`。这是预期行为，Release 不提供
  可单独安装的证书、MSIX、私钥或 SDK 工具。
