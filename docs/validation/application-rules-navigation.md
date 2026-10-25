# 程序规则页签验证 / Program rules tab validation

日期 / Date: 2026-10-25。实现提交 / Implementation commit: `1f69f47`。

本变更纳入 0.3.3，只调整控制中心导航与规则页布局，配置仍为 schema 21，Host 自动停用逻辑不变。下述记录来自版本号仍为 0.3.2 的实现阶段；0.3.3 发布构建与包检查另见 [VALIDATION.md](../VALIDATION.md)。
This change is included in 0.3.3 and updates Control Center navigation and layout without changing schema 21 or Host suppression behavior. The records below come from implementation checks with version 0.3.2; release builds and package checks are recorded separately in [VALIDATION.md](../VALIDATION.md).

## 构建与回归 / Builds and regression

使用独立的 `build/ui-navigation-full` 和 `build/ui-navigation-slim`，保留现有 IDE 构建目录。
构建环境：Visual Studio 2026、MSVC 19.51.36257、Windows SDK 10.0.28000.0。
Both variants used separate build directories, leaving the existing IDE cache intact.

| 检查 / Check | Full Release | Slim Release |
| --- | --- | --- |
| `bafx_control_center` / `bafx_control_center_tests` | 通过 / Passed | 通过 / Passed |
| CTest `control_center_activation` | 1/1，9.71 s | 1/1，9.46 s |
| 测试程序内部用例 / Internal cases | 68/68 | 68/68 |
| 原生控件位图 / Native control bitmaps | 134 | 134 |

执行命令 / Commands (PowerShell):

```powershell
$env:VCPKG_ROOT = 'D:\C++project\SDK\vcpkg'
cmake --preset x64 -B build/ui-navigation-full -G "Visual Studio 18 2026" -DCMAKE_SYSTEM_VERSION:STRING=10.0.28000.0
cmake --preset x64-slim -B build/ui-navigation-slim -G "Visual Studio 18 2026" -DCMAKE_SYSTEM_VERSION:STRING=10.0.28000.0
foreach ($variant in @('full', 'slim'))
{
    cmake --build "build/ui-navigation-$variant" --config Release --target bafx_control_center bafx_control_center_tests -- /m:2 /nodeReuse:false
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $variant" }
    $env:BAFX_UI_ARTIFACTS = Join-Path (Get-Location) "build/ui-navigation-$variant/ui-artifacts"
    ctest --test-dir "build/ui-navigation-$variant" -C Release -R '^control_center_activation$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Regression failed: $variant" }
}
node tools/verify-readme.mjs
```

`node tools/verify-readme.mjs` 与 `git diff --check` 通过。
Documentation verification and whitespace checks passed.

## 布局与导航 / Layout and navigation

- 覆盖中英文 × 96 / 144 / 192 DPI × 860×600 / 960×600 逻辑客户区，六页及高级参数各分组均检查可见控件边界。
- 六个导航项全局可见、顺序固定、字体一致；原生单选按钮分组顺序与界面一致，每页只有自己的页签选中。
- 每个页签容纳测量文字宽度加 32 逻辑像素，导航无重叠，末项与右侧 24 逻辑像素边距对齐。规则页总开关从左侧 24 逻辑像素开始，底部操作按钮也保持 24 逻辑像素的右侧与底部边距。
- 真实按钮通知覆盖六页两两切换；断开时规则编辑禁用，切换不产生配置补丁或代次变化。离开快捷键录制页会清理本地录制状态；离开规则页后进程刷新仍能正常完成。
- 缓存 Host 状态覆盖连接后的编辑控件启用、命中原因和规则行展示，并保留重复刷新不重写控件的回归。

The matrix covers both languages, three layout DPIs and both client sizes. Assertions check visible-control bounds, navigation order, native radio grouping, fonts, exclusive selection, measured label fit and margins. Button notifications exercise every page-to-page transition, disconnected edit restrictions, unchanged configuration, recording cleanup and asynchronous process-refresh completion. Existing incremental-refresh checks remain enabled.

每个构建生成 132 张页面 / 分组位图及 2 张确认框位图。使用现有原生控件渲染输出，查看了以下系统页 / 规则页样本：

- Full：96 DPI 英文最小窗口两页；144 DPI 中文默认窗口两页；192 DPI 英文默认窗口系统页、中文最小窗口规则页。
- Slim：96 DPI 英文最小窗口两页。

所查看样本中，页签文字完整、导航与内容未重叠，系统页旧入口和规则页返回按钮已移除。位图存于各构建目录的 `ui-artifacts/`，命名为 `语言-DPI-逻辑宽度-页编号-分组.bmp`（系统页编号 4，规则页编号 5）。

Each variant produced 132 page/section images and two confirmation images. Visual inspection covered the Full English minimum-size pages at 96 DPI, Chinese default-size pages at 144 DPI, selected 192-DPI pages, and both Slim English minimum-size pages. Labels fit and navigation stayed clear of content; obsolete buttons were absent. Images are local build artifacts.

200% 默认窗口的离屏尺寸可能超过当前显示器的原生跟踪上限；测试用 `SWP_NOSENDCHANGING` 固定客户区尺寸，并断言实际宽高。产品最小客户区仍为 860×600，未改变窗口大小限制。
Off-screen tests bypass native tracking limits and assert the exact client size. Product minimum-size behavior is unchanged. These are native-control renders, not physical multi-monitor DPI-transition tests.

## 实际进程联动 / Live process interaction

在 `build/ui-navigation-interaction` 使用本轮 Full 控制中心、已有 0.3.2 Full Host 和独立配置，通过原生控件通知驱动编辑，使用 Host IPC 核对结果；没有修改安装版或用户配置。测试关闭原始输入，使用 light-background 模式并保持手动关闭特效。

- Host 未连接时进入规则页：页签选中，编辑禁用。
- Host 连接后开启总开关，从运行列表添加测试进程：完整路径规则保存成功，Host 命中且控制中心显示原因。
- 连接状态下切换六页：配置代次保持不变。
- 停用、重新启用、删除规则：每次均写入成功，自动停用状态随之解除、恢复、解除；schema 21 和手动关闭特效保持不变。
- 所有测试进程已退出。记录：`build/ui-navigation-interaction/interaction.json`；复查脚本：同目录 `probe.py`。

The new Full UI was exercised against the existing 0.3.2 Full Host with isolated files. Native control notifications verified disconnected navigation, enabling the feature, adding a running process by path, match-reason display, configuration-preserving navigation, disable/re-enable and deletion. Host IPC confirmed each saved edit and suppression transition. Schema 21 and the manual disabled-effects setting were preserved, and all test processes exited.

实现阶段未重新执行完整 Host / 图形回归、物理多显示器 DPI 切换、OBS 合成或 WGC 启停验收，也未打包或发布。发布阶段的构建、回归与包检查见 [VALIDATION.md](../VALIDATION.md)；既有运行时验证见[程序规则验证](application-rules.md)。
The implementation phase did not rerun the complete Host/graphics suite, physical monitor transitions, OBS composition or WGC lifecycle acceptance, and did not package or publish a release. Release-stage builds, regression and package checks are recorded in [VALIDATION.md](../VALIDATION.md); prior runtime evidence remains in [program rules validation](application-rules.md).
