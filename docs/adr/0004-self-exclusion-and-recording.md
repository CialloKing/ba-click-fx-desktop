# ADR-004: 自排除与录屏兼容模式

- Status: Proposed
- Decision owner: Product/Platform

## Context

背景感知若捕获自身 overlay 会形成反馈；窗口显示亲和性可以请求排除，但也可能让外部捕获看不到特效。
不同录屏器的捕获路径不同，任何一种窗口标志都不能保证通用兼容。

## Proposed decision

产品公开三个互斥模式，其中两种关闭背景传感器：

- `BackgroundAware`：启动 WGC，向 overlay 请求 `WDA_EXCLUDEFROMCAPTURE`，优先防止反馈。
- `RecordingCompatible`：关闭背景传感器并撤销 WDA，使用透明覆盖层、`visual-max`、`bright-core`、
  `0.90` Alpha 上限、`source-over` 和未知背景拟合，优先让录屏器看到 overlay。
- `LightBackground`：同样关闭背景传感器并撤销 WDA，使用 `0.85` Alpha 上限的浅色背景拟合。

模式切换必须是事务：先停止旧 sensor，再切换窗口策略，再创建新路径。请求失败要暴露诊断，但基础 FX
继续工作；如果 WGC 未能启动或会话随后停止，也必须撤销窗口排除，让 FX-only 回退保持可见。
产品文案使用“提高兼容性”，禁止使用“保证可录制”。

## Acceptance

- Spike B 至少用一种桌面捕获和一种窗口捕获路径记录行为。
- 自动化测试验证模式切换的顺序、幂等性和失败降级。
- UI/日志能够区分 WDA 请求值、API 返回值和实际外部录制观察。
