# ADR-003: WGC 能力探测与功耗状态

- Status: Proposed
- Decision owner: Capture/Platform

## Context

WGC 是可选背景传感器。`CreateFreeThreaded` 消除 DispatcherQueue 依赖，但回调来自内部 worker；
Agile/Threading(Both) 不保证方法并发安全。`MinUpdateInterval` 只在 build 26100 及以上可用，旧系统
降低 consumer 频率不能视为 producer 节流。WGC 没有 Pause API。

## Proposed decision

- `FrameArrived` 只递增 generation 并置 event。
- Render Owner 串行 drain、保留最新帧、检查 ContentSize、复制到自有纹理并关闭所有 frame。
- Recreate、Close、退订、resize 与 device-lost 进入同一 capture state machine。
- HOT 正常消费；WARM 在 26100+ 设置 MinUpdateInterval，旧系统仅降低消费侧工作；COLD 关闭并在恢复时重建。
- API presence、contract version、border/cursor capability 都运行时探测。

## Portable Alpha boundary

portable Win32 EXE 不具备 MSIX package identity，也不能通过外部清单授予
`graphicsCaptureWithoutBorder` capability。schema 4 将 `background.allowSystemBorder` 默认为
`false`：Host 必须在 `StartCapture` 前请求并确认无边框会话；接口缺失、权限不足或系统仍要求边框时，
必须放弃该会话并继续 FX-only，不能先启动再隐藏黄色系统捕获框。

只有用户在 Control Center 中显式启用“允许黄色捕获边框”，即把
`background.allowSystemBorder` 设为 `true` 后，才允许带可见系统边框的实验 WGC，并将其记录为
`system-border=visible-allowed`，不能把它宣称为无边框捕获。光标排除仍按配置作为独立能力探测；
请求排除时若无法确认则关闭该捕获会话并继续 FX-only。`Support.WGC=fallback-fx-only` 只表示捕获
会话未能安全启动，不是 Host 启动失败。

背景合成只接受带有效时间戳、尺寸和自排除合同的帧；首个进入最终 pass 的样本会写入
`WGC background sample entered the final desktop composite`，以便区分会话启动和实际参与。

## Acceptance

- Spike B 覆盖授权成功/拒绝、边框开关、cursor 排除、ContentSize、session restart 和 self-exclusion。
- 默认配置在无边框能力不可用时于 `StartCapture` 前回退 FX-only；只有显式授权测试才允许观察到
  `system-border=visible-allowed`。
- 旧系统日志明确显示 `consumer-throttled`，不报告 producer FPS 或捕获功耗下降。
- 压力测试证明 frame 全部释放，Close/Recreate 不与 drain 并发。
