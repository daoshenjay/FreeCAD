# SwMaterial workbench / 材质工作台

A small Python workbench that adds a material browser and one-click assignment
to any solid that has a `ShapeAppearance` (Part, PartDesign, Part::Feature,
Import features, …).  It is deliberately dependency-free: a few Python files
plus SVG icons, no C++ and no build step of its own.

## 功能

| 操作 | 说明 |
|---|---|
| **Material browser** (`Ctrl+Shift+M`) | 右侧停靠面板，列出材料库里**有外观数据**的材质（本机 155 个，颜色预览 + 搜索） |
| **应用到对象** | 给选中的对象赋一个材质 |
| **应用到选中面** | 只给 3D 视图里选中的面赋材质；没有选面时退化为“逐面铺色” |
| **逐面铺色** | 每个面一个同色系深浅，快速做出“材质示意”效果 |
| **清除材质** | 恢复 FreeCAD 默认外观 |
| **贴图库** | 内置 302 张贴图（`bump` 7 张、`pattern` 295 张），带缩略图、分类和搜索，双击即贴 |
| **图片贴图** | 也可以从本机选任意图片；每面独立映射，可用“重复/scale”控制平铺次数 |
| **移除贴图** | 去掉贴图，恢复纯材质外观 |

面板上还有“覆盖材质自带的参数”开关：勾选后可以用 Shininess / Transparency
覆盖材质库里的值，不勾选则完全沿用材质定义（默认不勾选）。

## 结构

```
src/Mod/SwMaterial/
├── Init.py                  # 模块加载（控制台也用得到）
├── InitGui.py               # 工作台定义：工具栏 + 菜单 + 激活时打开面板
├── swmaterial/
│   ├── core.py              # 材质逻辑，不依赖 Qt（可在脚本里直接用）
│   ├── texture.py           # 图片贴图：逐面 UV + Coin 覆盖层 + 文档观察者
│   ├── textures.py          # 内置贴图库的扫描/分类/缩略图
│   ├── panel.py             # 停靠面板（材质页 + 贴图页）
│   ├── paths.py             # 图标与贴图目录
│   └── commands.py          # Gui 命令
├── Resources/icons/*.svg
├── Resources/textures/      # 内置贴图：bump/ pattern/ + _thumbs/ 预览图
└── CMakeLists.txt           # 安装到 <prefix>/Mod/SwMaterial
```

## 使用

启动 FreeCAD 后，工作台下拉框里选择 **SwMaterial**，或者
`View → SwMaterial` 菜单 / 工具栏按钮。选中一个立方体，在面板里挑材质，
点“应用到对象”。

### 贴图库

面板分两页：**材质 / Material** 和 **贴图 / Texture**。贴图页：

1. 顶部下拉框选分类（`凹凸 / bump`、`图案 / pattern`、或“全部”）；
2. 搜索框按文件名过滤；
3. 点缩略图选中，**双击直接贴到选中对象**；
4. `重复 / scale` 控制纹理平铺次数；
5. 「映射 / mapping」组选投影方式（见下）；
6. 「移除贴图」撤销。

自己的图片用「浏览本地图片…」，路径会填进同一个输入框，之后点「贴到选中对象」。

工具栏的 `Texture library…` 会直接打开这一页。

### 按面贴图

在 3D 视图里**选中面**（可以多选，Ctrl 点面），再点「贴到选中对象」，
贴图就只铺在这些面上；不选面时贴整件。

「范围」下拉决定动作语义：

| 范围 | 行为 |
|---|---|
| **选中面（保留其他面）**（默认） | 只改选中的面，其他面**继续用原来的贴图** |
| 选中面（清除其他面） | 先删掉本对象所有贴图，再只贴选中的面 |
| 整个对象 | 忽略面选择，整件替换成这张贴图 |

> 注意：默认的“保留”意味着如果你之前给整件贴过一张图，再选一个面贴第二张，
> 其他面会保持第一张——这不是自动生成，而是“保留原样”。想要只留新图，
> 把「范围」改成“清除其他面”。

**层的语义（基础层 + 覆盖层）**

