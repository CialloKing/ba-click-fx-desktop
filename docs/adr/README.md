# Architecture Decision Records

ADR 记录架构选择、替代方案和证据，不记录实现进度。状态只使用 `Proposed`、`Accepted`、
`Superseded` 或 `Rejected`。ADR 变为 Accepted 必须有独立评审提交；相关 Spike 的主假设可以失败，
但 fallback 必须写入 ADR 和能力矩阵。

| ADR | 主题 | 状态 | 主要证据 |
| --- | --- | --- | --- |
| 001 | Final Composition 与层顺序 | Proposed | SPK-001, SPK-003, Unity Golden |
| 002 | 强度语义与输出映射 | Proposed | SPK-003, VAL-COLOR |
| 003 | WGC 能力、生命周期与功耗 | Proposed | SPK-002, SPK-004 |
| 004 | 自排除与录屏兼容 | Proposed | SPK-002, VAL-RECORDING |
| 005 | Golden Oracle 与数值比较 | Proposed | 全部验证层级 |
| 006 | ROI、guard 与 mip 相位 | Proposed | VAL-ROI；未接受时走 full-screen |
| 007 | 背景时间有效性 | Proposed | SPK-002, SPK-004, VAL-TEMPORAL |
| 009 | 方案 C 本机身份安装通道 | Proposed | Identity Spike, SPK-002 |

最近评审：2026-08-09。
