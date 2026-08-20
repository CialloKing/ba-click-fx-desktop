# OBS 透明特效输出

标准构建可把点击和拖尾作为独立透明层发送给 OBS。发送者名称固定为
`ba-click-fx-desktop`，输出合同为
`BGRA8 + sRGB + extended premultiplied alpha + FX-only v2`：

- 空闲背景为 `(0, 0, 0, 0)`；
- 有效像素满足 `0 <= RGB <= 1`、`0 <= Alpha <= 1`，允许 `RGB > Alpha`
  和 `Alpha = 0, RGB > 0` 来携带加法发光；
- 不包含也不依赖桌面或游戏画面，WGC 不可用时仍可发送特效；
- 固定尺寸运行时保持同一共享句柄，尺寸变化或设备恢复才允许重建。

因此 OBS 必须负责捕获游戏或桌面，并把 Spout2 源作为最上层透明特效叠加。

## 插件准备

项目不会下载、安装或更新 OBS 插件。Control Center 的“系统”页只做只读探测：

- 从正在运行的 OBS 路径和 OBS 卸载注册信息定位 `win-spout.dll`；
- 显示插件版本和位数；
- OBS 运行时检查模块是否真正加载；
- 无法读取模块列表时显示“无法确认”，不会误报为插件缺失。

插件可从 [Off-World-Live 官方发布页](https://github.com/Off-World-Live/obs-spout2-plugin/releases/)
获取，安装方式以[官方说明](https://github.com/Off-World-Live/obs-spout2-plugin/blob/master/README.md)
为准。官方 `1.12.0` 发布说明列出的构建基线是 OBS `32.1.2`；OBS `32.2.2`
是否可用仍以本机加载日志和录像验收为准。

## OBS 场景设置

1. 在 OBS 中先添加游戏捕获、窗口捕获或显示器捕获，并置于来源列表底部。
2. 添加 `Spout2 Capture`（内部类型 `spout_capture`），置于来源列表顶部。
3. 选择发送者 `ba-click-fx-desktop`。
4. 将 Composite Mode 设为 `Premultiplied Alpha`。旧的 `Default` 或 `Opaque`
   设置会破坏透明输出。
5. 右键 Spout2 来源，把 `Blending Mode` 保持为 `Normal`。不要使用 `Add`；扩展预乘
   像素已经携带加法能量，再次切换来源混合会改变 Cross2 遮挡和整体视觉。
6. 对 Spout2 源执行 `Transform -> Fit to Screen`，确认其边界与 OBS 画布一致。
7. 在 Control Center 的“系统”页启用“OBS 透明特效输出”，检查发送者状态和插件状态。

空闲时预览应只显示底层捕获；点击或拖动时只出现特效，衰减结束后底层画面应完全恢复。

## 显式迁移旧场景

Control Center 不会修改 OBS 场景。需要迁移旧源时，先关闭 OBS，再显式运行：

```powershell
pwsh -NoProfile -File tools/repair-obs-spout2-scene.ps1 `
  -ScenePath "$env:APPDATA\obs-studio\basic\scenes\<场景集合>.json" `
  -Width 1920 -Height 1080 -CheckOnly
```

`-CheckOnly` 返回 `0` 表示无需修改，返回 `2` 表示需要迁移且不会写盘。确认后移除
`-CheckOnly` 再运行。脚本只处理绑定 `ba-click-fx-desktop` 的 `spout_capture`，把
Composite Mode 改为 `4`（Premultiplied Alpha）、来源混合恢复为 `normal`，并修复画布边界；
其他 Spout2 源不变。
写入前会在原文件旁生成 `.bak` 备份，单元素 `items` 仍保持 JSON 数组。

## 独立发送验收

标准构建可在不启动 OBS 时先运行进程边界探针：

```powershell
ctest --test-dir build/alpha-x64 -C Release `
  -R '^spout2_process_boundary$' --output-on-failure
```

该探针用独立进程采集“空闲、点击/拖尾、衰减结束”三阶段，并验证发送者持续存在、
共享句柄稳定、空闲全透明、活动 Cross2 Alpha 非零、存在 `RGB > Alpha` 与
`Alpha = 0, RGB > 0` 的加法像素，并最终恢复透明。
它不能替代 OBS 插件加载、合成模式和录像像素验收。

## 扩展预乘逐像素验收

亮度验收必须使用固定龄特效帧和只包含纯色背景、Spout2 源的隔离场景；不要用显示器捕获
录制 OBS 自身。接收器探针的 `--capture-output=<path>` 可保存紧密排列的原始 BGRA8 帧。
为同一帧分别保存黑色 `(0,0,0)`、灰色 `(96,96,96)`、白色 `(255,255,255)` 和彩色
`(32,80,144)` 场景 PNG 后，按以下结构编写清单：

```json
{
  "schemaVersion": 1,
  "contract": "bgra8-srgb-extended-premultiplied-fx-only-v2",
  "rawFrame": {
    "path": "active-frame.bgra",
    "width": 3840,
    "height": 2160,
    "format": 87
  },
  "cases": [
    {"name": "black", "backgroundRgb": [0, 0, 0], "image": "black.png"},
    {"name": "gray", "backgroundRgb": [96, 96, 96], "image": "gray.png"},
    {"name": "white", "backgroundRgb": [255, 255, 255], "image": "white.png"},
    {"name": "color", "backgroundRgb": [32, 80, 144], "image": "color.png"}
  ]
}
```

```powershell
python -B tools/verify-obs-spout2-composite.py artifacts/<证据目录>/manifest.json
```

验证器逐像素检查 `C = clamp(S + B * (1 - A), 0, 1)`，并单独确认
`RGB > Alpha`、`Alpha = 0, RGB > 0` 未被 OBS 压回普通预乘范围。

## 录像证据复核

本机 OBS 验收目录应包含 `frame-baseline.png`、
`frame-idle-sender-connected.png`、至少一个 `frame-active-*.png`、
`frame-final-transparent.png` 和同目录录像。使用下列命令复核：

```powershell
python -B tools/verify-obs-spout2-evidence.py `
  artifacts/<证据目录> artifacts/<证据目录>/<录像>.mp4
```

录像复核器会验证空闲和结束帧逐像素等于背景、活动阶段存在特效、没有黑色矩形，
并解码录像检查完整的“空闲、活动、恢复”三阶段。需要自动化 OBS 截图或录像时，
`tools/obs-websocket-request.mjs` 可从本机 OBS WebSocket 配置读取认证信息并执行单个
本地请求；它不会把密码写入命令行或输出。

## 当前验收边界

本轮闭环只覆盖 Windows 10 `19045`、RTX 4060、单显示器 SDR、OBS `32.2.2`
和 OBS Spout2 插件 `1.12.0`。以下项目保持 `Not Run`：HDR、多显示器、跨 GPU、
Windows 11、其他 OBS/插件版本，以及可复现的 Spout2 CI 构建。
