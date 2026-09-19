# Issue #2：点击桌面后常驻拖尾消失

- Issue：<https://github.com/CialloKing/ba-click-fx-desktop/issues/2>
- 排查日期：2026-09-19。
- 录屏版本：BAFX Desktop 0.2.18 安装版。
- 检查源码：`7baeade5f77c59b380e2b3f1517c9411b73312ab`；同时核对 `v0.2.18`。
- 状态：已检查录屏、源码和现有测试；尚未在反馈者环境复现，根因未确认。

本文源码证据、日志不足和原有测试结果针对上述检查基线。后续源码已补强诊断，见文末进展；
`v0.2.18` 安装版不包含这些新事件。

## 录屏证据

约 8 秒的录屏中，控制中心打开时，在窗口外的桌面区域移动能看到拖尾。
随后桌面出现框选矩形，拖尾消失。鼠标移回控制中心后没有立即恢复，
激活窗口后再次移到桌面区域又能看到拖尾。

现象与窗口激活切换相关，不能解释为“鼠标位于桌面区域时禁用拖尾”。
录屏没有同步输入计数、窗口层级、DWM 可见状态和帧提交证据，
也没有提供 Windows 构建号、配置文件或支持日志。

## 源码证据

| 路径 | 当前行为及判断边界 |
| --- | --- |
| `src/windows/src/overlay_window.cpp`，`registerRawMouse()` | 使用 `RIDEV_INPUTSINK | RIDEV_DEVNOTIFY`，输入应能在后台到达；不存在按前台窗口选择性接收输入的业务分支。实际投递是否正常仍需故障机数据。 |
| `src/control-center/control_center_window.cpp`，`WM_ACTIVATEAPP` | 失去激活时只结束快捷键录制，不关闭常驻拖尾、不暂停 Host；发布版行为相同。 |
| `src/desktop/display_pointer_router.cpp`，`consumeFrame()` | 按下结束旧的常驻段；释放后下一帧无边沿的有效 Move 可继续生成常驻段。路由依据显示器和坐标，不判断 Explorer 或普通窗口。 |
| `src/desktop/idle_render_policy.cpp` | 空闲策略依据输入、活动特效、暂停和显示器电源状态，不检查前台应用。 |
| `src/windows/src/overlay_window.cpp`，构造函数、`show()`、`WM_WINDOWPOSCHANGED` | 创建时设置 `WS_EX_TOPMOST`，显示时执行 `SetWindowPos(HWND_TOPMOST)`；运行期没有针对桌面激活的层级恢复。`WM_WINDOWPOSCHANGED` 只把位置或尺寸变化交给拓扑处理。 |

以上窗口、Raw Input、指针路由和模拟相关文件在 `v0.2.18` 与检查提交之间无差异。
当前 `main.cpp` 有主输出协商重构，不能将发布版和当前主循环整体视为相同。

## 待验证方向

1. **覆盖窗口层级或可见性变化。** 最值得先验证。若故障期间输入、模拟和帧提交继续，
   但覆盖层不可见，应检查渲染窗口的 `WS_EX_TOPMOST`、Z 序、`IsWindowVisible`、
   `IsIconic`、`DWMWA_CLOAKED` 和窗口矩形。缺少层级恢复只是代码事实，
   尚不能证明桌面激活确实改变了这些状态；也不能仅凭画面断言是桌面美化软件引起。
2. **输入投递或读取中断。** 检查 `MessagePump.InputDispatched`、`Input.RawMessages`、
   `Input.MoveEvents`、`Input.ButtonEdges`、`Input.CancelEvents`。
   `handleRawInput()` 在 `GetRawInputData`、`GetCursorPos`、`QueryPerformanceCounter`
   成功后才增加 `Input.RawMessages`。读取失败会直接返回，没有记录失败原因；
   因此 Raw 计数停止不等于 Windows 没有投递 `WM_INPUT`。
3. **输入正常但模拟或呈现停止。** 对照 `Window.FrameCount`、`Window.PresentedFps`、
   `Runtime.Paused`、`FramePacing.Timeouts` 及图形故障事件。
   帧提交成功也不证明 DWM 最终显示正常，仍需要窗口状态或可见像素佐证。

## 已执行验证

在检查提交上重新构建 `bafx_windows_tests`、`bafx_desktop_input_tests`、`bafx_fx_tests`，并运行：

```powershell
ctest --test-dir build/x64 -C Release -R '^(pointer_event_coalescing|desktop_input_dispatch|fx_simulation)$' --output-on-failure
```

三组全部通过，共 339 个用例。覆盖 Raw 按键合并、帧边沿、拖尾模拟和桌面输入相关策略。
这些测试不模拟反馈者的真实桌面激活、鼠标输入投递或 DWM 可见结果，不能据此关闭 Issue。

## 下一次复现需要的数据

开启常驻拖尾后，按下面顺序操作并记录各阶段开始的时间；每阶段持续移动鼠标约 20 秒，
使默认 10 秒的 `Performance.Interval` 能覆盖稳定状态：

1. 激活普通窗口，在桌面区域移动，记录正常阶段。
2. 点击或框选桌面，再松开鼠标继续移动，记录异常阶段。
3. 先只把鼠标移到普通窗口，再点击激活它，记录两者是否都能恢复。

随后通过控制中心“打开日志目录”取出相应支持日志，并保留 Windows 构建号、
显示器数量、配置文件，以及是否使用桌面替换或动态壁纸程序的信息。
新构建会同时采样窗口层级候选和 DWM cloak 状态；若输入和帧提交正常，优先比较这些状态，
再决定需要补充哪些故障机证据。当前不据推测加入每帧强制置顶或关闭空闲优化的补丁。

## 日志补强进展

新增 `Input.Health` 区分消息收到与成功读取，并保留失败阶段、错误及取消来源；
`Input.Routing` 记录转交、抑制和坐标失败；`Desktop.ForegroundChanged` 与
`Desktop.SurfaceState` 将前台、覆盖窗口状态、模拟处理、调度和 Present 关联到同一时间线。
状态变化最多每 250 ms 观察一次，稳定输入活动最多每秒汇总，空闲仍保留 10 秒心跳。
逐会话计数通过独立实例编号隔离，避免 HWND 复用造成错误增量。

具体字段和复现判读顺序见 [运行时日志排查](runtime-logging.md)。这些改动补齐故障证据，
没有修改窗口层级或输入路由策略；Issue #2 的根因仍需反馈者环境复现确认。

补强实现截至 `8a95bc9` 的本地验证：Full/Slim Release 构建通过；Full 首轮通过 45/46 项，
修正源码测试把退出阶段误计入渲染循环的边界后，剩余 1 项复测通过；Slim 45/45 项通过。
测试覆盖无效 Raw Input 读取、限频后的故障保留、模拟采样结果、隐藏/失效窗口、
会话重建时的计数基线，以及原有跨进程日志轮转和渲染验证。

独立目录运行 3.2 秒 `interior-trail` 演示和 11.2 秒空闲场景，均正常退出，验证了四类新增事件、
输入心跳、末次刷新、模拟计数和 Present 字段。新增观察记录最大 2,391 字节，无重复字段，
全部记录小于 64 KiB，未产生格式化失败或截断事件。窗口状态在前台变化时持续报告，
这些变化会重新开始其心跳间隔。实测禁用了 Raw Input，演示直接调用模拟，
因此不构成真实鼠标投递、Issue #2 复现、DWM 可见结果或性能开销的验收。
