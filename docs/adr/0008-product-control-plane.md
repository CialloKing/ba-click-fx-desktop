# ADR-0008：Host 产品控制面

- 状态：**Proposed**
- 日期：2026-08-10

## 背景

当前可执行文件已经具备输入、D3D11/DirectComposition、三种渲染模式和可选 WGC
背景采样，但配置和运行时控制仍然只存在于进程启动参数中。下一阶段需要让 Host 能够
长期运行，并允许独立的控制界面在不接触渲染线程的情况下读取状态、修改配置和请求退出。

## 决策

1. 配置由 `bafx_config` 持有，使用版本化 JSON（当前 schema 为 17）。读取时只接受完整的当前
   schema，不迁移非当前文件，也不接受未知字段或枚举别名；校验后生成不可变的运行时快照。写入使用
   同目录临时文件、flush、替换的原子流程。
2. Host 是配置的唯一写入者。外部客户端只能通过版本化的本地 Named Pipe 请求操作，不能
   取得 Renderer 或 D3D11 immediate context 的句柄。
3. IPC 使用 UTF-8、以换行分隔的请求/响应记录。请求是一个命令 token，可选地跟随一个
   JSON 负载；响应以 `OK` 或 `ERR <code> <message>` 开头。未知命令、格式错误、
   NUL/换行注入和超限请求都返回可诊断错误而不终止 Host。
4. Host 通过用户范围的命名互斥体保证单实例；管道服务在独立线程运行，Render Owner 只
   在帧边界消费已校验的命令。Control Center 退出不会影响 Host。
5. 基础配置协议保留 `GetState`、`GetDisplayState`、`GetConfig`、`SetConfig <schema-17-json>`、
   `SetConfig {generation,path,value}`、`Pause`、`Resume` 和 `Shutdown`。路径更新只允许
   配置库声明的产品字段，并在 generation 不匹配时返回冲突。响应中的 `generation` 用于
   客户端判断快照是否变化；`GetDisplayState` 固定使用严格 schema 2 和独立运行状态代次，同时报告
   配置/应用代次、全局拓扑状态、权威离线 override，以及逐屏来源身份、物理 cadence、颜色查询、
   SDR white level、GPU、已应用特效/HDR/帧率策略、请求/解析/实际输出、fallback、WGC 和故障状态。
   旧 schema、未知、重复或缺失字段均被拒绝，不增加兼容别名；未知能力保持 `null` 或 `unknown`，不能从
   配置请求推导。Preset/Profile 等更高层功能在此协议稳定后再增加。
6. `background.mode` 的产品 wire values 与 Control Center 显示名固定如下：
   `background-aware`（背景感知）、`recording-compatible`（录屏兼容测试）和
   `light-background`（浅色背景优化）。只有背景感知启用 WGC；WGC 失败时回退内部 FX-only
   coverage transport。RecordingCompatible 按 Web 透明覆盖层的 `visual-max` + `bright-core`、
   `0.90` Alpha 上限、`source-over` 和未知背景合同拟合；LightBackground 使用同一策略，但将 Alpha
   上限收紧为 `0.85`。后两者关闭 WGC。

   | Control Center 显示名 | `background.mode` wire value | WGC |
   | --- | --- | --- |
   | 背景感知 | `background-aware` | 启用，失败回退内部 FX-only |
   | 录屏兼容（测试，仅 Windows 11 26H1 及以后） | `recording-compatible` | 关闭 |
   | 浅色背景优化 | `light-background` | 关闭 |

   `recording-compatible` 只有在版本探测成功且 OS build 不低于 `28000` 时才可应用。该门槛只有下限，
   不为未来 Windows build 设置上限；版本探测失败、旧 build 或启动时发现已保存的测试模式时，Host
   拒绝或回退到 `light-background`，并在诊断日志中记录 requested/effective mode、原因、FX-only
   路径、WGC disabled 和 Alpha 上限。满足门槛时按
   `SessionLocalExclusion -> LegacyGlobalExclusion -> FxOnly` 顺序执行；该测试只评估透明覆盖层的
   外部录屏表现，不证明 Session-local exclusion 的生产能力。

