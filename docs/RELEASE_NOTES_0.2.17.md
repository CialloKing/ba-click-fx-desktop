# ba-click-fx-desktop 0.2.17

完善日志入口、操作追踪和运行诊断，让连接失败、配置变更、卡顿与退出问题更容易定位。

## 更新内容

- **日志目录与清理**：系统页可直接打开日志目录。清理操作汇总控制中心和 Host 的结果；Host 未连接时仍可清理控制中心日志。
- **日志可靠性**：记录写入、轮转、锁等待和维护失败，恢复后补记状态；跨进程协调日志操作，限制文件和单条记录大小，降低备份维护频率。
- **操作追踪**：控制中心记录连接变化、操作结果和 IPC 耗时，合并重复轮询错误。Host 记录 IPC/快捷键来源、配置前后值、代次及失败原因；超大内容不会挤掉结果与错误信息。
- **退出诊断**：记录退出原因、退出码、运行时长和呈现帧数，区分退出请求、故障与最终退出。
- **性能与资源**：十秒窗口记录最慢帧的同帧上下文、日志各阶段耗时，以及工作集、私有提交内存和进程句柄数。空闲窗口仍采样，不逐帧写盘。

主配置保持 schema 20，支持日志保持 schema 2，现有配置与特效预设无需迁移。诊断中的 API 经过时间不等同于 GPU 执行或物理上屏延迟；本次更新不新增渲染性能或外部录屏兼容性声明。

## 验证范围

- Windows SDK `10.0.28000.0`：Full 与 Slim Release 干净构建通过，未出现编译警告或错误。
- Full CTest 46/46、Slim CTest 45/45 通过，包含日志跨进程协调和外壳导航契约检查。
- 文档检查及便携包文件清单、版本、PE 依赖、Host 与控制中心启动验证通过。

## English

Improves log access and diagnostics for connection failures, configuration changes, slow frames, and shutdowns.

- Open the log folder from the System page. Log cleanup combines Control Center and Host results and remains available for Control Center while disconnected.
- Track log write, rotation, lock, and maintenance failures and recovery. Coordinate access across processes, bound file and record sizes, and reduce backup maintenance frequency.
- Record connection changes, operation outcomes, IPC duration, configuration sources, before/after values, generations, and errors. Repeated polling failures are combined; oversized details preserve outcome metadata.
- Record shutdown reasons, exit codes, uptime, and rendered frames, with separate events for shutdown intent, failure, and final exit.
- Retain a coherent slowest-frame snapshot per reporting window and sample process memory, handles, and diagnostic-stage durations every ten seconds, including idle windows.

Configuration schema 20 and log schema 2 remain unchanged. API elapsed times do not measure GPU execution or physical display latency. This release makes no new rendering-performance or external-recording compatibility claims.

Full and Slim clean Release builds passed with Windows SDK `10.0.28000.0`, without compiler warnings or errors. CTest passed 46/46 for Full and 45/45 for Slim, including cross-process logging and shell-navigation contracts. Documentation and Portable ZIP checks passed for contents, versions, PE dependencies, and Host/Control Center startup.

## 发布资产 / Downloads

提供 Full 版安装器、便携 ZIP 及各自的 SHA-256 校验文件。Slim 完成构建与测试，不上传预编译资产。

Full installer and Portable ZIP, each with a SHA-256 sidecar. Slim is built and tested but has no prebuilt release assets.
