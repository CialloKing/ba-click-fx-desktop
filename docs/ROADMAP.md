# 开发路线图

本路线图规定开发顺序；[VALIDATION.md](VALIDATION.md) 规定证据合同，
[SPIKES.md](SPIKES.md) 规定发布前的硬件/API 验收。自 2026-08-14 起，当前迭代优先解决
用户可感知的输入延迟、渲染成本和视觉差异，不再按 Spike 编号顺序扩张采集工具。

完成新的 collector、verifier 或证据归档，只计作验证基础设施进展，不能单独计作用户功能更新。
它们只有在解除当前体验问题或正式发布门槛时才进入主线排期。

## P0：输入、渲染与 Present 延迟诊断

先建立可复现的性能基线，再改变 WGC 或渲染路径。诊断必须低开销、可聚合，并同时覆盖：

- Raw Input 队列年龄、待处理消息量、Move 收敛量和按钮边沿到消费时刻；
- WGC producer/accepted FPS、被合并或拒绝的样本数、样本年龄和 `drainLatest` CPU 耗时；
- 背景 FP16 copy 与 Bloom 的 GPU 耗时，使用异步 D3D11 timestamp query，不把 CPU 提交耗时
  冒充 GPU 执行耗时；
- `Present(1, 0)` 的调用阻塞时间，以及输入边沿到对应 `Present` 返回的相关耗时；
- 渲染模式、adapter、driver、显示模式、降级原因、WGC 生命周期和资源账本等排障上下文。

日志默认输出固定时间窗的计数、FPS、`p50/p95/p99/max` 与有速率限制的慢帧样本，不逐帧刷盘。
启动、模式切换、设备重建、fallback 和失败仍作为独立事件记录。诊断本身需要记录采样开销和丢失计数，
避免测量行为掩盖原问题。

`input-to-Present-return` 只表示应用收到输入后完成本次 Present 调用的时间，不是真实上屏延迟。
物理显示还包含 DWM、显示队列、扫描输出和面板响应；在取得 ETW/PresentMon 类证据或外部高速测量前，
报告必须使用 `input-to-Present-return` 这一准确名称，不能写成“输入到显示”或“上屏延迟”。

本阶段交付为一份可复跑的基线报告，至少分别覆盖 FX-only 与 `background-aware`，并能回答延迟来自
输入积压、WGC、copy、Bloom 还是 Present。没有基线数据时，不凭主观卡顿直接选择优化点。

当前配对采集入口先给 WGC `50 ms` acquire 预热，再固定一个 `130 ms` 点击时间片，并在同一完整
10 秒性能窗内每 `25 ms` 投递一批 5 条无害线程消息：

```powershell
pwsh -NoProfile -File tools/collect-performance-baseline.ps1 `
  -Executable build/alpha-x64/src/desktop/Release/ba-click-fx-desktop.exe `
  -OutputDirectory artifacts/local/performance-baseline-<timestamp>
python -B tools/report-performance-baseline.py `
  artifacts/local/performance-baseline-<timestamp>
```

采集器拒绝覆盖目录、已有 Host、脏工作树和无界进程等待；该诊断场景关闭 Raw Input 注册以隔离
操作者活动。报告器要求 Raw Input 为零，并校验同一 HEAD/EXE、配置差异、WGC 参与、GPU 样本
覆盖率、丢样计数、资源账本与帧节流上限。

## P1：WGC 成本优化与 guard-band ROI

基线确认瓶颈后，按下列顺序降低背景感知路径成本：

1. 没有可见特效时保留必要的会话状态，但跳过不参与画面的背景 drain/copy；重新进入活跃状态时先取得
   新鲜样本，不能复用过期桌面快照。
2. 仅在特效存活期间处理背景，并验证暂停、模式切换和 WGC frame event 不会留下静态旧背景。
3. 以活跃特效包围盒为基础加入 Bloom 半径、滤波核、移动余量和 mip 对齐所需的 guard band，建立
   局部 ROI 原型；边缘、尺寸或资源状态不满足合同时必须回退全屏路径。
4. 使用相同场景比较全屏与 ROI 的 GPU 时间、copy 带宽、Present 阻塞和 FP16 像素结果；不能只报告
   CPU 提交时间或资源尺寸。

本阶段交付为 active-FX-only WGC 处理和 guard-band ROI 原型，以及相对全屏基线的成本与像素差异报告。
ROI 数值合同继续服从 ADR-006；原型通过前，全屏仍是生产 fallback。

## P2：Unity/Web/Native 固定时间片视觉回归

使用相同 viewport、配置、输入轨迹和背景，对 Unity、Web 与 Native 执行一次面向可见差异的回归：

- 点击固定 `50/100/120/250/450 ms` 时间片；
- 拖拽固定采样点、按下/保持/释放边界和释放后寿命；
- 比较总动画时长、粒子包络与径向能量、拖尾几何/材质/生命周期、圆环以及三角碎片 Bloom；
- Native 继续核对 FP16 中间层；Web 与 Unity 差异先单独记录，再以 Unity/游戏证据作为视觉真值；
- 每个可见差异必须有截图或数值证据、原因归类和处理结论，不能用新增测试数量代替结果。

本阶段交付为三端固定时间片差异报告和针对已确认差异的修正。随机流尚未等价的部分继续使用统计与
包络合同，不把不同随机布局误报成动画时序回归。

## P3：正式发布硬件验收

输入跟手、WGC 成本和视觉回归收敛后，再集中完成以下发布矩阵：

- WGC 权限、无边框、外部录屏、模式切换、压力和功耗；
- HDR/Advanced Color 输出与 SDR fallback；
- 多显示器、混合刷新率、DPI、多适配器、热插拔和 device lost。

这些单元格仍是相关 ADR 接受和正式支持声明的硬门槛。未执行项保持 `Not Run`，相关 ADR 保持
`Proposed`；优先级后移不等于放宽证据，也不允许用模拟结果代替真实硬件结果。除非发现会阻塞 P0-P2
或造成数据/资源安全问题的缺陷，当前迭代不继续扩张独立硬件 collector/verifier。

## 执行规则

- 每项小工作单独使用中文提交，并在提交前执行与改动范围相称的有界验证。
- 构建、测试、实机采集和外部工具都必须设置硬超时；超时后保存阶段与诊断，不做无界等待。
- 同一失败最多进行一次有新证据支撑的复跑；结果不变时先收敛根因或调整方案，避免盲目循环。
- 性能优化必须同时给出优化前后同场景数据；视觉修改必须同时给出固定时间片回归结果。
- 路线图进度按用户可感知结果、可复跑报告和已解释差异统计，不按新增日志行、collector 或 verifier 数量统计。
