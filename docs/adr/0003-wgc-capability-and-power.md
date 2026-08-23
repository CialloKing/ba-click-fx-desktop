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

产品渲染模式只有三项：`background-aware`（背景感知）、`recording-compatible`（录屏兼容测试）和
`light-background`（浅色背景优化）。只有 `background-aware` 启用 WGC；WGC 启动、排除或会话
失败时，当前可见批次回退到内部 FX-only coverage transport，不改变已保存的模式。
`recording-compatible` 与 `light-background` 都关闭 WGC；前者按 Web 透明覆盖层的 `visual-max`、
`bright-core`、`0.90` Alpha 上限、`source-over` 和未知背景合同拟合，后者使用同一策略并将 Alpha
上限收紧为 `0.85`。

`recording-compatible` 仅作为用户主动选择的外部录屏测试模式开放。Host 与 Control Center 共用
同一个运行时版本判定：版本探测成功且 `dwBuildNumber >= 28000` 才允许应用，低于该值或探测失败
均拒绝；不设未来 build 上限。允许尝试时，实际路径按
`SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly` 顺序回退；该模式的成功日志不能推导
WGC Session-local exclusion 已通过正式支持验收。

## Portable Alpha boundary

portable Win32 EXE 不具备 MSIX package identity，也不能通过外部清单授予
`graphicsCaptureWithoutBorder` capability。新生成的当前 schema 19 配置将
`background.allowSystemBorder` 默认为 `true`。schema 14 至 18 只通过配置层声明的固定迁移链升级，
其他版本仍被拒绝；迁移不得猜测或改写该字段。

允许系统边框时可以启动带 Windows 隐私提示的实验 WGC；可见状态记录为
`system-border=visible-allowed`，不能把它宣称为无边框捕获。用户可在 Control Center 中取消勾选
“允许黄色捕获边框”，即把 `background.allowSystemBorder` 设为 `false`。此时 Host 必须在
`StartCapture` 前请求并确认无边框会话；接口缺失、权限不足或系统仍要求边框时，必须放弃该会话并
继续使用内部 FX-only coverage transport，不能先启动再隐藏黄色系统捕获框。光标排除仍按配置作为
独立能力探测；请求排除时若无法确认则关闭该捕获会话并使用同一回退。
`Support.WGC=fallback-fx-only` 只表示捕获会话未能安全启动，不是 Host 启动失败。

授权请求不得同步等待 owner 线程。它作为 stop、WDA/profile 和 Session/FramePool 创建之前的独立事务
动作跨帧轮询，使用 `120 s` 有限截止时间；Pending 保持现有 capture/effective path，Host 继续处理输入、
呈现、IPC 和退出。配置、resize、device recovery 或 shutdown 覆盖请求时，所有者显式取消并用原控制
代次记录终态，随后只执行一次 FX-only 回滚。

背景合成只接受带有效时间戳、尺寸和自排除合同的帧；首个进入最终 pass 的样本会写入
`WGC background sample entered the final desktop composite`，以便区分会话启动和实际参与。

## Acceptance

- Spike B 覆盖授权成功/拒绝、边框开关、cursor 排除、ContentSize、session restart 和 self-exclusion。
- schema 19 默认允许系统边框；schema 14 至 18 的固定迁移不得改变已有边框选择，其他版本被拒绝。
- 关闭系统边框后，无边框能力不可用时于 `StartCapture` 前回退内部 FX-only coverage transport；
  允许系统边框的测试可观察到 `system-border=visible-allowed`。DComp overlay 没有浏览器 `Screen` API
  的逐像素等价物；捕获样本只在背景感知路径有效时参与合成。
- 旧系统日志明确显示 `consumer-throttled`，不报告 producer FPS 或捕获功耗下降。
- 压力测试证明 frame 全部释放，Close/Recreate 不与 drain 并发。
