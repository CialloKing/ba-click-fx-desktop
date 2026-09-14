# README 预览素材 / Preview assets

`native-click-trail.png` 来自产品版本 0.2.15、代码修订 `41eed6f` 的原生 D3D11 渲染器，
使用已有 GPU capture 工具的 `drag-trail` 场景，在 140 ms 取样。直接复制工具生成的 `FinalOverlay.png`，未修改像素。
该工具通过 WARP 离屏运行，将线性颜色转为黑底 sRGB PNG；此图用于展示特效，不是桌面实拍或硬件验收证据。

`native-click-trail.png` was produced by the native D3D11 renderer in version 0.2.15, revision `41eed6f`,
using the existing GPU capture tool's `drag-trail` case at 140 ms. It is an unchanged copy of `FinalOverlay.png`.
The tool uses WARP offscreen rendering and converts linear colors to an sRGB PNG over black.
This illustrates the effects; it is not a desktop screenshot or hardware acceptance evidence.

从仓库根目录重现（先完成 Full Release 构建）；Slim 将 `x64` 换成 `x64-slim`。
To reproduce from the repository root after a Full Release build, run the commands below. For Slim, replace `x64` with `x64-slim`.

```powershell
$previewRevision = git rev-parse --short HEAD
& .\build\x64\src\capture\Release\ba-click-fx-gpu-capture.exe `
    --output=artifacts/local/readme-preview --revision=$previewRevision --case=drag-trail
Copy-Item -LiteralPath artifacts/local/readme-preview/0140ms/FinalOverlay.png `
    -Destination docs/images/native-click-trail.png
```

更新图片时同步记录实际产品版本与修订，并检查两种语言 README 的显示。
When updating the image, record the actual product version and revision and inspect both README previews.
