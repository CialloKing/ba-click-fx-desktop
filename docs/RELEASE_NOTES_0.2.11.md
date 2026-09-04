# ba-click-fx-desktop 0.2.11

0.2.11 是小范围体验与参考证据同步版本。主配置继续使用 schema 20，升级不会改变现有快捷键、渲染设置、
显示器 override、`data` 目录或 effects-only `fx-profiles`。

## 项目仓库入口

Control Center 的“系统 > 版本与更新”新增“打开项目仓库”按钮，并提示用户可以前往项目仓库点 Star。
该按钮始终打开固定的[官方项目仓库](https://github.com/CialloKing/ba-click-fx-desktop)，不依赖更新检查是否
成功，也不采用网络响应中的跳转地址。原有“检查更新”和“打开 Release”行为不变。

## Unity 参考同步

本版本同步 Unity 重建工程更新后的材质、Touch Shader、审计文档和 9 张基线图清单。更新确认 Cross2、
Trail、Tri3 使用队列 4499，Tri2 使用队列 4550；三个 Touch Shader 使用 `Cull Back`、`ZWrite Off` 和
默认 `ZTest LEqual`，独立 UI Pass 继续覆盖为 `ZTest Always`。原生 D3D11 路径已经符合这些有效语义，
因此不改变运行时绘制逻辑。

## 发布资产与验证边界

正式 GitHub Release 只提供 Full 版四个资产：

- `ba-click-fx-desktop-0.2.11-Portable-windows-x64.zip`
- `ba-click-fx-desktop-0.2.11-Portable-windows-x64.zip.sha256`
- `ba-click-fx-desktop-0.2.11-setup-windows-x64.exe`
- `ba-click-fx-desktop-0.2.11-setup-windows-x64.exe.sha256`

`reference/unity-reference.json` 已通过 62 个文件、2 棵资源树的外部参考校验。本地 Full
`release-verify` 已通过 `45/45`，总测试时间 `114.64 s`；Slim `slim-release-verify` 已通过 `44/44`，
总测试时间 `92.17 s`。最终 Portable 候选的 144 DPI 整窗捕获确认仓库按钮与 Star 提示完整、启用且没有
重叠。Full 候选资产位于 `artifacts/release-0.2.11-candidate-20260904-r1`，发布清单固定如下：

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `ba-click-fx-desktop-0.2.11-Portable-windows-x64.zip` | 1,426,911 | `DA08A38B1C73B8B18A30B99E1619A6FB59AD57D127336A0F5C86F35F2673A0D3` |
| `ba-click-fx-desktop-0.2.11-Portable-windows-x64.zip.sha256` | 117 | `44A56B49E2EA210982B9B30499F2941CA782753CF38AFCF149BFB8A39D0E960F` |
| `ba-click-fx-desktop-0.2.11-setup-windows-x64.exe` | 4,075,657 | `9241FF8C01718FFBB1CA7301C08C8050FF1D93E8BA76870775FFC05F5BF76E01` |
| `ba-click-fx-desktop-0.2.11-setup-windows-x64.exe.sha256` | 114 | `A379836AC8764BF6922C70B3BC25F455BFAB38E3D7B6FD2217E009687BBF091B` |

Slim 继续保留源码构建和测试入口，不提供预编译 Release 下载。Windows SDK `10.0.19041.0`、
`10.0.22621.0`、`10.0.26100.0` 三档 CI 均已通过。发布标签固定在三档 SDK CI 全绿的精确提交；
远端 Release 只包含上述 Full 四资产，并在发布后按相同文件名回下载复核 SHA-256。