- 整件贴的那张是**基础层**（`faces: null`）；
- 按面贴的是**覆盖层**（记录具体面号），渲染时后加的覆盖先画；
- 改基础层的参数不会动覆盖层，反之亦然——两者各自记住自己的设置；
- 选中面点「移除贴图」只去掉该面最上层的覆盖，基础层自动露出来；
- **在选中面的状态下改参数，会自动为这些面新建一层覆盖**，基础层保持不变
  （状态栏会提示“新建 N 个面覆盖”）。所以“单独调某个面”就是：选面 → 调参数。

**面板会跟随选择**：选中对象 → 载入基础层参数；选中面 → 载入覆盖该面的那一层；
连贴图缩略图都会高亮成对象正在用的那张。状态栏提示
「已载入 X 的贴图参数（方式，作用范围）」。来回切换两个模型时参数各自还原。

**撤销**：贴图 / 移除 / 参数修改都包在文档事务里，`Ctrl+Z` 可逐步撤销。

**选中高亮**：贴图会盖住 FreeCAD 自带的面高亮，所以面板自己画了一层填充：
选中面时该面填**半透明绿色**（贴在贴图之上，但**正常参与深度测试**，
转到背面就看不到了）；只对“选中面”生效，选中整个对象不画；坐标经过与
贴图相同的显示空间逆变换，对象移动后依然贴合；点空白取消选择自动清除。

数据存在对象的 `TextureLayers` 属性里（JSON），形如：

```json
[{"image": "D:/tex/sand.png", "mapping": {...}, "faces": null},      // 基础层
 {"image": "D:/tex/blue.png", "mapping": {...}, "faces": [5]}]       // 覆盖层
```

### 映射方式

| 方式 | 说明 | 适合 |
|---|---|---|
| **自动 / auto（默认）** | 按面类型选算法：圆柱面→柱面、球面→球面、圆柱体的端盖→平面、其余→盒式 | **大多数情况直接用这个**：圆柱、罐体、管件一次贴完 |
| 盒式 / cubic | 按面法向选主轴，盒子式投影（纹理跨面连续） | 立方体、板件、构件 |
| **参数域 / parametric** | 用面自己的参数域 (u,v) 铺满 0..1，**每个面一张图、与物体大小无关** | logo、铭牌、每面都要完整图案的场合；对应 Bentley 的 Parametric |
| 平面 / planar | 沿「方向/轴」正交投影 | 幕墙、地面、贴纸 |
| 高程投影 / elevation drape | 固定从 +Z 投影 | 地形、屋面 |
| 定向投影 / directional drape | 沿任意方向投影 | 斜面、立面 |
| 柱面 / cylindrical | 绕轴向包裹（u=绕轴角度、v=按**轴向高度**归一化） | 柱、管、罐 |
| 球面 / spherical | 经纬包裹（u=经度，v=纬度） | 球、穹顶 |

自动模式的判定：面的 `Surface` 是 `Cylinder` → 柱面（用该面自己的轴和中心）；
`Sphere` → 球面；`Plane` 且法向与轴向平行、且实体里含圆柱面 → 端盖用平面；
其它一律盒式。纯立方体仍走盒式，不会因为“自动”而变形。

参数域模式（parametric）用 `face.Surface.parameter(point)` 反算每个顶点的 (u,v)，
再按这个面的 `ParameterRange` 归一化到 0..1，所以**任何大小的物体、任何一块面都显示完整一张图**；
反算失败（极端裁剪面）时自动退回盒式。注意它和“世界尺寸”是相反的语义：
1 m 和 4 m 的立方体贴同一张瓷砖图，都是每面 4×4 块砖——这正是 Bentley 里 Parametric 的行为。

参数（「映射」组）：

- **方式**：上表 8 种里的 7 种；
- **方向/轴**：平面/投影的法向；柱面/球面的轴向（默认 `0,0,1`）；
- **原点**：默认取对象包围盒中心，取消勾选后可填具体坐标（球心/柱轴位置）；
- **旋转**：直接旋转 UV（度），**对所有方式都生效**（包括参数域和盒式），
  `+90°` 按钮每次加 90°。参数域/柱面/球面绕面中心转，平面/盒式绕映射原点转，
  所以转完可能需要用「偏移」把图案挪回想要的位置。

参数（「调整」组，对 UV 做后处理）：