7. `background.allowSystemBorder` 默认为 `true`。Control Center 通过复选框更新该字段；
   用户取消勾选后，Host 必须在 `StartCapture` 前确认无边框 WGC 能力，否则回退内部 FX-only。
   DComp overlay 没有浏览器
   `Screen` API 的逐像素等价物，控制面不得宣称三种模式都能逐像素复现桌面。
8. Control Center 提供默认关闭的“拖尾常驻”复选框，并反向映射到配置字段
   `input.trailOnlyWhilePressed`。这是桌面版原生产品增强；开启后，未按键的 Raw Input Move 使用独立的
   纯拖尾实例，首个样本只建立锚点，不能生成点击圆盘、圆环或点击 burst。真实按下、出界、暂停、
   关闭拖尾或关闭总特效会结束当前常驻段，已有几何按原生命周期衰减，下一段不得与旧坐标建立假连接。
9. Control Center 提供 `input.samplingRateHz` 滑块。Host 在每次输入消费/呈现更新中为按压 FX 锁存一份
   帧边界当前位置，普通路径按 Down→Held→Up 处理，并让本轮模拟动作共享 `renderTime`；普通 Up-only
   释放帧不提交 Held Move。`0` 表示不额外限频，整数 `1..1000` 只使用消息分派 QPC 推进可选位置采样
   相位，不丢弃 Raw 边沿或归约后的帧态，也不改变模拟时间。切换采样率会清空旧相位，使下一位置样本
   立即建立新相位。含任一边沿的帧不得从尾随 Move 重启常驻段。Raw Input 同帧多边沿按原序无损保留，
   仅用于诊断和原生扩展；严格效果路径将其归约为 Down/Held/Up 布尔帧态并按 Down→Held→Up 执行，
   Cancel 最后作为原生硬边界。
   Unity `2021.3.45f1` Player 已确认 `Down-Up-Down` 的聚合帧三态同时为 true；其他边沿排列及游戏所用
   Unity `2021.3.56f2` 仍未验证。`30 Hz` 只作为手机客户端视觉近似的人工审核建议，不能宣称为游戏固定参数。
10. Control Center 的高级页包含“时间与透明度”“粒子与材质”“圆环参数”“点击碎片”“Bloom 参数”
    和“分层开关”六个二级页面。分层开关分别控制中心圆盘、圆环、点击碎片、拖尾碎片、拖尾线和
    Bloom；这些开关只过滤呈现，不因热切换重建仍在寿命内的模拟状态。关闭 Bloom 必须旁路 Bloom
    金字塔并清空持久输出，不能只把强度设为零。
    特效参数只使用原生 `effects.*` 路径，字段名与 `EffectsConfig` 一致，例如
    `effects.diskRadius`、`effects.diskLifetimeMs`、`effects.ringsCount`、
    `effects.ringsLifetimeMs`、`effects.ringsRadiusMin`、`effects.ringsRadiusMax`、
    `effects.ringsAngularVelocityMultiplier`、`effects.ringsRotationDirection`、
    `effects.shardsClickCount`、`effects.shardsSizeMin`、`effects.shardsSizeMax` 和
    `effects.trailOpacity`。IPC 同时提供 `GetFxConfig`、`SetFxParam`、原子批量的
    `SetFxParams` 与 `ResetFxConfig`；这些是本项目的原生控制接口，不接受 Web 别名或额外单位换算。
    FX 快照和写入白名单不包含输入、HDR、背景、性能或系统字段；这些产品配置必须通过
    `GetConfig`/`SetConfig` 读写，因此 `ResetFxConfig` 的作用域始终只对应 `effects`。
