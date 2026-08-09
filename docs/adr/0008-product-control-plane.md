# ADR-0008：Host 产品控制面

- 状态：**Proposed**
- 日期：2026-08-10

## 背景

当前可执行文件已经具备输入、D3D11/DirectComposition、FX-only Bloom 和可选 WGC
背景采样，但配置和运行时控制仍然只存在于进程启动参数中。下一阶段需要让 Host 能够
长期运行，并允许独立的控制界面在不接触渲染线程的情况下读取状态、修改配置和请求退出。

## 决策

1. 配置由 `bafx_config` 持有，使用版本化 JSON（当前 schema 为 3）。读取时先完成迁移和
   校验，再生成不可变的运行时快照；写入使用同目录临时文件、flush、替换的原子流程。
2. Host 是配置的唯一写入者。外部客户端只能通过版本化的本地 Named Pipe 请求操作，不能
   取得 Renderer 或 D3D11 immediate context 的句柄。
3. IPC 使用 UTF-8、以换行分隔的请求/响应记录。请求是一个命令 token，可选地跟随一个
   `SetConfig` JSON 负载；响应以 `OK` 或 `ERR <code> <message>` 开头。未知命令、格式错误、
   NUL/换行注入和超限请求都返回可诊断错误而不终止 Host。
4. Host 通过用户范围的命名互斥体保证单实例；管道服务在独立线程运行，Render Owner 只
   在帧边界消费已校验的命令。Control Center 退出不会影响 Host。
5. 首个垂直切片只承诺 `GetState`、`GetConfig`、`SetConfig <schema-3-json>`、
   `SetConfig {generation,path,value}`、`Pause`、`Resume` 和 `Shutdown`。路径更新只允许
   配置库声明的产品字段，并在 generation 不匹配时返回冲突。响应中的 `generation` 用于
   客户端判断快照是否变化；Preset/Profile 和 WinUI 3 页面在此协议稳定后再增加。

## 取舍

- 采用自描述文本协议便于 PowerShell、诊断工具和未来 WinUI 客户端调试；性能不是控制面
  的瓶颈。
- 配置字段只暴露产品语义（启用、特效缩放、Bloom 强度、捕获模式等），底层材质常量仍由
  Renderer 维护，避免 UI 形成不受控的 GPU 依赖。
- 当配置文件损坏或管道不可用时，Host 继续使用内存默认值并写入诊断日志；不会为了保存
  配置阻塞或关闭特效。

## 验收

- 无配置文件首次启动会创建当前 schema 的默认 JSON。
- schema 1/2 配置可迁移到 schema 3，非法值被拒绝并保留原文件。
- 一个 Host 进程能同时服务至少一个客户端；第二个 Host 启动会快速退出。
- `GetState`/`SetConfig` 在下一帧可观察，`Shutdown` 能使 Host 正常退出且无残留进程。