| 参数 | 说明 |
|---|---|
| **缩放 U / V** | U、V 分别缩放；勾「等比」时联动。做 300×600 的砖、或修正曲面拉伸都用它 |
| **偏移 U / V** | 图案沿 U/V 平移（单位是“一张图”），用来对齐砖缝、墙角 |
| **翻转 U / 翻转 V** | 按块镜像图案，对称件、背面校正用 |
| **交换 U/V** | 把竖纹变横纹（等价 90° 旋转，但精确） |
| **环绕** | `重复`（默认，平铺）、`钳制`（logo 类，边缘拉伸不重复）、`镜像`（无缝图，正反交替） |
| **实际尺寸** | 勾选后贴图按模型单位铺：`一张 = 1000 mm` 时 1 m 的墙铺 1 张、4 m 的墙铺 4×4 张（Bentley 的世界尺寸语义）。只影响 cubic/planar/柱面/球面，参数域本身按面归一化 |

变换顺序：`交换 → 旋转 → 翻转 → 缩放 → 偏移 → 环绕`（接缝展开在变换之前做，用的是原始 UV）。

**参数改动立即生效**：贴图页底部有个同名开关（默认开）。选中已贴图对象后，
改任何参数都会在 140 ms 内自动重建覆盖层，不用再点「贴到选中对象」；
不想实时预览时把它关掉，改完再点按钮。

哪些参数对哪些方式有效（`-` 表示该方式不用这个参数）：

| 方式 | 方向/轴 | 原点 | 旋转 | 缩放 | 偏移 | 翻转/交换 | 环绕 |
|---|---|---|---|---|---|---|---|
| 自动 / auto | -（用面自身轴） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 盒式 / cubic | -（按面选轴） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 参数域 / parametric | - | -（用曲面参数域） | ✓ | ✓ | ✓ | ✓ | ✓ |
| 平面 / planar | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 高程 / elevation | -（固定 +Z） | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 定向 / directional | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 柱面 / cylindrical | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 球面 / spherical | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

环绕在默认缩放（1）下看不出差别，因为 UV 本来就在 0..1 内；缩放 > 1 或偏移把它推出
0..1 之后才有区别。

`Solid`（3D 体积纹理）还没实现，面板里暂时不提供。

内置贴图放在 `Resources/textures/<分类>/`，加新图只要：

```
Resources/textures/我的分类/xxx.jpg
Resources/textures/_thumbs/我的分类/xxx.jpg   # 可选，96x96 预览图
```

重启 FreeCAD 就会出现在分类下拉框里；没有缩略图时用灰色占位图标。

实现方式（因为 FreeCAD 自己的 Part 渲染路径不吃材质里的图片）：

- 对 `Shape.Faces` 逐面三角化，每个面用自身法向建一个 U/V 基，把投影归一化到
  0..1 → **盒式映射**，每个面完整显示一张图，不会跨面拉伸；
- 拼成一个 `SoIndexedFaceSet`（带 `SoTextureCoordinate2` 和 `SoTexture2`），
  挂在对象自己的 `RootNode` 里、**显示模式开关（ModeSwitch）内部**——
  FreeCAD 隐藏对象就是把这个开关关掉，所以贴图会跟着对象一起隐藏；
  同时每个可着色显示模式（Shaded / Flat Lines）各挂一份，
  `Wireframe` / `Points` 模式不挂，线框模式下不会强加表面；
- 坐标要转换到**显示空间**：这个 fork 把 `Placement` 烘进了 `Shape`（世界坐标），
  视图提供者又用 `SoFCTransform` 把它加回去，所以覆盖层要先乘上这些变换的逆矩阵，
  否则贴图会整体偏移（对象离原点越远越明显）；PartDesign Body 里的特征同样适用；
- 每个顶点按所选映射方式算 UV（`swmaterial/mapping.py`），柱面/球面会做接缝展开，
  再把 V 翻转（QImage 第 0 行是图像顶部，Coin 的 t=0 是纹理底部），贴图不会倒置；
- 改完场景图后要**关掉 Coin 的自动近远裁剪面**（`SoRenderManager.setAutoClipping`
  → `NO_AUTO_CLIPPING`）：本 fork 在运行期修改场景图后，Coin 自动裁剪会算出一个
  空的包围盒，把相机 near/far 压成 `-0.55 / 0.00`，模型整体消失；裁剪由
  `NavigationStyle` 用双精度包围盒负责，所以关掉它是安全的；
