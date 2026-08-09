# 内嵌资产清单

Release 可执行文件内嵌以下四个 PNG 容器。它们在启动时仅于内存中解码，不作为旁置文件进入包。

| 名称 | 尺寸 | 字节数 | SHA-256 |
| --- | ---: | ---: | --- |
| `FX_TEX_Circle_01.png` | 512x512 | 23,540 | `F8675E0A16959EDA829AE7D516FB609A4D45434520866466D04B78745F0BADD2` |
| `FX_TEX_Grad_Ring3.png` | 256x128 | 19,743 | `517236C7C818A3715F8BA03EC316853BEC92FFD6E032B8E5D21DAEDFFC809684` |
| `FX_TEX_Triangle_02_1.png` | 256x128 | 4,582 | `0EB35FDA5710344BEDB5713B0B197B1C190EC4D8851EF8DD916B4E17DE39A068` |
| `FX_TEX_Trail_03.png` | 512x512 | 54,161 | `16001511757E7007F43DB9613E24144B5E8D726239DE0262F55D9E14C0F00FEB` |

这些纹理来自 Blue Archive `FX_Touch` 的本地游戏资源提取与 Unity 重建审计，两棵参考资源树中的
容器已经逐字节校验一致。仓库的 GNU GPL v2 仅覆盖本项目自行编写的代码，不主张拥有这些第三方美术
资产，也不授予其再分发许可。

因此，包含这些字节的 Alpha 包仅供本地研究和测试。在取得相应权利或替换为可分发资产前，不应将
该二进制包作为公开 Release 发布。外部 Unity 工程、prefab、mesh、材质、shader、截图和游戏目录均不
进入 Alpha 包。
