# ba-click-fx-desktop 0.2.9（未发布）

0.2.9 是 OBS/Spout2 特效输出回归修复补丁。它恢复 0.2.2 的特效亮度与可见度，不改变帧率策略、
空闲资源优化、WGC 或 Spout2 SDK 发送生命周期。

## OBS 输出修复

- 0.2.3 将 FX-only 的线性特效能量从逐通道 sRGB 编码改为共享峰值 SDR rolloff。该映射会显著压低
  低能量字节值，并同时影响圆盘、Bloom、圆环、碎片和拖尾。
- 0.2.9 恢复 `LinearToSrgb(max(E, 0))`；超过 1 的通道继续由 BGRA8 目标饱和。完整渲染与核心性能
  模式使用同一映射。
- Cross2 coverage、纯加法层的 `1/255` Alpha 步进、空闲透明帧及固定尺寸共享句柄行为保持不变。

## 输出合同与兼容性

- 输出合同升级为 `bgra8-srgb-extended-premultiplied-fx-only-v6`。Host 状态、进程边界探针、OBS runner
  和三个证据验证器使用同一合同标识；混合版本 Host/Control Center 继续 fail-closed。
- OBS 仍需使用 `Premultiplied Alpha` Composite Mode，并将来源的 Blending Method 保持为 `Default`、
  Blending Mode 保持为 `Normal`。已经正确配置的场景无需迁移或重建来源。
- 产品版本提升到 0.2.9，主配置 schema 保持 19。现有 `BAFX.config.json`、显示器 override、`data`
  目录和 effects-only `fx-profiles` 不需要迁移、删除或重建。

## 验证结果

- Full `cmake --workflow --preset release-verify`：`44/44` 通过；Slim
  `cmake --workflow --preset slim-release-verify`：`43/43` 通过。帧率和 idle policy 共用的
  `desktop_input_dispatch` 单独重跑 `91/91` 通过。
- WARP 覆盖完整与核心路径的固定输入 sRGB 字节值、各特效层可见性、扩展预乘 Alpha、空闲透明和
  Bloom 圆盘外输出；WARP 结果不替代真实 OBS 证据。
- OBS `32.2.2`、win-spout `1.12.0` 下，隔离 Profile 的 `FixedComposite` 与 `DynamicLifecycle`
  均通过。证据分别保存到 `artifacts/obs-v6-fixed-20260904-011900` 和
  `artifacts/obs-v6-lifecycle-20260904-012025`；包含 raw BGRA、截图及无音轨录像，且 OBS 配置恢复
  前后完全一致。
- 其他 OBS/插件版本、HDR、多显示器、跨 GPU 和 Windows 11 仍为 `Not Run`，不得从本机结果外推。
- 本说明对应未发布源码候选，不创建标签、GitHub Release 或发布资产。