- 图片路径、映射参数（JSON）和 scale 存进三个动态属性
  `TextureImage` / `TextureMapping` / `TextureScale`，
  所以**保存后重开文档会自动重建**；`TextureObserver` 在形体变化时也会重建。

脚本里：

```python
import swmaterial.mapping as mp
import swmaterial.texture as tex

mapping = mp.Mapping(mode=mp.CYLINDRICAL, direction=(0, 0, 1), scale=2.0)
tex.apply_texture(obj, r"D:\textures\brick.png", mapping)
tex.remove_texture(obj)
```

## 已知限制（与本仓库版本相关）

- 材质本身只能改**外观参数**（颜色、高光、透明度）。材质库里带的纹理数据
  （`TextureImage`）不会显示在 3D 视图里，所以贴图由上面的覆盖层实现，
  **属于显示效果**：不写入 STEP/IGES/3MF，也不参与布尔运算。
- 贴图按“面”映射：平面/立方体效果最好；柱面/球面在端盖处会退化成一条线
  （和 Bentley 一致，端盖建议换一种映射）；真正的 UV 展开还没做。
- 覆盖层是独立几何，形体大改时会有一次重建（`slotChangedObject` 里触发）；
  模型很复杂时贴图会变慢，建议只对需要的对象贴。
- 面板的材质列表来自 `Materials.MaterialManager()`；没有 Material 模块时
  自动退回内置的 10 个常用材质。
- `bump` 分类里多是灰度凹凸/位移图（noise、concrete、rough…），目前只能当
  **颜色贴图**用，看起来是灰色花纹；真正的凹凸/法线效果要等渲染器支持。
- 逐面材质按 `Shape.Faces` 顺序写 `ShapeAppearance`，与 GUI 的
  “Appearance → per face” 一致。

## 安装

**方式一：当作一个普通 Python 工作台（最快，适合试用）**

把这个目录整个复制到用户 Mod 目录，重启 FreeCAD：

```powershell
Copy-Item -Recurse src\Mod\SwMaterial "$env:APPDATA\FreeCAD\Mod\"
```

`swmaterial/paths.py` 会先在 `getResourceDir()/Mod/SwMaterial` 找图标，
找不到就退回到模块自己旁边的 `Resources/icons`，所以单目录拷贝也能用。

**方式二：跟着源码树构建安装**

```powershell
pixi run cmake --build build/debug --target SwMaterial
pixi run cmake --install build/debug
```

安装规则：Python 文件 → `<prefix>/Mod/SwMaterial`，
图标 → `<prefix>/data/Mod/SwMaterial/Resources`（和其它模块一致）。
注意 `cmake --install` 需要完整的构建产物，本机当前只有预编译的 prefix，
所以实际是把文件直接拷到 `.pixi/envs/default/Library/Mod/SwMaterial` 和
`.../Library/data/Mod/SwMaterial/Resources` 验证的。


`cmake --install` 会把模块复制到 `<prefix>/Mod/SwMaterial`，重启 FreeCAD 后生效。

## 卸载

删除 `<prefix>/Mod/SwMaterial`，并从 `src/Mod/CMakeLists.txt` 去掉
`add_subdirectory(SwMaterial)`。


## 自测

`tests/` 下有两个验收脚本，在 GUI 里跑（每个都会打印 PASS/FAIL 并退出）：

```powershell
powershell -NoProfile -File src\Mod\SwMaterial\tests\run_test.ps1 `
    -Script src\Mod\SwMaterial\tests\qa_layers_ui.py      # 层语义/持久化/撤销/面板
powershell -NoProfile -File src\Mod\SwMaterial\tests\run_test.ps1 `
    -Script src\Mod\SwMaterial\tests\qa_robustness.py     # 健壮性/多对象/性能
```

运行器带硬超时（默认 240 s）：超时或脚本抛异常时会强杀 FreeCAD 进程，
不会留下需要手动关闭的窗口。

覆盖的点：UV 变换数学、JSON 兼容（旧的单图属性、损坏 JSON）、基础层/覆盖层
增删改、保存重开、隐藏/显示模式、多对象、两个对象参数互不串、局部撤销、
缺失贴图文件、非法面号、对象删除、稠密网格构建耗时（<2 s）。




