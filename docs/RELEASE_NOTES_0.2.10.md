# ba-click-fx-desktop 0.2.10

0.2.10 增加由 Host 持有、Control Center 配置的原生全局快捷键。默认行为保持保守：升级后不会自动
占用任何组合，原有渲染、Profile、显示器 override 和 Spout2 设置保持不变。

## 四项全局快捷键

Control Center 新增“快捷键”页，可以录制、清除、整组保存和重试以下动作：

- 暂停／恢复特效，只改变当前运行状态；
- 切换常驻拖尾，通过现有配置事务保存；
- 切换到下一个特效预设，不把快捷键写入 effects-only Profile；
- 退出 Host。

四项默认全部未绑定。0.2.10 删除旧的 `Ctrl+Alt+F12`、`Ctrl+Shift+F12` 固定退出组合及
`GetAsyncKeyState` 轮询兜底；未配置退出快捷键时，仍可使用通知区域菜单或 Control Center 关闭 Host。

## 录制和注册限制

- Host 只使用 `RegisterHotKey`/`WM_HOTKEY`，注册统一附加 `MOD_NOREPEAT`，长按不会重复执行动作。
- 支持单个非修饰主键，或 Ctrl/Alt/Shift/Win 加一个主键；不区分左右修饰键。不支持多普通键、宏、
  仅修饰键或 F12，重复组合不能保存。Win 组合和其他由系统保留的组合不保证可用。
- 注册是系统级占用，不提供按键透传，可能影响前台软件原有操作。已被其他程序占用的组合可能无法
  录制或注册，页面会显示对应 Win32 错误。
- 录制期间旧注册继续占有组合但不执行动作，候选只保存在 Control Center 草稿。失焦、显式取消、
  30 秒总时限或连续 5 秒没有匹配会话 token 的续期都会结束录制并恢复动作。

## 保存、异常和恢复

“保存全部”按当前 Host generation 提交完整绑定，顺序固定为：保留旧注册并准备全部新增组合、原子写入
完整配置、发布新动作映射、释放不再使用的旧组合。注册失败、generation 冲突或写盘失败时，旧配置和
旧注册保持有效。

如果配置已经写入，但激活结果无法确认或旧注册清理失败，Control Center 会重新读取 Host 的权威配置并
提示重启 Host。启动时某个已保存组合注册失败不会阻止特效运行；释放占用后可使用“重试注册”，该操作
只重试已保存绑定，不改写配置，并汇总已保存、已注册和失败数量。

未保存草稿在切换页面后保留；离开快捷键页、Control Center 失焦、Host 断线或关闭窗口都会结束录制。
关闭时可以保存、丢弃或取消；Host 离线时会明确提供丢弃/取消选择。Control Center 的“重置默认”恢复
其他持久化设置，但保留 Host 已保存的整组快捷键和当前暂停状态。

## 配置与诊断兼容性

主配置升级为 schema 20，新增 `hotkeys.togglePause`、`toggleAlwaysOnTrail`、`nextFxProfile`、`shutdown`。
字段完整的 schema 14 至 19 按固定链迁移；schema 19 只补充四项 `null` 绑定。其他未知版本、未知字段、
缺失字段和枚举别名继续 fail-closed。迁移不删除现有 `data`、显示器 override 或 effects-only
`fx-profiles`。

支持报告新增以下启动快照字段：

- `Hotkeys.StateScope=startup`；
- `Hotkeys.RegisteredMask`、`Hotkeys.CleanupError`；
- 四项动作各自的 `Registered` 和 `Error`；
- `Exit.PollingFallback=disabled`。

这些字段记录 Host 启动时的注册结果，不代表生成报告时的实时状态。实时绑定、注册错误和录制会话由
`GetHotkeyState` 提供。

## 发布资产与验证边界

正式 GitHub Release 只提供 Full 版四个资产：

- `ba-click-fx-desktop-0.2.10-Portable-windows-x64.zip`
- `ba-click-fx-desktop-0.2.10-Portable-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.10-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.10-setup-windows-x64.exe.sha256`

Slim 继续保留源码构建、测试和本地打包入口，不提供预编译 Release 下载。发布前仍须记录 Full/Slim
workflow、三档 Windows SDK CI、Control Center 快捷键人工验收和 Full 四资产校验结果；未完成的门禁
不得写成已通过。本次功能不改变历史 Active-FX ROI schema 19 证据或其结论。
