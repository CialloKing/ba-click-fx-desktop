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
`graphicsCaptureWithoutBorder` capability。因而无边框接口是可选增强：接口缺失或权限不足时
仍可启动实验 WGC，但必须把 `system-border=visible` 写入诊断，不能把它宣称为无边框捕获。
光标排除仍按配置作为独立能力探测；请求排除时若无法确认则关闭该捕获会话并继续 FX-only。
`Support.WGC=fallback-fx-only` 只表示捕获会话未能安全启动，不是 Host 启动失败。

背景合成只接受带有效时间戳、尺寸和自排除合同的帧；首个进入最终 pass 的样本会写入
`WGC background sample entered the final desktop composite`，以便区分会话启动和实际参与。

## Acceptance

- Spike B 覆盖授权成功/拒绝、边框开关、cursor 排除、ContentSize、session restart 和 self-exclusion。
- 旧系统日志明确显示 `consumer-throttled`，不报告 producer FPS 或捕获功耗下降。
- 压力测试证明 frame 全部释放，Close/Recreate 不与 drain 并发。
