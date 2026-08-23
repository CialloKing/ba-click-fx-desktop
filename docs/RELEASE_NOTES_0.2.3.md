# ba-click-fx-desktop 0.2.3

0.2.3 是修复 OBS 光晕过曝并降低原生 Host GPU 占用的正式补丁版
（非 prerelease）。

## GPU 与帧率优化

旧版的 `match-display` 没有设置额外最小帧周期；DXGI frame-latency 对象只能提供 GPU
背压，不能保证提交速率等于显示器刷新率。同时，启用 Spout2 后空闲策略会无条件继续完整的
全分辨率 FX、Bloom 和 Present，因此没有活动特效时仍会持续占用 GPU。

本版让 `match-display` 使用目标 DisplayConfig 刷新率的精确分数计算帧周期，刷新率缺失或
无效时保守回退到 60 FPS。控制中心新增“无限制 FPS”，对应 wire value `unlimited`，只有该
模式保留旧版不设置额外最小帧周期的行为；`core` 模式仍固定为 60 FPS。

启用空闲资源优化时，Spout2 会在活动特效结束后再提交一帧透明清屏，随后停止完整渲染，
仅以低频重发最终纹理维持发送者和共享句柄。新的输入、特效或渲染失效仍会立即唤醒渲染。

## OBS 光晕

Spout2/OBS v5 输出不再把独立 Bloom 做 sRGB 提亮后直接叠加到编码游戏画面。发送端改用
共享峰值的保色相 SDR rolloff，避免光晕暗部被放大和中心高光过早削顶，同时保留 v4 的
扩展预乘 Alpha、圆盘 coverage 以及圆环、碎片、拖尾和 Bloom 的加法能量合同。

OBS 仍应使用 `Premultiplied Alpha`、`Default` 和 `Normal`；不要切换到 `Add` 或添加
`sRGB Off` 滤镜。

## 本机验证

Windows 10 `19045`、RTX 4060 Laptop GPU、3840x2160、170 Hz 环境下，使用同一条
Spout2 空闲命令串行比较正式版 0.2.2 与 0.2.3 候选版：

- 0.2.2 在首个 10 秒窗口提交 13214 帧，`PresentedFps=1321.043`；10 个一秒采样的
  进程 GPU Engine 利用率总和均值为 `47.907%`。
- 0.2.3 在对应窗口只提交初始化透明帧，`Window.FrameCount=1`、
  `PresentedFps=0.100`；GPU Engine 利用率总和均值为 `0.669%`，下降约 `98.6%`。
- 持续渲染探针读取到目标刷新率 `170/1 Hz`，两个窗口分别为 `169.488 FPS` 和
  `169.669 FPS`。
- Spout2 独立进程生命周期测试通过，覆盖空闲透明、点击/拖尾活动层和衰减后恢复透明，
  发送器状态保持 `sent`。

GPU 百分比是该机器同次串行 A/B 的 Windows GPU Engine 计数器总和，不是对其他分辨率、
刷新率、显卡或 Bloom 配置的固定承诺。

## 发布验证

- 标准版 `release-verify` 通过全部 `40/40` 项测试，其中包含真实 Spout2 进程边界测试；
  Slim 源码构建的 `slim-release-verify` 通过 `39/39` 项测试。
- 便携 ZIP SHA-256 为 `F606EEBCC4967C985E4D1EF4C00C7F71B86259F8887C2C0AC8AA80A526462233`。
- 安装器 SHA-256 为 `CC2E8D7C618646CB5363A0E17EB50B29103061734B960E74B68534E5ADDDEFCA`。

## 下载哪个包

本次 Release 只发布带 Spout2 的完整标准版：

- `ba-click-fx-desktop-0.2.3-Portable-windows-x64.zip`
- `ba-click-fx-desktop-0.2.3-Portable-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.3-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.3-setup-windows-x64.exe.sha256`

Slim 版继续保留源码构建入口，但不发布预编译 Release 资产。便携版没有 Package Identity，
因此不承诺无边框 WGC。

## 安装、升级和支持边界

从 0.2.0、0.2.1 或 0.2.2 升级会保留现有配置和 `data` 目录，配置 schema 仍为 17。
若在 0.2.3 中保存 `unlimited` 后降级，旧版会按严格枚举合同拒绝该配置并使用内存默认值；
降级前应改回 `match-display` 或固定帧率。

本版不扩大硬件支持范围。HDR/Advanced Color、多显示器、混合 DPI/刷新率、跨适配器、
ROI、真实 GPU device lost、无边框 WGC 跨版本稳定性和完整热插拔矩阵仍为实验项或
`Not Run`。
