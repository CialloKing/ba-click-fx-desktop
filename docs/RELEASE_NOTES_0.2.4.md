# ba-click-fx-desktop 0.2.4

0.2.4 是增加特效分层控制、Active-FX ROI 开关和特效 Profile 的正式补丁版
（非 prerelease）。

## 特效分层开关

高级参数新增“分层开关”页面，可以分别显示或隐藏中心圆盘、圆环、点击碎片、拖尾碎片、
拖尾线和 Bloom。分层开关只控制对应呈现层，不改变其他层的已配置参数；再次打开时会直接
恢复原参数效果。关闭 Bloom 会旁路整条 Bloom 金字塔，但不会移除仍启用的直接材质层。

总特效、点击特效和鼠标拖尾开关继续保留原有职责。分层开关是更细粒度的呈现控制，不把
这些入口合并成含义重叠的预设。

## Active-FX ROI

“显示与性能”页新增默认关闭的“Active-FX ROI（实验）”开关。当前生产路径只在纯特效 Bloom
的首级预滤波使用对齐 ROI；Bloom 后续 down/up、最终合成、WGC、Spout2 和逐显示器输出仍保持
全屏合同。

ROI 接触目标边界、覆盖面积过大、Bloom 关闭、Core 模式或可见范围计划不完整时，Host 会在
同一帧安全回退全屏预滤波。该开关已具备 WARP 像素等价回归，但尚不构成真实显卡性能、边界像素、
HDR、多显示器或跨适配器支持声明。

## 特效 Profile

Control Center 基础页新增 Host-owned、effects-only Profile 管理。Host 固定提供“Unity 原版”、
“轻量”、“纯点击”和“纯拖尾”四个不可覆盖、不可删除的内置项；用户可以按名称保存、应用、
覆盖或删除自定义 Profile。

每个自定义项分别保存在主配置旁的 `fx-profiles/<名称>.json`，只包含完整的 `EffectsConfig`。
Profile 不保存也不修改背景、逐屏设置、输入、性能、系统或 Active-FX ROI。保存使用临时文件、
flush 和原子替换；损坏、冲突、重复或无法读取的文件会被跳过并在控制中心提示，不会阻断内置项。

Host 现在分别维护控制 generation 和渲染配置 generation。保存或删除 Profile 只推进控制状态，
不会因为目录变化而重新应用渲染或取消捕获事务；应用 Profile 才会原子写入主配置并推进渲染代次。

## 配置与升级

当前主配置 schema 为 19。从 0.2.3 的 schema 17 升级时，固定迁移链会把六个分层开关初始化为
全开，并把 Active-FX ROI 初始化为关闭，因此默认视觉和性能路径保持不变。schema 14 至 18 只按
该固定链迁移；其他版本、未知字段和枚举别名仍会被拒绝。

从 0.2.0 至 0.2.3 升级会保留 `data`、主配置、日志和显示器 override。`fx-profiles` 是 0.2.4
新增的独立目录。若降级到 0.2.3，旧版会拒绝 schema 19 主配置并使用内存默认值；降级前应自行
备份配置和自定义 Profile。

## 下载哪个包

本次 Release 只发布带 Spout2 的完整标准版：

- `ba-click-fx-desktop-0.2.4-Portable-windows-x64.zip`
- `ba-click-fx-desktop-0.2.4-Portable-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.4-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.4-setup-windows-x64.exe.sha256`

Slim 版继续保留源码构建与兼容验证入口，但不发布预编译 Release 资产。便携版没有 Package Identity，
因此不承诺无边框 WGC。安装器目前没有公开代码签名，SmartScreen 显示 `Unknown Publisher` 属于预期。

## 发布验证

2026-08-23 的正式发布验证结果：

- Full `release-verify`：`40/40` 通过。
- Slim `slim-release-verify`：`39/39` 通过。
- 便携包和安装器均通过包结构、PE 依赖与版本合同检查；Host、Control Center 和
  安装器的文件/产品版本均为 `0.2.4`。
- 便携 ZIP SHA-256：`64AF766DDE2D56D692D0BA4D1BAA82AE601C947E1AFA16F583C24C24B581872D`。
- 安装器 SHA-256：`A867EC24A8B17B9669DDD235A64DBE30EC988082C08996C5F0CE9892361190B3`。

## 支持边界

本版新增的是用户可见控制面和受限预滤波优化，不扩大硬件支持范围。HDR/Advanced Color、
多显示器、混合 DPI/刷新率、跨适配器、真实 GPU device lost、无边框 WGC 跨版本稳定性、
完整热插拔矩阵和 Active-FX ROI 硬件矩阵仍为实验项或 `Not Run`。
