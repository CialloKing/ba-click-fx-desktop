# ba-click-fx-desktop 0.2.8

0.2.8 是一次普通用户体验补丁：把已存在的暂停/恢复能力补到 Control Center 通知区域菜单，并精简
重复的 ROI 测试代码。本版不是 ROI 性能晋级版本，不附带整机性能、功耗或输入延迟声明。

## 通知区域暂停与恢复

- Host 已连接并运行时，Control Center 通知区域菜单显示“暂停特效”；暂停后动态显示“恢复特效”。
- 打开菜单前会先刷新 Host 状态，避免 Control Center 长时间隐藏或其他本地 IPC 客户端改变状态后显示
  旧文案。
- Host 断开时暂停/恢复项置灰；原有“打开控制中心”和“退出控制中心”保持不变。
- 该入口复用现有 `Pause`/`Resume` IPC 和错误处理，不增加状态机或配置字段。暂停状态只属于当前 Host
  进程，不写入配置，也不跨 Host 重启持久化。

## 兼容性与升级

- 产品版本提升到 0.2.8，Host 与 Control Center 仍要求产品版本完全一致后才允许写设置。
- 主配置 schema 保持 19；现有 `BAFX.config.json`、显示器 override、`data` 目录和 effects-only
  `fx-profiles` 不需要迁移、删除或重建。
- Active-FX ROI 在本版及后续版本继续默认关闭。缺少 4K 170 Hz 环境不阻塞本次普通体验更新；相关
  schema 4 硬件晋级仍保持 `Not Run`，只门禁整机性能、功耗、输入延迟声明和独立的 Differential Bloom
  ROI 实验里程碑。

## 测试代码精简

- 合并 ROI 渲染器中的重复断言与场景驱动，复用采集器和报告器公共夹具及像素数据。
- 非发布诊断报告测试继续保留，但只在显式配置 `BAFX_ENABLE_ROI_DIAGNOSTIC_TESTS=ON` 时注册，不再延长
  每次普通发布门。
- 六个测试文件合计 `205` 行新增、`804` 行删除，净减少 `599` 行；生产配置、渲染路径和验证阈值不变。

## 候选验证状态

- Full `cmake --workflow --preset release-verify`：`44/44` 通过，总测试时间 `118.73 s`。
- Slim `cmake --workflow --preset slim-release-verify`：`43/43` 通过，总测试时间 `71.85 s`。
- Full 与 Slim 均重新配置并编译了 0.2.8 Host、Control Center 和 Identity Signer；Slim 只做源码验证，
  不生成或上传发布资产。
- Full 便携 ZIP、ZIP 哈希、安装器和安装器哈希已在本地生成并通过脚本与独立 SHA-256 复算。ZIP 为
  `98489A0E1456779904DAA52C1AEEE10D982637F5112D0D06C057B31B81824E37`，安装器为
  `60D403A087D9C3F295EEDCB38B38935F52764EA8292BC2B83017EE3E3B495C59`。
- 正式发布前仍需完成通知区域菜单的完整人工操作验收、SDK
  `10.0.19041.0`/`10.0.22621.0`/`10.0.26100.0` CI、annotated tag 和远端资产复核。
