# ADR-004: 自排除与录屏兼容模式

- Status: Proposed
- Decision owner: Product/Platform

## Context

背景感知若捕获自身 overlay 会形成反馈；窗口显示亲和性可以请求排除，但也可能让外部捕获看不到特效。
不同录屏器的捕获路径不同，任何一种窗口标志都不能保证通用兼容。

## Proposed decision

产品公开三个互斥模式，默认路径保持稳定，测试模式可显式启用：

- `BackgroundAware`：启动 WGC，向 overlay 请求 `WDA_EXCLUDEFROMCAPTURE`，优先防止反馈。
- `RecordingCompatible`：用户主动选择的实验测试模式。保持 `WDA_NONE`，使用透明覆盖层、
  `RecordingCompatible` profile，并在自身 WGC Session 上设置 WindowId exclusion；内部背景采样不包含
  overlay，而外部截图/录屏不继承该排除。Session-local 启动或运行失败时，显式回退旧的
  `WDA_EXCLUDEFROMCAPTURE` 路径，再失败才进入 FX-only。`0.90` Alpha 上限、`visual-max`、`bright-core`、
  `source-over` 和未知背景拟合仍是测试模式的视觉合同。
- `LightBackground`：同样关闭背景传感器并撤销 WDA，使用 `0.85` Alpha 上限的浅色背景拟合。

`RecordingCompatible` 是用户主动选择的录屏兼容测试模式，不是默认路径，也不代表 WGC Session-local
排除已经可用。Control Center 显示为“录屏兼容（测试，仅 Windows 11 26H1 及以后）”，wire value
仍为 `recording-compatible`。运行时只接受版本探测成功且 `OS build >= 28000`；不支持或无法探测时，
UI 不发送 IPC，Host 也拒绝直接请求。旧配置在启动时回退并原子持久化为 `light-background`，保存失败
仍以内存中的安全模式运行；未来 build 不设置上限，自动纳入测试资格。`LightBackground` 的 `0.85`
Alpha 合同保持不变，测试模式的 `0.90` 仅用于外部录屏观察。

新版 WGC Session 专属 WindowId 排除已经接入 `RecordingCompatible` 测试模式，但不是默认路径，也不
代表当前机器或所有目标系统已经具备该能力。实际路径固定为：

```text
SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly
```

独立 Spike 与测试模式都在 `WDA_NONE` 下探测
`IDisplayGraphicsCaptureSession::SetWindowExclusionList` / `GetWindowExclusionList`，不修改产品默认路径，
不移除现有 `WDA_EXCLUDEFROMCAPTURE`。测试模式接入现有 `WgcBackgroundSensor`，但不改变默认
`BackgroundAware` 的旧 WDA 路径。Overlay hide/show 和基于上一帧 FX 的反解不作为候选方案。

模式切换必须是事务：先停止旧 sensor，再切换窗口策略，再创建新路径。请求失败要暴露诊断，但基础 FX
继续工作；如果 WGC 未能启动或会话随后停止，也必须撤销窗口排除，让 FX-only 回退保持可见。
产品文案使用“提高兼容性”，禁止使用“保证可录制”。

## Acceptance

- Spike B 至少用一种桌面捕获和一种窗口捕获路径记录行为。
- Session 专属排除 Spike 必须在真实目标系统得到 `capability.status=Available` 和
  `evidence.result=Passed`：Set/Get WindowId 往返、configuration iteration 与 frame 对应、
  baseline/excluded/restored 三阶段 FP16 像素、远端 control ROI 和资源清理必须全部通过。调用成功但
  iteration 或像素证据不足只能记为 `NotVerified`。
- 外部录屏/OBS、HDR、多显示器、device lost 和 packaged 权限必须作为独立矩阵证据；未执行项保持
  `Not Run`，不能由离线合同或单机 API 成功替代。
- 测试模式的 Session-local 路径已经遵守该事务：Overlay 使用 `WDA_NONE`，Session 创建后设置排除列表，
  在收到对应 configuration iteration 的 frame 前不发布新的 `BackgroundSnapshot`。只有 Spike 在真实目标
  系统达到 `Available + Passed` 并补齐硬件矩阵后，才评审是否把该路径提升为默认 `BackgroundAware`；
  诊断必须区分三条实际路径。
- 自动化测试验证模式切换的顺序、幂等性和失败降级。
- UI/日志能够区分 WDA 请求值、API 返回值和实际外部录制观察。
