# 安装维护流程验证 / Installer maintenance validation

日期：2026-10-26。实现提交：`4395d24`、`895935a`、`dbe33f4`、`a666d24`。
版本仍为 `0.3.3`；本次只生成本地验证产物，没有发布版本、改变配置格式或数据保留规则。

## 已执行 / Executed

环境：Windows 10 专业工作站版 `19045`、Windows PowerShell `5.1.19041.6456`、VS 2026、SDK `10.0.28000.0`、Inno Setup `6.7.3`。
当前会话未提权；未发现可用 Windows Sandbox、Hyper-V 服务、VirtualBox 或 VMware 工具。

| 验证 | 结果与边界 |
| --- | --- |
| Full / Slim Release 全目标构建 | 均通过；最后的控制中心修改也分别重新构建 |
| Windows PowerShell 5.1 安装器契约 | 通过，包含 ACL、身份恢复、失败诊断、打包与恢复依赖 |
| Full CTest | `46/47`；只有 `hotkey_process_boundary` 失败 |
| Slim CTest | `45/46`；同一快捷键测试失败 |
| 最后一次针对性 CTest | 安装器契约、进程预检、控制中心均通过，`3/3` |
| 控制中心保存退出 | 离线快捷键草稿和待提交设置失败时保留；无待保存修改时确认后退出；已确认后阻止排队滑块修改 |
| 真实隔离子进程 | 隐藏窗口正常退出、Host 超时强杀、已确认保存的控制中心挂起后强杀、拒绝保存、消息超时、旧协议正常关闭及挂起拒绝强杀均通过 |
| 多进程与身份 | 同路径双 Host 均退出；其他目录同名控制中心保留；控制中心拒绝保存时 Host 保留；句柄核对路径通过 |
| 故障决策 | 未知路径、查询失败、保存结果不明及强杀失败均中止；其中权限和强杀失败采用故障注入，未实测其他会话或受保护进程 |
| 维护事件 | 查询、重复获取拒绝、释放重获、协调者释放后辅助进程句柄继续保护均通过 |
| Inno 原生事件代码 | 提取生产 Acquire/End 实现，在唯一 Local 事件名下运行；正常结束及进程异常终止（退出码 77）后均无遗留事件；正常路径刻意在 InitializeSetup 返回 False（退出码 1） |
| 内部配置入口 | Full / Slim 在非正式安装目录调用均立即返回 `5`；正式目录内的成功初始化仍待隔离管理员验收 |
| Full / Slim 安装包 | 均编译通过，PE 依赖检查及 SHA-256 文件通过；不等于安装、注册和卸载已经验收 |

原生进程回归为缩短 CTest 耗时使用 200 ms 正常退出期限，生产默认仍为每类进程共享 10 秒，强杀后共享 3 秒。
测试只启动、结束自己创建的临时进程，不操作现有安装程序。模拟的保存回复只验证跨进程协议及退出策略，
不能证明实际 Host 已持久化任意用户草稿；该完整链路仍属于下方隔离安装验收。

快捷键测试失败信息为 `timed out waiting for native hotkey state at line 234`，对应输入注入后等待暂停状态。
Full 单独重试仍失败；运行本次修改前保留的 `build/x64/tests/Release/bafx_hotkey_process_tests.exe`
（2026-10-24 构建）也在相同位置失败。相关快捷键实现及测试源没有被本次修改，未放宽断言或隐藏失败。

可复现命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify-user-installer-contract.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify-user-installer-contract.ps1 -ProcessPreflightOnly
cmake --build build/ui-navigation-full --config Release --parallel 2 -- /nodeReuse:false
cmake --build build/ui-navigation-slim --config Release --parallel 2 -- /nodeReuse:false
ctest --test-dir build/ui-navigation-full -C Release --output-on-failure -j 2
ctest --test-dir build/ui-navigation-slim -C Release --output-on-failure -j 2
```

本地证据位于 `artifacts/local/maintenance-20261026/`：`native-processes.log`、`targeted-full.log`、
`ctest-slim.log`、`package-full.log`、`package-slim.log` 及 `probe-inno-maintenance.ps1` 生成的 Inno 日志。
这些本地产物未加入 Git。

| 本地安装器 | SHA-256 |
| --- | --- |
| `verified-full/ba-click-fx-desktop-0.3.3-setup-windows-x64.exe` | `95506A44FDE80B97CDF8CA603B32C4FA74BE128D79A6443357EE5EAC9AB6FEE0` |
| `verified-slim/ba-click-fx-desktop-0.3.3-slim-setup-windows-x64.exe` | `80774C622DA2E3501AE6E2A303869E6B9B253BA3979C780DA28D8C9097247790` |

## 尚未验收 / Outstanding acceptance

**不能据上述测试声称真实安装、AppX、证书和卸载清理验收完成。** 当前会话没有可用的隔离管理员环境，
也没有在用户现有安装上执行覆盖、证书清理或卸载。应在可恢复快照的 Windows 10/11 虚拟机中分别完成 Full 和 Slim：

1. 安装旧版，启动 Host 和控制中心（含托盘隐藏），创建快捷键草稿与待提交设置，再运行本地新安装器；确认配置持久化、进程顺序、文件版本和身份状态。
2. 使用非法草稿、Host 离线或未确认的保存结果，确认更新及卸载中止、草稿和原安装状态仍在；修正后可重试。
3. 挂起 Host、已确认保存的控制中心；验证 10 秒及 3 秒期限、强制结束失败后的材料保留。旧控制中心挂起时必须手动退出。
4. 在维护期间反复启动两端、并行运行第二个安装器或卸载器；检查 `1618`、互斥和取消释放。分别终止协调者与辅助进程，验证重启后的事务恢复。
5. 验证跨 Windows 会话、多实例、无法读取路径、便携同名实例、从旧版首次升级以及首次安装尚未提交文件时的卸载恢复。
6. 验证正式目录内部初始化生成缺失配置、保留已有配置、拒绝组合参数和非管理员；模拟覆盖、注册、回滚及清理失败，确认不报告成功。
7. 完整卸载后核对本实例 AppX、证书、启动项和程序文件清理，`data` 与其他安装实例的数据及身份保留。

## English summary

Implementation and local packaging are complete; release/version, configuration formats and retention policies are unchanged.
Full and Slim builds passed. Full CTest passed 46/47 and Slim 45/46; both failed the native hotkey input test at line 234.
The same pre-change binary fails at that location, and a separate Full retry also failed. No assertion was weakened.
The final installer contract, process preflight and Control Center checks passed 3/3.

Real isolated child processes covered graceful shutdown, acknowledged-save termination, refusal/unknown-save protection, legacy fallback,
hidden windows, multiple Host instances and same-name path exclusion. Native Inno code verified event release after normal cancellation and
abrupt process termination using a unique Local test event. Unit/event fixtures verified that a worker lease outlives its coordinator lease.
These tests use short test deadlines and synthetic save replies; they do not prove real configuration persistence through an installed Host.

Machine-level acceptance is still outstanding: this session is not elevated and no available sandbox/VM tooling was found.
The seven steps above require a disposable Windows VM and include real AppX registration, certificate ownership, Program Files replacement,
full uninstall, data retention, cross-session behavior and recovery failures. Existing user installations were not modified for this validation.

API references: [bounded window messaging](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw),
[event handles and lifetime](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-openeventw).
