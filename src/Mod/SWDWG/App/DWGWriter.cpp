//! DWG export (writer) - implementation.
//!
//! 导出功能：本文件为 DWG 导出新增（不经 DXF），对应导入侧的 DWGExtract：
//!   Entity -> dwg_add_* -> dwg_write_file(AC1032 / R2018)
//!
//! 说明：
//!  * 只用 LibreDWG 的公开写接口；公共属性（图层/颜色/线宽）通过实体结构里
//!    的 `parent`（Dwg_Object_Entity）直接赋值，避免 dynapi 的字段尺寸歧义；
//!  * 写完后主动补 EXTMIN/EXTMAX、LIMMIN/LIMMAX、TILEMODE 和 *ACTIVE 视口，
//!    否则 CAD 打开时视图是空的（LibreDWG 自己不计算范围）。

#include "DWGWriter.h"

// 导出功能：DWGInternal.h 先按 libredwg.lib 的构建配置设置 HAVE_WCHAR_H /
// SIZEOF_WCHAR_T 等 ABI 宏，然后才 include dwg_api.h，必须保持这个顺序。
#include "DWGInternal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>
#include <string>
#include <vector>

#include <Base/Console.h>

namespace SWDWG
{
namespace
{



//! AutoCAD 标准线宽序列（1/100 mm），索引与 DWG 的 linewt 编码一致。
const int kLineWeights[] = {0,  5,  9,  13, 15, 18, 20, 25, 30, 35, 40, 50,
                            53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200};

//! DWG linewt: BYLAYER
const unsigned char kLineWeightByLayer = 0x1f;

//! 导出功能：圆周率（不依赖 _USE_MATH_DEFINES / M_PI）
constexpr double kPi = 3.14159265358979323846;

//! 导出功能：TrueType 字高补偿系数。
//! DWG 的 TEXT/MTEXT 字高对 SHX 字体是"字框（cap height）"，对 TrueType 字体
//! CAD 也按大写字母高度换算；而 FreeCAD/SWDWG 渲染 TTF 回退字体时按 em 高度
//! 排版，cap 高约 0.7 em —— 同一数值写出去，CAD 里会显得大约 1/0.7 倍。
//! 这里乘 0.7，让导出字号与 FreeCAD 里看到的大小一致（要微调改这一个数即可）。
constexpr double kTrueTypeHeightScale = 0.7;

//! 导出功能：把 1/100 mm 的线宽映射成 DWG 的 linewt 索引。
unsigned char lineWeightIndex(int hundredths)
{
    if (hundredths <= 0) {
        return kLineWeightByLayer;
    }
    int best = 0;
    int bestDiff = std::abs(hundredths - kLineWeights[0]);
    for (int i = 1; i < (int)(sizeof(kLineWeights) / sizeof(kLineWeights[0])); ++i) {
        const int diff = std::abs(hundredths - kLineWeights[i]);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = i;
        }
    }
    return (unsigned char)best;
}

inline dwg_point_3d toPoint(const Vec3& v)
{
    dwg_point_3d p;
    p.x = v.x;
    p.y = v.y;
    p.z = v.z;
    return p;
}

//! 导出功能：实体的真彩色优先（同一图层上会有多组不同颜色），
//! 其次才是显式 ACI，都没有就按图层（BYLAYER）继承。
void applyCommon(Dwg_Object_Entity* ent, const Entity& e)
{
    if (!ent) {
        return;
    }
    if (e.rgb) {
        ent->color.method = DWG_COLOR_METHOD_TRUECOLOR;
        ent->color.rgb = e.rgb & 0x00ffffff;
        ent->color.index = (BITCODE_BSd)e.colorIndex;
    }
    else if (e.colorIndex > 0 && e.colorIndex != 256) {
        // 实体自带颜色：写成真彩色（ACI 通过调色板换算），避免 ACI 在 R2004+
        // 编码里的歧义
        ent->color.method = DWG_COLOR_METHOD_TRUECOLOR;
        ent->color.rgb = aciRgb(e.colorIndex) & 0x00ffffff;
        ent->color.index = (BITCODE_BSd)e.colorIndex;
    }
    else {
        ent->color.method = DWG_COLOR_METHOD_BYLAYER;
        ent->color.rgb = 0;
        ent->color.index = 256;
    }
    ent->linewt = kLineWeightByLayer;
    ent->invisible = 0;
}

//! 导出功能：图层颜色（R2004+ 用真彩色写，ACI 先经调色板换算）。
void applyLayerColor(Dwg_Object_LAYER* layer, int colorIndex, bool off)
{
    if (!layer) {
        return;
    }
    const int aci = std::max(1, std::abs(colorIndex));
    layer->color.method = DWG_COLOR_METHOD_TRUECOLOR;
    layer->color.rgb = aciRgb(aci) & 0x00ffffff;
    // index 保留给低版本（R2000 及更早按 ACI 存），关闭的图层用负值
    layer->color.index = (BITCODE_BSd)(off ? -aci : aci);
}

//! 导出功能：按名字查找表记录句柄（LAYER / LTYPE / STYLE）。
BITCODE_H tableHandle(Dwg_Data* dwg, const std::string& name, const char* table)
{
    if (name.empty()) {
        return nullptr;
    }
    return dwg_find_tablehandle(dwg, name.c_str(), table);
}

//! 导出功能：估算一段文字的画布宽度（mm）。CJK/全角按 1 个字高，西文按
//! 0.55 个字高；只用于对齐补偿和范围计算，不要求精确。
double estimateTextWidth(const std::string& text, double height,
                         double widthFactor)
{
    const double h = std::max(height, 1.0) * (widthFactor > 0.0 ? widthFactor : 1.0);
    double em = 0.0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char ch = (unsigned char)text[i];
        unsigned long cp = ch;
        if (ch < 0x80) {
            ++i;
        }
        else if ((ch & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp = ((ch & 0x1Fu) << 6) | ((unsigned char)text[i + 1] & 0x3Fu);
            i += 2;
        }
        else if ((ch & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp = ((ch & 0x0Fu) << 12) | (((unsigned char)text[i + 1] & 0x3Fu) << 6)
                 | ((unsigned char)text[i + 2] & 0x3Fu);
            i += 3;
        }
        else if ((ch & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp = ((ch & 0x07u) << 18) | (((unsigned char)text[i + 1] & 0x3Fu) << 12)
                 | (((unsigned char)text[i + 2] & 0x3Fu) << 6)
                 | ((unsigned char)text[i + 3] & 0x3Fu);
            i += 4;
        }
        else {
            ++i;
        }
        // CJK、全角标点、假名、谚文等按 1 em，其余按 0.55 em
        em += (cp >= 0x2E80) ? 1.0 : 0.55;
    }
    return em * h;
}

//! 导出功能：复制一段文本给 LibreDWG（它用 free() 释放这些字段）。
char* dupText(const std::string& text)
{
#ifdef _MSC_VER
    return _strdup(text.c_str());
#else
    return strdup(text.c_str());
#endif
}

//! 导出功能：文字样式（含 CJK 大字体）。同名样式只建一次。
//! 没有这一步，中文文字在 CAD 里会变成 "?"（标准样式 txt 没有大字体）。
//! 注意：非 ASCII 样式名建表后按名字查不到（LibreDWG 按代码页存名），
//! 所以这类样式改用 SWDWG_STYLE_N 这样的 ASCII 名；字体/宽度因子照原样带过去。
BITCODE_H ensureStyle(Dwg_Data* dwg, const Entity::TextAttrs& attrs,
                      std::map<std::string, BITCODE_H>& cache, int& nextIndex)
{
    const std::string name = attrs.styleName.empty() ? std::string("Standard")
                                                     : attrs.styleName;
    auto it = cache.find(name);
    if (it != cache.end()) {
        return it->second;
    }

    // 已有同名样式（ASCII 名）直接复用；但它若是 LibreDWG 建库时留下的空字体
    // 样式（如 Standard），补上标准 CJK 组合，否则中文会画成 "?"。
    if (BITCODE_H existing = tableHandle(dwg, name, "STYLE")) {
        if (existing->obj && existing->obj->tio.object
            && existing->obj->tio.object->tio.STYLE) {
            Dwg_Object_STYLE* style = existing->obj->tio.object->tio.STYLE;
            const bool noFont = !style->font_file || style->font_file[0] == '\0';
            if (noFont) {
                // 导出功能：空字体样式补 TrueType，保证中英文都能显示
                style->font_file = dupText(std::string("simsun.ttc"));
                style->bigfont_file = dupText(std::string());
            }
        }
        cache[name] = existing;
        return existing;
    }

    bool ascii = true;
    for (const unsigned char ch : name) {
        if (ch > 127) {
            ascii = false;
            break;
        }
    }
    const std::string tableName =
        ascii ? name : ("SWDWG_STYLE_" + std::to_string(++nextIndex));
    Dwg_Object_STYLE* style = dwg_add_STYLE(dwg, tableName.c_str());
    if (!style) {
        cache[name] = tableHandle(dwg, "Standard", "STYLE");
        return cache[name];
    }
    // 导出功能：字体选择（实测于 ZWCAD 2026，见 docs/41 的字体表）——
    //  · SHX 主字体 + SHX 大字体（源图常见的 fsdb_e.shx + fsdb.shx）：一旦大字体
    //    在 CAD 里加载失败，**整段文字都不画**，比缺字更糟；
    //  · 只有 SHX 主字体：ASCII 正常，中文变 "?"；
    //  · TrueType（simsun.ttc / simhei.ttf …）：中英文都能画，且不需要大字体。
    // 所以：源字体本身是 TTF 就照用；否则统一换成 simsun.ttc，保证文字一定可见。
    const bool isTtf = attrs.fontFile.size() > 4
        && (attrs.fontFile.rfind(".ttf") != std::string::npos
            || attrs.fontFile.rfind(".ttc") != std::string::npos
            || attrs.fontFile.rfind(".TTF") != std::string::npos
            || attrs.fontFile.rfind(".TTC") != std::string::npos);
    const std::string fontName =
        isTtf ? attrs.fontFile : std::string("simsun.ttc");
    style->font_file = dupText(fontName);
    style->bigfont_file = dupText(std::string());
    style->width_factor = attrs.widthFactor > 0.0 ? attrs.widthFactor : 1.0;
    style->oblique_angle = attrs.oblique * kPi / 180.0;
    style->last_height = 0.0;  // 可变字高
    cache[name] = tableHandle(dwg, tableName, "STYLE");
    return cache[name];
}

//! 导出功能：给实体挂上图层句柄。
void setLayer(Dwg_Object_Entity* ent, BITCODE_H layer)
{
    if (ent && layer) {
        ent->layer = layer;
    }
}

//! 导出功能：路径是否全为 ASCII（LibreDWG 的 fopen 只认 ANSI 代码页）。
bool isAsciiPath(const std::string& path)
{
    for (const unsigned char ch : path) {
        if (ch > 127) {
            return false;
        }
    }
    return true;
}

//! 导出功能：把模型空间范围和 *ACTIVE 视口写成"打开就能看到图形"。
void setExtentsAndViewport(Dwg_Data* dwg, double minx, double miny, double maxx,
                           double maxy)
{
    dwg->header_vars.EXTMIN.x = minx;
    dwg->header_vars.EXTMIN.y = miny;
    dwg->header_vars.EXTMIN.z = 0.0;
    dwg->header_vars.EXTMAX.x = maxx;
    dwg->header_vars.EXTMAX.y = maxy;
    dwg->header_vars.EXTMAX.z = 0.0;
    dwg->header_vars.LIMMIN.x = minx;
    dwg->header_vars.LIMMIN.y = miny;
    dwg->header_vars.LIMMAX.x = maxx;
    dwg->header_vars.LIMMAX.y = maxy;
    // 模型空间（而不是图纸空间布局）打开
    dwg->header_vars.TILEMODE = 1;

    const double width = std::max(maxx - minx, 1e-6);
    const double height = std::max(maxy - miny, 1e-6);
    const double viewHeight = std::max(width, height) * 1.05;
    const double aspect = std::min(std::max(width / height, 0.1), 10.0);
    const double cx = (minx + maxx) / 2.0;
    const double cy = (miny + maxy) / 2.0;

    for (BITCODE_BL i = 0; i < dwg->num_objects; ++i) {
        Dwg_Object* obj = &dwg->object[i];
        if (!obj || obj->fixedtype != DWG_TYPE_VPORT || !obj->tio.object) {
            continue;
        }
        Dwg_Object_VPORT* vp = obj->tio.object->tio.VPORT;
        if (!vp) {
            continue;
        }
        vp->VIEWSIZE = viewHeight;
        vp->view_width = viewHeight * aspect;
        vp->aspect_ratio = aspect;
        // DWG 里 VIEWCTR 是相对 view_target 的"2D 增量"（FIELD_2DD），
        // 两个都写成中心点会让视图中心变成 2 倍（实测 ZWCAD 读出来
        // VIEWCTR = 2×中心，图纸跑到左上角只露出一小块）。
        // 源图的做法是 view_target = (0,0,0)、VIEWCTR = 中心，这里照抄。
        vp->VIEWCTR.x = cx;
        vp->VIEWCTR.y = cy;
        vp->view_target.x = 0.0;
        vp->view_target.y = 0.0;
        vp->view_target.z = 0.0;
        vp->VIEWDIR.x = 0.0;
        vp->VIEWDIR.y = 0.0;
        vp->VIEWDIR.z = 1.0;
        break;  // the first VPORT is *ACTIVE
    }
}

}  // namespace

int DWGWriter::rgbToAci(unsigned long rgb)
{
    const int r = (int)((rgb >> 16) & 0xff);
    const int g = (int)((rgb >> 8) & 0xff);
    const int b = (int)(rgb & 0xff);
    int best = 7;
    long bestDist = -1;
    for (int i = 1; i <= 255; ++i) {
        const unsigned long c = aciRgb(i);
        const int dr = r - (int)((c >> 16) & 0xff);
        const int dg = g - (int)((c >> 8) & 0xff);
        const int db = b - (int)(c & 0xff);
        const long dist = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (bestDist < 0 || dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

bool DWGWriter::writeDWG(const std::string& filename,
                         const std::vector<Entity>& entities,
                         const std::vector<DWGOutLayer>& layers,
                         const std::vector<DWGOutLinetype>& linetypes,
                         std::string& error, DWGWriteStats& stats)
{
    stats = DWGWriteStats();
    error.clear();

    if (filename.empty()) {
        error = "导出失败：文件名为空";
        return false;
    }

    // 导出功能：填充面（SOLID）先写、线后写，保证线压在填充上面。
    // （CAD 按实体顺序绘制，表格单元格的实心填充会把里面的线盖住。）
    std::vector<Entity> orderedStorage;
    const std::vector<Entity>& list = [&]() -> const std::vector<Entity>& {
        if (std::none_of(entities.begin(), entities.end(), [](const Entity& e) {
                return e.type == EntType::SOLID;
            })) {
            return entities;
        }
        orderedStorage.reserve(entities.size());
        for (const Entity& e : entities) {
            if (e.type == EntType::SOLID) {
                orderedStorage.push_back(e);
            }
        }
        for (const Entity& e : entities) {
            if (e.type != EntType::SOLID) {
                orderedStorage.push_back(e);
            }
        }
        return orderedStorage;
    }();

    Dwg_Data dwg;
    memset(&dwg, 0, sizeof(dwg));
    // 导出功能：只写 AutoCAD 2018（AC1032）。版本必须在建文档之前设置，
    // 否则 dwg_add_Document 会按默认版本（R2000）建表。
    dwg.header.version = R_2018;

    int err = dwg_add_Document(&dwg, 0 /* metric */);
    if (err >= DWG_ERR_CRITICAL) {
        error = "创建失败：LibreDWG 无法创建文档 (code " + std::to_string(err)
            + ")";
        dwg_free(&dwg);
        return false;
    }

    Dwg_Object* mspace = dwg_model_space_object(&dwg);
    Dwg_Object_BLOCK_HEADER* hdr =
        (mspace && mspace->tio.object) ? mspace->tio.object->tio.BLOCK_HEADER : nullptr;
    if (!hdr) {
        error = "导出失败：LibreDWG 没有模型空间";
        dwg_free(&dwg);
        return false;
    }

    // 导出功能：单位 mm + 公制，与 FreeCAD 的内部单位一致。
    dwg.header_vars.INSUNITS = 4;
    dwg.header_vars.MEASUREMENT = 1;
    dwg.header_vars.LTSCALE = 1.0;

    // --- 线型 -----------------------------------------------------------
    // 导出功能：先把线型表建好（虚线/点划线等），否则实体只能落到 CONTINUOUS，
    // 源图的虚线在 CAD 里就变成实线。图案来自 DWGLineType 对象的 Pattern。
    std::map<std::string, BITCODE_H> ltypeHandles;
    if (BITCODE_H cont = tableHandle(&dwg, "CONTINUOUS", "LTYPE")) {
        ltypeHandles["CONTINUOUS"] = cont;
    }
    for (const DWGOutLinetype& lt : linetypes) {
        if (lt.name.empty() || ltypeHandles.count(lt.name)) {
            continue;
        }
        if (BITCODE_H existing = tableHandle(&dwg, lt.name, "LTYPE")) {
            ltypeHandles[lt.name] = existing;
            continue;
        }
        Dwg_Object_LTYPE* rec = dwg_add_LTYPE(&dwg, lt.name.c_str());
        if (!rec) {
            continue;
        }
        rec->description = dupText(lt.description.empty()
                                       ? (lt.name + " linetype")
                                       : lt.description);
        rec->pattern_len = lt.patternLength;
        if (!lt.dashes.empty()) {
            rec->numdashes = (BITCODE_RC)lt.dashes.size();
            rec->dashes = (Dwg_LTYPE_dash*)std::calloc(lt.dashes.size(),
                                                      sizeof(Dwg_LTYPE_dash));
            if (rec->dashes) {
                for (size_t i = 0; i < lt.dashes.size(); ++i) {
                    rec->dashes[i].length = lt.dashes[i];
                    rec->dashes[i].scale = 1.0;
                    rec->dashes[i].shape_flag = 0;
                }
            }
        }
        ltypeHandles[lt.name] = tableHandle(&dwg, lt.name, "LTYPE");
        SWDWG_LOG("[SWDWG EXPORT] LTYPE '%s' dashes=%zu len=%.2f\n",
                  lt.name.c_str(), lt.dashes.size(), lt.patternLength);
    }

    // --- 图层 -----------------------------------------------------------
    std::map<std::string, BITCODE_H> layerHandles;
    BITCODE_H layer0 = tableHandle(&dwg, "0", "LAYER");
    if (layer0) {
        layerHandles["0"] = layer0;
        // 导出功能：默认图层 "0" 由 LibreDWG 建库时创建，颜色是 BYBLOCK，
        // 这里补成白色（按句柄匹配，避免 R2018 的字符串编码差异）
        for (BITCODE_BL i = 0; i < dwg.num_objects; ++i) {
            Dwg_Object* obj = &dwg.object[i];
            if (obj && obj->fixedtype == DWG_TYPE_LAYER && obj->tio.object
                && obj->tio.object->tio.LAYER
                && obj->handle.value == layer0->absolute_ref) {
                applyLayerColor(obj->tio.object->tio.LAYER, 7, false);
                break;
            }
        }
    }
    int layerIndex = 0;
    for (const DWGOutLayer& layer : layers) {
        if (layer.name.empty() || layerHandles.count(layer.name)) {
            continue;
        }
        // 导出功能：非 ASCII 图层名建表后按名字查不回来（LibreDWG 的表查找
        // 只对 ASCII 生效）。先用 ASCII 临时名建表拿到句柄，再把名字改回原名。
        bool asciiName = true;
        for (const unsigned char ch : layer.name) {
            if (ch > 127) {
                asciiName = false;
                break;
            }
        }
        const std::string createName =
            asciiName ? layer.name
                      : ("SWDWG_LAYER_" + std::to_string(++layerIndex));
        Dwg_Object_LAYER* rec = dwg_add_LAYER(&dwg, createName.c_str());
        if (!rec) {
            ++stats.skipped;
            continue;
        }
        // 句柄必须在改名之前取：改名之后按临时名就查不到了。
        BITCODE_H createdHandle = tableHandle(&dwg, createName, "LAYER");
        if (!asciiName) {
            rec->name = dupText(layer.name);  // 改回原名
        }
        rec->color.index = (BITCODE_BSd)(layer.off ? -std::abs(layer.colorIndex)
                                                  : std::abs(layer.colorIndex));
        applyLayerColor(rec, layer.colorIndex, layer.off);
        if (layer.frozen) {
            rec->flag |= 1;
        }
        if (layer.lineWeight > 0) {
            rec->linewt = lineWeightIndex(layer.lineWeight);
        }
        auto ltIt = ltypeHandles.find(layer.ltype);
        BITCODE_H lt = (ltIt != ltypeHandles.end())
                           ? ltIt->second
                           : tableHandle(&dwg, layer.ltype, "LTYPE");
        if (lt) {
            rec->ltype = lt;
        }
        layerHandles[layer.name] =
            createdHandle ? createdHandle
                          : tableHandle(&dwg, layer.name, "LAYER");
        ++stats.layers;
    }

    // 导出功能：文字样式表缓存（同名样式只建一次）
    std::map<std::string, BITCODE_H> styleHandles;
    int nextStyleIndex = 0;

    // --- 实体 -----------------------------------------------------------
    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
    auto grow = [&minx, &miny, &maxx, &maxy](const Vec3& p) {
        minx = std::min(minx, p.x);
        miny = std::min(miny, p.y);
        maxx = std::max(maxx, p.x);
        maxy = std::max(maxy, p.y);
    };
    auto layerOf = [&layerHandles, &layer0](const std::string& name) -> BITCODE_H {
        auto it = layerHandles.find(name);
        if (it != layerHandles.end()) {
            return it->second;
        }
        return layer0;
    };

    // --- 大图纸分块 -------------------------------------------------------
    // 导出功能：LibreDWG 的 R2018 写入在一个块里超过 ~1.6 万个实体后就会写出
    // 结构损坏的文件（实测 16000 条线即报 "expected ENDBLK"，8000 条正常，
    // 12000 条实测正常）。超过阈值时把实体按顺序切成若干 BLOCK（每块 <=
    // kMaxEntitiesPerBlock），再在模型空间放一个原点上的 INSERT。
    // 注意：块里的实体在 CAD 里是"块参照"，逐条线悬停高亮/选中会失效，
    // 所以阈值尽量放大（配合多段线合并，常规图纸已经用不到块）。
    const size_t kMaxEntitiesPerBlock = 9000;
    struct BlockChunk
    {
        std::string name;
        Dwg_Object_BLOCK_HEADER* header = nullptr;
    };
    std::vector<BlockChunk> blocks;
    if (list.size() > kMaxEntitiesPerBlock) {
        const size_t count =
            (list.size() + kMaxEntitiesPerBlock - 1) / kMaxEntitiesPerBlock;
        for (size_t i = 0; i < count; ++i) {
            BlockChunk chunk;
            chunk.name = "SWDWG_EXPORT_" + std::to_string(i + 1);
            chunk.header = dwg_add_BLOCK_HEADER(&dwg, chunk.name.c_str());
            if (!chunk.header) {
                error = "导出失败：LibreDWG 无法创建块 " + chunk.name;
                dwg_free(&dwg);
                return false;
            }
            // BLOCK 实体必须由块自己的 BLOCK_HEADER 拥有，否则写入时
            // 报 "BLOCK_HEADER ... first_owned_entity missing"
            dwg_add_BLOCK(chunk.header, chunk.name.c_str());
            blocks.push_back(chunk);
        }
        SWDWG_LOG("[SWDWG EXPORT] %zu entities split into %zu blocks\n",
                  list.size(), blocks.size());
    }

    size_t entityIndex = 0;
    for (const Entity& e : list) {
        Dwg_Object_Entity* ent = nullptr;
        Dwg_Object_BLOCK_HEADER* target =
            blocks.empty()
                ? hdr
                : blocks[std::min(entityIndex / kMaxEntitiesPerBlock, blocks.size() - 1)]
                      .header;
        ++entityIndex;
        const dwg_point_3d p0 = toPoint(e.pts.empty() ? Vec3() : e.pts[0]);

        switch (e.type) {
            case EntType::LINE: {
                if (e.pts.size() < 2) {
                    ++stats.skipped;
                    continue;
                }
                const dwg_point_3d p1 = toPoint(e.pts[1]);
                Dwg_Entity_LINE* line = dwg_add_LINE(target, &p0, &p1);
                if (line) {
                    ent = line->parent;
                    grow(e.pts[0]);
                    grow(e.pts[1]);
                }
                break;
            }
            case EntType::CIRCLE: {
                const dwg_point_3d c = toPoint(e.center);
                Dwg_Entity_CIRCLE* circle = dwg_add_CIRCLE(target, &c, e.radius);
                if (circle) {
                    ent = circle->parent;
                    grow(Vec3{e.center.x - e.radius, e.center.y - e.radius, 0});
                    grow(Vec3{e.center.x + e.radius, e.center.y + e.radius, 0});
                }
                break;
            }
            case EntType::ARC: {
                const dwg_point_3d c = toPoint(e.center);
                Dwg_Entity_ARC* arc =
                    dwg_add_ARC(target, &c, e.radius, e.startAngle, e.endAngle);
                if (arc) {
                    ent = arc->parent;
                    grow(Vec3{e.center.x - e.radius, e.center.y - e.radius, 0});
                    grow(Vec3{e.center.x + e.radius, e.center.y + e.radius, 0});
                }
                break;
            }
            case EntType::POINT: {
                Dwg_Entity_POINT* point = dwg_add_POINT(target, &p0);
                if (point) {
                    ent = point->parent;
                    grow(e.pts[0]);
                }
                break;
            }
            case EntType::FACE3D: {
                if (e.pts.size() < 3) {
                    ++stats.skipped;
                    continue;
                }
                const dwg_point_3d p1 = toPoint(e.pts[1]);
                const dwg_point_3d p2 = toPoint(e.pts[2]);
                const dwg_point_3d p3 = toPoint(e.pts.size() > 3 ? e.pts[3] : e.pts[2]);
                Dwg_Entity__3DFACE* face = dwg_add_3DFACE(target, &p0, &p1, &p2, &p3);
                if (face) {
                    ent = face->parent;
                    for (const Vec3& p : e.pts) {
                        grow(p);
                    }
                }
                break;
            }
            case EntType::SOLID: {
                // 导出功能：填充面（表格单元格、实心箭头等）。DWG 的 SOLID
                // 第 3、4 点顺序是交叉的（P1,P2,P4,P3），否则会画成蝴蝶结。
                if (e.pts.size() < 3) {
                    ++stats.skipped;
                    continue;
                }
                const dwg_point_3d a = toPoint(e.pts[0]);
                const dwg_point_2d b{e.pts[1].x, e.pts[1].y};
                const Vec3& corner3 = e.pts.size() > 3 ? e.pts[3] : e.pts[2];
                const Vec3& corner4 = e.pts[2];
                const dwg_point_2d c{corner3.x, corner3.y};
                const dwg_point_2d d{corner4.x, corner4.y};
                Dwg_Entity_SOLID* solid = dwg_add_SOLID(target, &a, &b, &c, &d);
                if (solid) {
                    ent = solid->parent;
                    for (const Vec3& p : e.pts) {
                        grow(p);
                    }
                }
                break;
            }
            case EntType::TEXT: {
                // 导出功能：TrueType 字号补偿（见 kTrueTypeHeightScale）
                const double drawHeight =
                    std::max(e.textHeight, 0.1) * kTrueTypeHeightScale;
                const dwg_point_3d ins = toPoint(e.insPt);
                Dwg_Entity_TEXT* text =
                    dwg_add_TEXT(target, e.text.c_str(), &ins, drawHeight);
                if (text) {
                    ent = text->parent;
                    text->rotation = e.rotation;
                    text->width_factor = e.textAttrs.widthFactor > 0.0
                                             ? e.textAttrs.widthFactor
                                             : 1.0;
                    text->oblique_angle = e.textAttrs.oblique * kPi / 180.0;
                    text->elevation = e.insPt.z;
                    // 导出功能：LibreDWG 写 R2018 时会丢掉 TEXT 的 alignment_pt
                    // （对齐点），CAD 于是把"居中/中间"对齐的文字全摆到 (0,0)
                    // ——表格里成百上千个居中文字就会"消失"到原点。这里统一按
                    // 左/基线写入，再把插入点按对齐方式平移同样的量，
                    // 显示位置等价，而且不依赖 alignment_pt。
                    const double boxWidth = estimateTextWidth(
                        e.text, drawHeight, text->width_factor);
                    const double boxHeight = std::max(drawHeight, 1.0);
                    double dx = 0.0;
                    switch (e.textHAlign) {
                        case 1:  // center
                        case 3:  // aligned
                        case 4:  // middle
                        case 5:  // fit
                            dx = -boxWidth / 2.0;
                            break;
                        case 2:  // right
                            dx = -boxWidth;
                            break;
                        default:
                            break;
                    }
                    double dy = 0.0;
                    switch (e.textVAlign) {
                        case 1:  // bottom
                            dy = 0.2 * boxHeight;
                            break;
                        case 2:  // middle
                            dy = 0.5 * boxHeight;
                            break;
                        case 3:  // top
                            dy = boxHeight;
                            break;
                        default:  // baseline
                            break;
                    }
                    text->horiz_alignment = 0;
                    text->vert_alignment = 0;
                    // 导出功能：偏移量要跟着文字旋转一起转 —— 平移量是在"文字坐标
                    // 系"里算的（沿字行方向 / 垂直字行），旋转 90° 的标注如果直接
                    // 加到世界坐标上，方向就错了（实测标注会整体偏一格）。
                    const double cosR = std::cos(e.rotation);
                    const double sinR = std::sin(e.rotation);
                    text->ins_pt.x = e.insPt.x + dx * cosR - dy * sinR;
                    text->ins_pt.y = e.insPt.y + dx * sinR + dy * cosR;
                    text->alignment_pt.x = 0.0;
                    text->alignment_pt.y = 0.0;
                    // 导出功能：样式缺失时按实体携带的字体建 STYLE（CJK 大字体）
                    text->style = ensureStyle(&dwg, e.textAttrs, styleHandles, nextStyleIndex);
                    // 导出功能：按字高和字符数估算文字包围盒，保证范围/视口
                    // 把文字也算进去（CJK 约 1 个字高，西文约 0.7）
                    grow(Vec3{text->ins_pt.x, text->ins_pt.y, e.insPt.z});
                    grow(Vec3{text->ins_pt.x + boxWidth, text->ins_pt.y + boxHeight,
                              e.insPt.z});
                }
                break;
            }
            case EntType::MTEXT: {
                const dwg_point_3d ins = toPoint(e.insPt);
                Dwg_Entity_MTEXT* text = dwg_add_MTEXT(
                    target, &ins, e.textAttrs.mtextRectW, e.text.c_str());
                if (text) {
                    ent = text->parent;
                    text->attachment = (BITCODE_BS)e.mtextAttachment;
                    // 导出功能：字高必须显式写（dwg_add_MTEXT 不会从矩形高度推），
                    // 并做 TrueType 字号补偿（见 kTrueTypeHeightScale）。
                    text->text_height =
                        std::max(e.textHeight, 0.1) * kTrueTypeHeightScale;
                    // 导出功能：MTEXT 的旋转不在 rotation 字段，而是 x_axis_dir
                    // 这个单位向量（DXF 11），按 Z 轴旋转推出方向。
                    text->x_axis_dir.x = std::cos(e.rotation);
                    text->x_axis_dir.y = std::sin(e.rotation);
                    text->x_axis_dir.z = 0.0;
                    text->linespace_factor = e.textAttrs.lsFactor > 0.0
                                                 ? e.textAttrs.lsFactor
                                                 : 1.0;
                    // 导出功能：样式缺失时按实体携带的字体建 STYLE（CJK 大字体）
                    text->style = ensureStyle(&dwg, e.textAttrs, styleHandles, nextStyleIndex);
                    grow(e.insPt);
                    grow(Vec3{e.insPt.x + std::max(e.textAttrs.mtextRectW, 1.0),
                              e.insPt.y + std::max(e.mtextHeight, 1.0), 0});
                }
                break;
            }
            case EntType::LWPOLYLINE: {
                // 导出功能：写成真正的 LWPOLYLINE（一个实体 N 个顶点），不再拆成
                // 一堆 LINE —— 合并同类线段后实体数能降一个量级，文件更小，也不会
                // 再触发"单块超过 1.6 万实体"的按块拆分。
                if (e.pts.size() < 2) {
                    ++stats.skipped;
                    continue;
                }
                std::vector<dwg_point_2d> pts;
                pts.reserve(e.pts.size());
                for (const Vec3& p : e.pts) {
                    dwg_point_2d q{p.x, p.y};
                    pts.push_back(q);
                }
                Dwg_Entity_LWPOLYLINE* pl =
                    dwg_add_LWPOLYLINE(target, (int)pts.size(), pts.data());
                if (pl) {
                    ent = pl->parent;
                    // 导出功能：DWG 二进制里 LWPOLYLINE 的"闭合"是 flag 的 512 位
                    // （DXF 的 1 位是"有挤出方向"，写错会让矩形少一条闭合边）。
                    BITCODE_BS lwFlag = (BITCODE_BS)(pl->flag & ~0x200);
                    if (e.closed) {
                        lwFlag = (BITCODE_BS)(lwFlag | 0x200);
                    }
                    pl->flag = lwFlag;
                    pl->elevation = e.pts.front().z;
                    for (const Vec3& p : e.pts) {
                        grow(p);
                    }
                    // 每个顶点一个 bulge（0 = 直线段）；LibreDWG 没分配数组时跳过
                    if (pl->bulges && pl->num_points > 0) {
                        for (BITCODE_BL i = 0;
                             i < pl->num_points && i < (BITCODE_BL)e.pts.size(); ++i) {
                            pl->bulges[i] = e.bulges.size() > i ? e.bulges[i] : 0.0;
                        }
                    }
                }
                break;
            }
            default:
                ++stats.skipped;
                continue;
        }

        if (!ent) {
            ++stats.skipped;
            continue;
        }
        setLayer(ent, layerOf(e.layer));
        applyCommon(ent, e);
        // 导出功能：按实体挂线型（虚线/点划线），并写它自己的线型比例。
        // 线型名大小写不固定（FreeCAD 里是 "Continuous"，DWG 里是
        // "CONTINUOUS"），这里统一按不区分大小写处理，并直接按名字查表。
        if (!e.ltype.empty()) {
            std::string lt = e.ltype;
            for (char& ch : lt) {
                ch = (char)std::toupper((unsigned char)ch);
            }
            if (lt != "CONTINUOUS" && lt != "BYLAYER" && lt != "BYBLOCK") {
                if (BITCODE_H handle = tableHandle(&dwg, e.ltype, "LTYPE")) {
                    ent->ltype = handle;
                    // ltype_flags: 0=ByLayer 1=ByBlock 2=Continuous 3=显式句柄
                    ent->ltype_flags = 3;
                    ent->ltype_scale = e.ltypeScale > 0.0 ? e.ltypeScale : 1.0;
                }
            }
        }
        ++stats.entities;
    }

    // 导出功能：收尾分块 —— 结束每个块，并在模型空间放一个原点 INSERT。
    // INSERT 不缩放不旋转，实体坐标即世界坐标，图纸位置不变。
    if (!blocks.empty()) {
        for (BlockChunk& chunk : blocks) {
            dwg_add_ENDBLK(chunk.header);
        }
        for (const BlockChunk& chunk : blocks) {
            dwg_point_3d origin = {0.0, 0.0, 0.0};
            dwg_add_INSERT(hdr, &origin, chunk.name.c_str(), 1.0, 1.0, 1.0, 0.0);
        }
    }

    if (stats.entities > 0 && minx <= maxx) {
        setExtentsAndViewport(&dwg, minx, miny, maxx, maxy);
    }

    // 导出功能：非 ASCII 输出路径。
    // LibreDWG 用 ANSI fopen，中文路径会按 ANSI 解释 UTF-8 字节，写出的文件名
    // 会乱码。这里切到目标目录、用纯 ASCII 临时文件名写盘（LibreDWG 只看到
    // ASCII），写完再切回并把临时文件改名成真正的目标名。
    namespace fs = std::filesystem;
    std::string writePath = filename;
    fs::path finalPath;
    fs::path tempPath;
    fs::path previousDir;
    bool moveAfterWrite = false;
    if (!isAsciiPath(filename)) {
        std::error_code ec;
        finalPath = fs::u8path(filename);
        tempPath = finalPath.parent_path().empty()
                       ? fs::u8path("swdwg_export_tmp.dwg")
                       : finalPath.parent_path() / fs::u8path("swdwg_export_tmp.dwg");
        previousDir = fs::current_path(ec);
        fs::current_path(finalPath.parent_path().empty() ? fs::path(".")
                                                         : finalPath.parent_path(), ec);
        if (ec) {
            error = "导出失败：无法进入目标目录 " + filename;
            dwg_free(&dwg);
            return false;
        }
        writePath = "swdwg_export_tmp.dwg";
        moveAfterWrite = true;
    }

    // 导出功能：LibreDWG 的 dwg_write_file 不会覆盖已存在的文件（返回 4096），
    // 所以先把旧文件删掉；删除失败（文件被 CAD 占用）时写盘会报错，由上层提示。
    std::remove(writePath.c_str());

    err = dwg_write_file(writePath.c_str(), &dwg);
    if (moveAfterWrite) {
        std::error_code ec;
        fs::current_path(previousDir, ec);
        if (err < DWG_ERR_CRITICAL) {
            fs::rename(tempPath, finalPath, ec);
            if (ec) {
                // 目标已存在时 rename 在 Windows 上会失败：先删再改名
                fs::remove(finalPath, ec);
                fs::rename(tempPath, finalPath, ec);
            }
            if (ec) {
                error = "导出失败：无法把临时文件改成目标文件名 " + filename;
                dwg_free(&dwg);
                return false;
            }
        }
    }
    if (err >= DWG_ERR_CRITICAL) {
        error = "导出失败：LibreDWG 写文件失败 (code " + std::to_string(err) +
                ")： " + filename + "（若文件正被 CAD 打开，请先关闭）";
        dwg_free(&dwg);
        return false;
    }

    dwg_free(&dwg);
    SWDWG_LOG("[SWDWG EXPORT] %s: %d entities, %d layers, %d skipped\n",
              filename.c_str(), stats.entities, stats.layers, stats.skipped);
    return true;
}

}  // namespace SWDWG
