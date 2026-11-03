# SwGlb

把实体连同 **SwMaterial 的贴图**导出成 glTF 2.0（`.glb`）。

## 为什么需要它

FreeCAD 自带的 GLB 导出是：

```
Import.export  →  ExportOCAF2  →  OCCT RWGltf_CafWriter
```

- 几何只有 `POSITION` + `NORMAL`，**没有 `TEXCOORD_0`**（B-rep 没有 UV）；
- 材质颜色只有在调用方传 `(对象, 颜色列表)` 元组时才写入，菜单导出传的是纯对象，
  所以 `materials` 直接是空的；
- SwMaterial 的贴图只存在于 Coin 场景节点和 `TextureLayers` 属性里，
  任何导出器都看不到。

实测同一个立方体：

| 导出方式 | materials | images | 图元属性 |
|---|---|---|---|
| `Import.export([cube], "a.glb")` | 0 | 0 | POSITION, NORMAL |
| `Import.export([(cube, colors)], "b.glb")` | 1 | 0 | POSITION, NORMAL |
| **SwGlb** | 2 | 2 | POSITION, NORMAL, **TEXCOORD_0** |

## 用法

1. 切到 **SwGlb** 工作台；
2. 选中要导出的实体（不选 = 导出所有可见实体）；
3. 点工具栏「导出 GLB（带贴图）…」，选保存路径。

导出结果包含：

- 每个对象一个 mesh，按「贴图层」拆成多个 primitive；
- 贴图层的 UV 与 3D 视图**完全一致**（复用 `swmaterial.mapping` 的算法，
  含缩放/偏移/旋转/翻转/交换/环绕/实际尺寸）；
- 每张贴图内嵌进 GLB（JPEG/PNG），材质用 `baseColorTexture`；
- 没贴图的面导出 `ShapeAppearance` 的漫反射色（sRGB→线性转换后写入
  `baseColorFactor`），所以纯材质色也能带出去。

## 实现

不修改 FreeCAD 源码，只复用它的 API：

- 几何：`Shape.tessellate()`，自适应精度（包围盒对角线 × 0.002，夹在
  0.05–3 mm），和视口同一套；
- 贴图与映射：`swmaterial.texture.read_layers()` +
  `swmaterial.mapping`（所以 UV 不会跑偏）；
- 颜色：`ViewObject.ShapeAppearance[i].DiffuseColor`；
- glTF 容器：`swglb/gltf.py`，纯 Python（`json` + `struct`），无第三方依赖，
  按规范写 12 字节头 + JSON chunk（空格补齐）+ BIN chunk（零补齐）。

## 坐标与性能

- glTF 是 **Y 轴向上**，FreeCAD 是 Z 轴向上，导出时统一做 `(x, y, z) → (x, z, -y)`
  （法线同样处理），所以平躺的模型在查看器里仍然平躺。
- 颜色/单一贴图且映射只依赖坐标的对象，用**整形状一次三角化**的快速路径；
  只有需要逐面参数（parametric/auto）或逐面贴图时才逐面细分。
- 实测（48 个对象、8449 面、8922 三角形、31 个贴图）：首次实现 75 s，
  优化后 **3.5 s**。主要靠：`shape.Faces` 只取一次（原先逐面访问会重建整个
  面列表，O(n²)）、numpy 批量算法线、`array.array` 代替 `struct.pack(*values)`、
  缓存 `ShapeAppearance` 与颜色。

## 限制

- 只导出实体（Part shape）；网格对象、点云不导；
- 不导出透明度/金属度等 PBR 扩展参数，统一 `metallic=0`、`roughness=0.9`；
- 单个对象超过 40 万面会跳过并在 Report view 提示；
- 贴图按绝对路径读取，文件不存在时该组退化成面颜色。

