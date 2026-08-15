# SPK-002 portable 异步无边框拒绝与恢复子集 / 2026-08-15

- Spike：`SPK-002 / portable asynchronous borderless rejection and recovery subset`
- 采集提交：`c3781f7`
- 系统：Windows 10 Pro for Workstations `10.0.19045`，x64
- GPU/驱动：NVIDIA GeForce RTX 4060 Laptop GPU，`32.0.16.1074`
- 显示输出：`3840x2160 @ 144 Hz`，SDR
- Host：`0.1.0-alpha.12`
- 结果：**当前 HEAD 的 portable 权限预检、FX-only 回退和允许边框恢复子集通过**

## 场景

Host 使用隔离 portable 目录，模式始终保持 `background-aware`：

```text
allowSystemBorder=true -> generation 1 active
-> allowSystemBorder=false -> generation 2 not-packaged / fallback-fx-only
-> allowSystemBorder=true -> generation 3 active
-> 6000 ms 延迟 demo click -> background participation
-> IPC Shutdown
```

该场景没有发送真实鼠标输入。延迟 demo click 只在恢复完成后由 Host 自己生成，避免移动用户光标或点击
覆盖层下面的窗口。Host 同时设置 `--quit-after-ms=14000` 作为 IPC shutdown 之外的独立截止；每次 pipe
连接、写入和读取分别有 `750/1000/1500 ms` 截止，进程退出另有 `10 s` 截止。

## 结果

- 控制代次按 `1:active -> 2:fallback-fx-only -> 3:active` 前进。
- 事件 `23` 在代次 2、动作 0 记录
  `not-packaged / 0x80073D54 / AsyncStatus=not-started / Allowed=false`。
- 代次 2 的动作严格为
  `request-borderless-access -> stop-sensor -> set-affinity-included`。权限结论位于任何新 WDA 排除、
  Session 或 FramePool 创建之前；拒绝后事件 `39` 确认 WDA 请求值和回读值均为 `0x00000000`。
- 事件 `42` 证明旧 Session 的 1 帧已关闭，首个 FramePool/Session 和两类事件注册全部配平，
  `Failures=0`、`AllReleased=true`。拒绝事务没有创建第二组捕获资源。
- 重新允许系统边框后，代次 3 执行
  `stop-sensor -> set-affinity-excluded -> apply-overlay-profile -> start-sensor`。延迟 demo click 在事件 `62`、
  帧 `28125` 产生背景参与，WGC/快照身份均为 `epoch=2, generation=116`。
- 最终性能窗记录 `28130` 个呈现帧、WGC 获取 `228` 帧、接受 `119` 个样本、背景参与 `6` 帧；
  本场景 Raw Input 和按钮边沿均为 `0`，符合无外部输入合同。
- `Shutdown` 得到 IPC 成功响应，Host 退出码为 `0`。最终 `228/228` 帧、`2/2` FramePool、`2/2`
  Session 和两类 `2/2` 事件注册全部配平，`Failures=0`、`AllReleased=true`。日志有连续的 `1..79`
  事件序号、一个 `Process.Exited`，没有 Error/Fatal，也没有遗留 Host 进程。

## 原始证据

- `ba-click-fx-desktop-support.log`
  - SHA-256：`9882D7966B2B6257DE1EE1EC9019D34EAE73C48FB45F6587E3BAF3EF9985DC3B`
- `BAFX.config.json`
  - SHA-256：`8D00E9BB3FE112AF3965F9682403B347DBEA48054D284399ECD7A645C81741E7`
- 被测 `ba-click-fx-desktop.exe`
  - SHA-256：`E866C019F1A8F3237878EFB975F407304D525D904AFBD21E7AAFE375EB9293EA`
- `ipc-transcript.txt` 保存有界控制步骤
  - SHA-256：`16EC69C808D7F6AA9A4E70DE8B5E2B0700ACB4EC86488672B860CB9FED485E73`
- `verification.json` 保存机器可读的关键事件与支持边界。

## 限制

- 这是 portable EXE 的 `not-packaged` 早期拒绝，不是 packaged `DeniedByUser`/`DeniedBySystem`，也不证明
  无边框成功。
- 本场景验证当前异步权限预检发生在资源副作用前；权限接口因 portable 身份同步返回 `not-started`，
  不覆盖 Windows 11 权限 UI 长时间 Pending、用户取消或截止竞态。
- 未使用外部录屏器，不证明录屏可见性；未覆盖外部 WDA 篡改、真实 item/monitor close、长期压力/功耗、
  HDR、多显示器、混合 DPI/刷新率、热插拔、跨适配器或真实 device lost/reset。
- 完整 SPK-002 继续为 `Not Run`；ADR-003、ADR-004 和 ADR-007 继续为 `Proposed`。
