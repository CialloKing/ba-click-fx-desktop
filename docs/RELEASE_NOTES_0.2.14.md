# ba-click-fx-desktop 0.2.14

录屏兼容测试模式调整为 Windows 11 26H2 及以后，最低 OS build 从 `28000` 降为 `26300`。

## 主要更新

- 控制中心的中英文模式名称、版本不足提示和版本无法确认提示同步更新为 26H2。
- Host 与控制中心共用 build `26300` 的最低门槛；原已允许的 build `28000` 及更高版本继续可选。
- Host 拒绝信息直接使用统一的最低版本常量，诊断日志同步记录新门槛。
- 更新使用文档和验收说明，明确版本门槛与实际 WGC 能力探测的区别。

## 使用与兼容性

在控制中心“基础设置”的渲染模式中选择“录屏兼容（测试，仅 Windows 11 26H2 及以后）”。
版本低于 build `26300` 或无法确认时不会应用该选择；旧系统启动时发现保存的测试模式仍回退到浅色背景优化。

允许选择后，程序继续探测实际 WGC Session 的窗口排除接口，并检查排除列表与 configuration iteration。
Session-local 排除失败时依次回退到全局窗口排除和 FX-only；全局排除路径可能使外部录屏看不到特效。
26H2／OBS 的实机录制组合尚未完成验收，模式继续标为测试，不代表所有录屏器均可正常录制。

默认背景感知模式、主配置 schema 20、IPC 和特效预设格式保持不变。更新时请使用完整安装器或便携包，保持 Host 与控制中心版本一致。

## English

Recording compatibility testing now targets Windows 11 26H2 and later, lowering the minimum OS build from `28000` to `26300`.

- Updated both Chinese and English mode labels, unsupported-version messages and diagnostics.
- Host and Control Center share the new build threshold. Previously eligible builds `28000` and above remain selectable.
- Actual WGC session capabilities are still checked at runtime. Session-local exclusion falls back to global window exclusion, then FX-only if needed.
- Global exclusion may hide effects from external recording. The 26H2/OBS combination still requires testing on a target system; this remains a test mode.
- The default background-aware mode, configuration schema 20, IPC and effect profile format are unchanged. Update Host and Control Center together.

## 发布资产

提供 Full 版安装器、便携 ZIP 及各自的 SHA-256 校验文件。Slim 保留源码构建与本地验证，不上传预编译资产。
