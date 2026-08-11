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

产品渲染模式只有三项：`background-aware`（背景感知）、`classic`（贴近原版）和
`light-background`（浅色背景优化）。只有 `background-aware` 启用 WGC；WGC 启动、排除或会话
失败时，当前可见批次回退到 `classic` 的 FX-only coverage transport。`classic` 与
`light-background` 都关闭 WGC；后者使用 `visual-max`、`bright-core`，并将桌面 source-over Alpha
限制为 `0.85`。

## Portable Alpha boundary

portable Win32 EXE 不具备 MSIX package identity，也不能通过外部清单授予
`graphicsCaptureWithoutBorder` capability。新生成的 schema 7 配置将
`background.allowSystemBorder` 默认为 `true`，schema 1/2/3 迁移到当前版本时采用该值；schema 4
迁移时缺失字段也使用该默认值，使旧系统仍可使用背景感知路径。schema 4 已显式保存的 `false` 是
用户选择，迁移时必须保留。

允许系统边框时可以启动带 Windows 隐私提示的实验 WGC；可见状态记录为
`system-border=visible-allowed`，不能把它宣称为无边框捕获。用户可在 Control Center 中取消勾选
“允许黄色捕获边框”，即把 `background.allowSystemBorder` 设为 `false`。此时 Host 必须在
`StartCapture` 前请求并确认无边框会话；接口缺失、权限不足或系统仍要求边框时，必须放弃该会话并
继续 Classic（FX-only coverage transport），不能先启动再隐藏黄色系统捕获框。光标排除仍按配置作为独立
能力探测；请求排除时若无法确认则关闭该捕获会话并继续 Classic。`Support.WGC=fallback-fx-only` 只表示捕获会话未能安全
启动，不是 Host 启动失败。

背景合成只接受带有效时间戳、尺寸和自排除合同的帧；首个进入最终 pass 的样本会写入
`WGC background sample entered the final desktop composite`，以便区分会话启动和实际参与。

## Acceptance

- Spike B 覆盖授权成功/拒绝、边框开关、cursor 排除、ContentSize、session restart 和 self-exclusion。
- 新配置和 schema 1/2/3 迁移默认允许系统边框；schema 4 迁移时缺失字段采用该默认值，显式保存的
  `false` 必须保留。
- 关闭系统边框后，无边框能力不可用时于 `StartCapture` 前回退 Classic（FX-only coverage transport）；
  允许系统边框的测试可观察到 `system-border=visible-allowed`。DComp overlay 没有浏览器 `Screen` API
  的逐像素等价物；捕获样本只在背景感知路径有效时参与合成。
- 旧系统日志明确显示 `consumer-throttled`，不报告 producer FPS 或捕获功耗下降。
- 压力测试证明 frame 全部释放，Close/Recreate 不与 drain 并发。
