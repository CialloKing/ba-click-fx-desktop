# ADR-0008：Host 产品控制面

- 状态：**Proposed**
- 日期：2026-08-10

## 背景

当前可执行文件已经具备输入、D3D11/DirectComposition、三种渲染模式和可选 WGC
背景采样，但配置和运行时控制仍然只存在于进程启动参数中。下一阶段需要让 Host 能够
长期运行，并允许独立的控制界面在不接触渲染线程的情况下读取状态、修改配置和请求退出。

## 决策

1. 配置由 `bafx_config` 持有，使用版本化 JSON（当前 schema 为 7）。读取时先完成迁移和
   校验，再生成不可变的运行时快照；写入使用同目录临时文件、flush、替换的原子流程。
2. Host 是配置的唯一写入者。外部客户端只能通过版本化的本地 Named Pipe 请求操作，不能
   取得 Renderer 或 D3D11 immediate context 的句柄。
3. IPC 使用 UTF-8、以换行分隔的请求/响应记录。请求是一个命令 token，可选地跟随一个
   `SetConfig` JSON 负载；响应以 `OK` 或 `ERR <code> <message>` 开头。未知命令、格式错误、
   NUL/换行注入和超限请求都返回可诊断错误而不终止 Host。
4. Host 通过用户范围的命名互斥体保证单实例；管道服务在独立线程运行，Render Owner 只
   在帧边界消费已校验的命令。Control Center 退出不会影响 Host。
5. 首个垂直切片只承诺 `GetState`、`GetConfig`、`SetConfig <schema-7-json>`、
   `SetConfig {generation,path,value}`、`Pause`、`Resume` 和 `Shutdown`。路径更新只允许
   配置库声明的产品字段，并在 generation 不匹配时返回冲突。响应中的 `generation` 用于
   客户端判断快照是否变化；Preset/Profile 等更高层功能在此协议稳定后再增加。
6. `background.mode` 的产品 wire values 与 Control Center 显示名固定如下：
   `background-aware`（背景感知）、`classic`（贴近原版）和 `light-background`（浅色背景优化）。
   只有背景感知启用 WGC；WGC 失败时回退 Classic。Classic 使用现有 FX-only coverage transport，
   LightBackground 使用 `visual-max` + `bright-core`，并将 source-over Alpha 限制为 `0.85`；后两者
   关闭 WGC。

   | Control Center 显示名 | `background.mode` wire value | WGC |
   | --- | --- | --- |
   | 背景感知 | `background-aware` | 启用，失败回退 Classic |
   | 贴近原版 | `classic` | 关闭 |
   | 浅色背景优化 | `light-background` | 关闭 |

7. 新配置的 `background.allowSystemBorder` 默认为 `true`。Control Center 通过复选框更新该字段；
   用户取消勾选后，Host 必须在 `StartCapture` 前确认无边框 WGC 能力，否则回退 Classic。已有
   schema 4 中显式保存的 `false` 必须保留，不能被启动或迁移流程重置。DComp overlay 没有浏览器
   `Screen` API 的逐像素等价物，控制面不得宣称三种模式都能逐像素复现桌面。
8. Control Center 提供默认关闭的“拖尾常驻”复选框，并反向映射到历史配置字段
   `input.trailOnlyWhilePressed`。开启后，未按键的 Raw Input Move 使用独立的纯拖尾实例；首个样本只
   建立锚点，不能生成点击圆盘、圆环或点击 burst。真实按下、出界、暂停、关闭拖尾或关闭总特效会
   结束当前常驻段，已有几何按原生命周期衰减，下一段不得与旧坐标建立假连接。schema 5 中该字段
   从未接入运行时，因此迁移到 schema 6 时统一设为 `true`，保持升级前的实际按住拖尾行为。
9. Control Center 提供 `input.samplingRateHz` 滑块：`0` 表示不限频，整数 `1..1000` 表示 Move
   采样率上限。Host 用 Raw Input 的 QPC 时间判断间隔，限频发生在轨迹/碎片模拟之前，且独立于渲染帧率、
   暂停后的模拟时间和 Unity 空间阈值。Down/Up/Cancel 不限频；切换采样率会清空旧相位，使下一 Move
   立即建立新相位。`30 Hz` 只作为手机客户端视觉近似的人工审核建议，不能宣称为游戏固定参数。

## 取舍

- 采用自描述文本协议便于 PowerShell、诊断工具和原生 Win32 客户端调试；性能不是控制面
  的瓶颈。
- 配置字段只暴露产品语义（启用、特效缩放、Bloom 强度、捕获模式等），底层材质常量仍由
  Renderer 维护，避免 UI 形成不受控的 GPU 依赖。
- 当配置文件损坏或管道不可用时，Host 继续使用内存默认值并写入诊断日志；不会为了保存
  配置阻塞或关闭特效。

## 验收

- 无配置文件首次启动会创建当前 schema 的默认 JSON。
- schema 1/2/3/4/5/6 配置可迁移到 schema 7；schema 1/2/3 以及 schema 4 缺失字段时默认允许系统捕获
  边框，schema 4 的显式 `background.allowSystemBorder=false` 保持不变。旧的背景模式名称按迁移规则归一到
  `background-aware`，非法值被拒绝并保留原文件。
- 默认模式下未按键 Move 不产生内容；开启拖尾常驻后，第二个有效 Move 起生成拖尾且没有点击 burst。
  常驻、真实按住、出界重入和动态关闭形成独立 stroke，不允许跨状态连线。
- 采样率为 `0` 时保留现有全部转折；限频时只连接被接受的 Move，且空间距离不足的已接受样本仍推进
  时间相位。动态修改采样率后的下一 Move 立即接受。
- 一个 Host 进程能同时服务至少一个客户端；第二个 Host 启动会快速退出。
- `GetState`/`SetConfig` 在下一帧可观察，`Shutdown` 能使 Host 正常退出且无残留进程。