11. Control Center 增加“显示与性能”顶层页。显示器选择器消费 `GetDisplayState`，刷新时优先保留同一
    显示会话，并在可滚动只读区域展示 Host 实际报告的边界、DPI、物理/捕获刷新率、DRR、颜色查询、
    SDR white level、GPU、色彩/输出策略、fallback、WGC 和故障；严格解析失败、
    空会话或未知字段状态必须显式显示为不可用，不得伪装成支持。该页同时通过 `SetConfig` 管理默认关闭的
    `display.hdrEnabled`，以及 `performance.framePacing` 的 `match-display`、`60`、`120`、`144`、
    `unlimited` 五个 wire values。`match-display` 按目标刷新率的精确分数设置最小帧周期，证据缺失或
    无效时回退 60 FPS；只有 `unlimited` 不设置额外最小帧周期。具有稳定 DisplayConfig 标识的会话可通过原子的 `SetDisplayOverride` 和
    `RemoveDisplayOverride` 创建或删除完整逐屏策略；完整策略同时包含特效启用、HDR 请求和帧率模式，
    避免部分写入意外继承另一字段。全局拓扑完整时，schema 2 还列出未连接显示器的遗留 override；
    Control Center 不为它伪造运行状态，只允许通过同一原子删除命令清理。无稳定标识时只允许查看，
    禁止持久化覆盖。配置请求不构成 HDR、
    多显示器或混合 DPI/刷新率支持声明。

## 取舍

- 采用自描述文本协议便于 PowerShell、诊断工具和原生 Win32 客户端调试；性能不是控制面
  的瓶颈。
- 控制面只暴露经过配置校验且已接入原生求值的产品与特效参数；内部 shader、mesh 和 render graph
  常量仍由 Renderer 维护，避免 UI 形成不受控的 GPU 依赖。
- 配置快照表达用户请求，`GetDisplayState` 表达 Host 的逐屏实际运行结果。两者使用独立代次和失败状态，
  避免控制面把“已请求 HDR”误显示成“当前输出 HDR”。
- 当配置文件损坏或管道不可用时，Host 继续使用内存默认值并写入诊断日志；不会为了保存
  配置阻塞或关闭特效。

## 验收

- 无配置文件首次启动会创建当前 schema 的默认 JSON。
- 只接受显式 schema 17；缺少版本、非当前版本、未知字段和枚举别名均被拒绝。
  Host 使用内存默认值继续运行并保留原文件，不执行迁移或部分字段套用。
- 默认模式下未按键 Move 不产生内容；开启拖尾常驻后，第二个有效 Move 起生成拖尾且没有点击 burst。
  常驻、真实按住、出界重入和动态关闭形成独立 stroke，不允许跨状态连线；含边沿帧的尾随 Move 不会
  在同一帧重启常驻段。
- 按压 FX 使用单一帧边界位置；Raw 边沿保持原序供诊断，效果派发归约为布尔帧态并按
  Down→Held→Up 处理；普通 Up-only 帧不移动，三态同时为 true 时各执行一次，Cancel 最后作为原生
  硬边界。采样率为 `0` 时不额外限频；限频时只有位置采样相位读取消息分派 QPC，所有模拟动作仍使用
  统一 `renderTime`。动态修改采样率后的下一位置样本立即接受。
- 一个 Host 进程能同时服务至少一个客户端；第二个 Host 启动会快速退出。
- `GetState`/`SetConfig`/`SetFxParam`/`SetFxParams` 在下一帧可观察，批量参数必须全部通过校验后才提交；
  `Shutdown` 能使 Host 正常退出且无残留进程。
- `GetDisplayState` schema 2 返回独立运行/配置/应用代次、全局拓扑、权威离线 override 和稳定顺序的
  逐屏快照；Control Center 能保留选择，区分配置与实际应用的逐屏策略，并区分请求、解析、实际输出、
  fallback 及未知能力。真实 HDR、多显示器、混合 DPI/刷新率和跨适配器矩阵在硬件执行前保持 `Not Run`，
  本 ADR 继续为 `Proposed`。
