# SWDWG

基于 **LibreDWG + OpenCASCADE** 的 FreeCAD 原生 DWG 导入模块,核心导入管线为纯 C++ 实现(不经 Python 几何包装层)。

## 特性

- **纯 C++ 导入管线**:`dwg_read_file → parse → 实体提取 → OCC 几何构建 → 图层聚合 → 对象创建`,万级实体场景下性能与可控性更好。
- **文本乱码根治**:GBK / UTF-8 / Latin-1 多候选解码 + 评分择优(TEXT / MTEXT 全路径)。
- **高保真几何**:bulge 弧、圆→椭圆、ARC 三点法、SPLINE(二次 B 样条)、退化几何回退链。
- **HATCH 图案填充还原**:`.pat` 无限线族 → 边界求交裁剪 → 虚线相位。
- **文字与尺寸**:TEXT/MTEXT → `SWDWG::DWGText`,`DIMENSION` → `SWDWG::DWGDimension`;支持 shx→TrueType 字体映射、SHX 字形几何化渲染与 TTF(`SoAsciiText`)两条渲染路径。
- **图层聚合**:同一图层样式(颜色/线宽/线型)的实体聚合成一个 `Part::Feature`,把对象规模从"实体数"降到"样式组数"。
- **复杂线型**:L0 名称 → L1 16-bit 量化 → L2 几何展开 三层映射;支持复杂线型中 SHX shape/TTF 文字元素。
- **性能优化**:块几何零拷贝复用、导入/restore 批量窗口抑制、渲染缓存与精确失效、纯 C++ 导入、状态栏进度 UI、测宽/字形走缓存与轻量计算等。
- **海量 3DFACE 网格化**:大面量图纸(如地形)的顶层 3DFACE 自动聚合为按图层/颜色的 `SWDWG::DWGSurfaceMesh`(单对象、单 `SoIndexedFaceSet`、共享顶点 + 逐顶点平滑法线),避免逐面 BRep/Coin 的开销与内存尖峰。
- **SHX 字形预览**:`SWDWG_ViewSHX`("View SHX Font…")命令把 `.shx` 中的全部字形/shape 以网格方式绘制出来,便于校验字形解码。

## 总体架构

```
DWG 二进制
  → 解析层   App/DWGImporter.cpp::parse()   (图层/STYLE/DIMSTYLE/LINETYPE/块表)
  → 提取层   App/DWGExtract.cpp             (Dwg_Object → Entity,零 FreeCAD 依赖)
  → 构建层   App/DWGShape.cpp / DWGHatch    (Entity → OCC TopoDS_Shape)
  → 编排层   App/DWGImporter.cpp::expand()  (块展开 / 矩阵变换 / 图层样式分组 / 对象创建)
  → FreeCAD  App::Document:
             DWGLineGroup(Part::Feature) · SWDWG::DWGText · SWDWG::DWGDimension
             · SWDWG::DWGSurfaceMesh(3DFACE 网格)
  → GUI      Gui::Document:
             ViewProviderPart · ViewProviderDWGText · ViewProviderDWGDimension
             · ViewProviderDWGSurfaceMesh
```

依赖方向:**App 层不依赖 Gui 层**;Gui 层只通过属性系统读取 App 对象,不直接触碰 LibreDWG 数据结构。

| DLL | 主要内容 | 依赖 |
|---|---|---|
| `SWDWG`(App,Python 模块) | 导入管线、几何构建、文字/尺寸对象、预览读取器 | LibreDWG · OpenCASCADE · FreeCAD App/Base · PyCXX |
| `SWDWGGui`(Gui,Python 模块) | 导入命令与样式回填、文字/尺寸视图提供者 | SWDWG App · Coin3D · FreeCAD Gui |

## 构建集成

前置依赖:FreeCAD 源码树(App/Gui/Base)、LibreDWG、OpenCASCADE、Coin3D(Qt)。

1. 将本模块放入 FreeCAD 源码目录 `src/Mod/SWDWG`,随 FreeCAD 顶层 CMake 一起构建(模块自带 `CMakeLists.txt`,生成 `SWDWG` / `SWDWGGui` 两个 Python 扩展目标)。
2. LibreDWG 通过 `find_package` 或子目录方式集成(MSVC 需按模块 `CMakeLists` 传入 ABI 宏)。
3. `SWDWGGui` 仅在 GUI 会话加载;纯命令行(`FreeCADCmd`)也能通过 `SWDWG` 导入 DWG。

> 单测/工具:`tests/` 下提供可脱离 FreeCAD/OCC 独立编译运行的单测与字形探测工具。

## 使用

SWDWG 是**文件格式模块**(库模块),不是工作台:它不往工作台选择器里添加条目,DWG 的入口就是标准的文件菜单,任何工作台里都能用。

### GUI(FreeCAD 图形界面)

1. 菜单 **文件 → 导入…**,在过滤器里选 `Autodesk DWG (*.dwg *.DWG)` 后选中 `.dwg` 文件
   (**文件 → 打开** 也可以,文件对话框里同样有 DWG 过滤器;DWG 只有 SWDWG 一个模块声明,
   不会弹模块选择框);
   - 无活动文档时自动新建以 DWG 文件名命名的文档;
   - 导入过程在主窗口**底部状态栏**以进度条显示(短任务不弹任何窗口),完成后状态栏显示耗时与对象数;
   - 导入结果按图层/样式聚合成图层组,颜色、线宽、线型回填到视图;
   - 大图纸导入不做逐对象 undo(防止内存爆炸),需要回退时删除对象/文档。
2. 导出:**文件 → 导出…**,格式选 `Autodesk DWG (*.dwg)`(由 `SWDWG/Init.py` 注册、`export_dwg.py` 提供,直接写 DWG,不经 DXF)。
3. 诊断/开发命令没有菜单入口,在 Python 控制台里调用:
   ```python
   Gui.runCommand("SWDWG_Import")         # 等价于"新建文档并导入 DWG"
   Gui.runCommand("SWDWG_Export")         # 导出当前/选中对象
   Gui.runCommand("SWDWG_ViewSHX")        # 网格绘制 .shx 里的全部 shape/glyph
   Gui.runCommand("SWDWG_MultiInstance")  # SoMultipleInstance 示例
   ```

### 命令行

```python
# FreeCADCmd
import SWDWG
SWDWG.readDWG("a.dwg")              # 预览读取(返回 dict)
SWDWG.open("a.dwg")                 # 新建唯一名文档并导入
SWDWG.insert("a.dwg", "docName")    # 导入到指定/活动文档
```

## 目录结构

```
SWDWG/
├── App/            # SWDWG DLL:导入管线、几何、文字/尺寸对象、预览读取器
├── Gui/            # SWDWGGui DLL:命令/视图提供者、导入与打开进度
├── tests/          # 独立单测与字形探测工具
├── Init.py         # App 模块 Python 入口(注册 .dwg 导入/导出)
├── InitGui.py      # GUI 模块入口(注册命令与视图提供者,不注册工作台)
├── CMakeLists.txt  # 模块构建
└── LICENSE         # 许可
```

## 许可

见 [LICENSE](LICENSE)。
