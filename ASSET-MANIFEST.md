# 内嵌资产清单

Release 可执行文件内嵌四个无文件头的 raw LZ4 Block。它们不是 PNG 容器或 Base64 文本，不作为
旁置文件进入包。Host 启动时逐张解压为 RGBA8 texel，直接上传为 D3D11 immutable sRGB 纹理，随后
释放该张纹理的 CPU 缓冲区。

| 名称 | 尺寸/格式 | LZ4 字节数 | 解码字节数 | 解码 texel SHA-256 |
| --- | --- | ---: | ---: | --- |
| Center Disk | 512x512 RGBA8 | 31,743 | 1,048,576 | `9721F56BD0EF000C6A8D9ACC718B0D20FE845F4EF849275C097544188E23C3FE` |
| Dissolve Ring | 256x128 RGBA8 | 83,306 | 131,072 | `9EED3861862413CA47757182CF5C36A9BE553E53227BED7FA4BC12DD0D480B01` |
| Triangle Atlas | 256x128 RGBA8 | 4,413 | 131,072 | `B2C18E0C1377530C7870EFE02CE48C75C2142C7262C9FF668E71B85AE471269E` |
| Trail | 512x512 RGBA8 | 189,378 | 1,048,576 | `DE8B8B1039080784BB164655424F8532EAC86A85AA9D90FB677EA02C2DB87E99` |

LZ4 载荷合计 308,840 字节；解码 texel 合计 2,359,296 字节。Ring 保留完整 RGBA8，而不压缩成
单通道，因为当前 Dissolve 材质同时采样 RGB 和 Alpha；Trail 的 Alpha 是由原 RGB 每列峰值归一化
得到的透明覆盖率，用于保持发射亮度与桌面透明传输相互独立。

开发期生成器在读取参考 PNG 后先锁定下列源容器 SHA-256，再解码、派生 Trail coverage 并压缩。
这些 PNG 不进入源码、EXE 或发布包：

| 参考文件 | 源 PNG SHA-256 |
| --- | --- |
| `FX_TEX_Circle_01.png` | `F8675E0A16959EDA829AE7D516FB609A4D45434520866466D04B78745F0BADD2` |
| `FX_TEX_Grad_Ring3.png` | `517236C7C818A3715F8BA03EC316853BEC92FFD6E032B8E5D21DAEDFFC809684` |
| `FX_TEX_Triangle_02_1.png` | `0EB35FDA5710344BEDB5713B0B197B1C190EC4D8851EF8DD916B4E17DE39A068` |
| `FX_TEX_Trail_03.png` | `16001511757E7007F43DB9613E24144B5E8D726239DE0262F55D9E14C0F00FEB` |

这些纹理通过 Blue Archive `FX_Touch` 的本地资源提取与 Unity 重建审计完成视觉校准，两棵参考资源树
中的源容器已经逐字节校验一致。其内容只由圆盘、环形渐变、三角形和拖尾光斑等基础几何图形组成，
不包含角色、场景、标识或其他具有特定表达的美术内容。

本项目将上表四组解码 SHA-256 锁定的 texel 及其 RLE、LZ4、Base64 或其他无损编码形式作为可自由
使用的简单几何纹理数据。任何人均可出于商业或非商业目的复制、使用、修改、重新编码和再分发这些
数据，无需另行授权或署名；它们可以随本项目源码、测试包和公开 Release 一同分发。

该许可仅适用于上表明确列出的四组几何纹理数据，不延伸到外部 Unity 工程中的其他资源。外部 PNG、
prefab、mesh、材质、shader、截图和游戏目录仍不进入发布包。本项目自行编写的代码继续适用仓库根目录
中的 GNU GPL v2。
