# ADR-0008：Host 产品控制面

- 状态：**Proposed**
- 日期：2026-08-10

## 背景

当前可执行文件已经具备输入、D3D11/DirectComposition、三种渲染模式和可选 WGC
背景采样，但配置和运行时控制仍然只存在于进程启动参数中。下一阶段需要让 Host 能够
长期运行，并允许独立的控制界面在不接触渲染线程的情况下读取状态、修改配置和请求退出。

## 决策

1. 配置由 `bafx_config` 持有，使用版本化 JSON（当前 schema 为 13）。读取时只接受完整的当前
   schema，不迁移非当前文件，也不接受未知字段或枚举别名；校验后生成不可变的运行时快照。写入使用
   同目录临时文件、flush、替换的原子流程。
2. Host 是配置的唯一写入者。外部客户端只能通过版本化的本地 Named Pipe 请求操作，不能
   取得 Renderer 或 D3D11 immediate context 的句柄。
3. IPC 使用 UTF-8、以换行分隔的请求/响应记录。请求是一个命令 token，可选地跟随一个
   JSON 负载；响应以 `OK` 或 `ERR <code> <message>` 开头。未知命令、格式错误、
   NUL/换行注入和超限请求都返回可诊断错误而不终止 Host。
4. Host 通过用户范围的命名互斥体保证单实例；管道服务在独立线程运行，Render Owner 只
   在帧边界消费已校验的命令。Control Center 退出不会影响 Host。
5. 基础配置协议保留 `GetState`、`GetConfig`、`SetConfig <schema-13-json>`、
   `SetConfig {generation,path,value}`、`Pause`、`Resume` 和 `Shutdown`。路径更新只允许
   配置库声明的产品字段，并在 generation 不匹配时返回冲突。响应中的 `generation` 用于
   客户端判断快照是否变化；Preset/Profile 等更高层功能在此协议稳定后再增加。
6. `background.mode` 的产品 wire values 与 Control Center 显示名固定如下：
   `background-aware`（背景感知）、`recording-compatible`（录屏兼容拟合）和
   `light-background`（浅色背景优化）。只有背景感知启用 WGC；WGC 失败时回退内部 FX-only
   coverage transport。RecordingCompatible 按 Web 透明覆盖层的 `visual-max` + `bright-core`、
   `0.90` Alpha 上限、`source-over` 和未知背景合同拟合；LightBackground 使用同一策略，但将 Alpha
   上限收紧为 `0.85`。后两者关闭 WGC。

   | Control Center 显示名 | `background.mode` wire value | WGC |
   | --- | --- | --- |
   | 背景感知 | `background-aware` | 启用，失败回退内部 FX-only |
   | 录屏兼容拟合 | `recording-compatible` | 关闭 |
   | 浅色背景优化 | `light-background` | 关闭 |

7. `background.allowSystemBorder` 默认为 `true`。Control Center 通过复选框更新该字段；
   用户取消勾选后，Host 必须在 `StartCapture` 前确认无边框 WGC 能力，否则回退内部 FX-only。
   DComp overlay 没有浏览器
   `Screen` API 的逐像素等价物，控制面不得宣称三种模式都能逐像素复现桌面。
8. Control Center 提供默认关闭的“拖尾常驻”复选框，并反向映射到配置字段
   `input.trailOnlyWhilePressed`。这是 native/Web 产品增强；开启后，未按键的 Raw Input Move 使用独立的
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
10. Control Center 的高级页包含“时间与透明度”“粒子与材质”“圆环参数”“点击碎片”“Bloom 参数”五个二级页面。
    特效参数使用 Web 风格的点号路径，当前入口包括 `disk.radius`、`disk.lifetimeMs`、
    `rings.count`、`rings.lifetimeMs`、`rings.radiusMin`、`rings.radiusMax`、
    `rings.angularVelocityMultiplier`、`rings.rotationDirection`、`rings.hdrIntensity`、
    `shards.hdrIntensity`、`shards.clickCount`、点击寿命上下限、出生半径、速度上下限、
    `shards.sizeMin`、`shards.sizeMax` 和 `trail.trailOpacity`。IPC 同时提供 `GetFxConfig`、`SetFxParam`、原子批量的
    `SetFxParams` 与 `ResetFxConfig`；只暴露已经接入 Native 模拟或材质求值的子集，不能根据 Web Schema
    中存在某个路径就宣称 Native 已实现该参数。

## 取舍

- 采用自描述文本协议便于 PowerShell、诊断工具和原生 Win32 客户端调试；性能不是控制面
  的瓶颈。
- 控制面只暴露经过配置校验且已接入 Native 求值的产品或 Web 参数；内部 shader、mesh 和 render graph
  常量仍由 Renderer 维护，避免 UI 形成不受控的 GPU 依赖。
- 当配置文件损坏或管道不可用时，Host 继续使用内存默认值并写入诊断日志；不会为了保存
  配置阻塞或关闭特效。

## 验收

- 无配置文件首次启动会创建当前 schema 的默认 JSON。
- 只接受显式 schema 13；缺少版本、非当前版本、未知字段和枚举别名均被拒绝。
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
