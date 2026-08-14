# P0 配对渲染性能基线 / 2026-08-15

- 场景：`p0-static-click-message-pressure-v3`
- 采集提交：`c87c83ae566b874e976c7c95230f58c0487ad31d`
- 报告器提交：`63cb7ab2fb6f3d294027137f5df9012dc5fcfc1d`
- 系统：Windows `10.0.19045`，主屏 `3840x2160 @ 170 Hz`，DPI `144`
- GPU/驱动：NVIDIA GeForce RTX 4060 Laptop GPU，`32.0.16.1074`
- D3D11：Hardware，fallback `none`
- 结果：**通过配对渲染基线门禁**

采集使用同一份 SHA-256 为
`12B748478FDEFAB043B18BDD2DC3250881327D4E8B374E4D7C9A031DFDEF3D13`
的 Host。运行时产品字段仍为 `0.1.0-alpha.12`；采集身份以完整 Git 提交和 EXE 哈希为准，
不能据此宣称已发布包含该提交的 `alpha.12` Release。

## 场景

两个模式各运行 `10.5 s`，先等待 `50 ms` 让 WGC 取得样本，再固定渲染一次 `130 ms`
点击时间片。采集期间每 `25 ms` 投递一批 5 条无害线程消息，共 705 条，用于验证消息唤醒
不会绕过交换链帧槽节流。Raw Input 注册被显式关闭，避免操作者输入污染配对渲染成本。

原始采集命令和每个模式的进程退出状态位于 `capture.json`。本目录不提交两份重复 EXE；
`capture.json` 保留原始 EXE 哈希，两份日志、配置和报告由 Git 固定，`SHA256SUMS` 固定其字节内容。

## 结果

- FX-only：`166.643 FPS`；background-aware：`165.832 FPS`。
- 两个模式的 `GPU.PendingFrames.Max` 都为 `1`，无节流超时、失败、预算耗尽或 GPU 查询错误。
- background-aware 的 WGC accepted FPS 为 `164.433`，样本年龄 p95 为 `186 us`。
- WGC drain/copy 与背景快照分别为 `251 us`、`256 us` p95。
- Bloom/final p95 从 `2392 us` 增至 `2883 us`，增加 `491 us / 20.527%`。
- GPU command span p95 从 `2453 us` 增至 `3428 us`，增加 `975 us / 39.747%`。
- Present p95 从 `346 us` 降至 `93 us`，稳定区间的主要新增成本不在 Present；但最大值
  从 `5661 us` 增至 `12913 us`，保留 `+7252 us` 尾部风险记录。

`summary.json` 保存结构化判定，`summary.md` 保存人工可读报告。最大增量只在所列 WGC、
background snapshot 和 Bloom p95 分项中比较；不同百分位样本不能相加，也不能作为逐帧因果证明。

## 限制

- 本场景关闭 Raw Input，因此没有验证受控 Down 边沿、Win32 输入队列年龄或
  input-to-Present-return；这些仍是 P0 未完成项。
- 本结果只覆盖本机单主屏 SDR，不接受 HDR、多显示器、外部录屏或 device-lost 矩阵。
- 固定 `130 ms` 点击时间片用于配对渲染成本，不是完整交互或动画时序验收。
- 报告中的 Present 是 API 返回耗时，不是 DWM、扫描输出、面板或光子延迟。
