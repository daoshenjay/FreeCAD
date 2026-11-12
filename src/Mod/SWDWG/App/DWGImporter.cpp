//! Document import orchestration and the public DWGImporter entry point.
//!
//! The pipeline is split across several files so each stage can be analysed
//! and debugged independently:
//!   DWGExtract.cpp    LibreDWG object -> Entity   (per-type parsing)
//!   DWGHatch.cpp      HATCH pattern lines  -> 2D segments
//!   DWGShape.cpp      Entity               -> OCC shape (per-type building)
//!   DWGDimstyle.cpp   DIMSTYLE table       -> DimStyle struct
//!   DWGDimension.cpp  DIMENSION entity     -> DimensionData + geometry
//!   DWGImporter.cpp   document import + block expansion + FreeCAD objects
//!
//! This file owns the Importer runner: parsing the DWG structure, grouping
//! entities by style, expanding INSERTs and creating FreeCAD objects.

#include "DWGInternal.h"

#include "DWGImporter.h"
#include "PathUtil.h"
#include "DWGLineGroup.h"
#include "DWGLineType.h"
#include "DWGText.h"
#include "DWGSurfaceMesh.h"
#include "FTTextRenderer.h"
#include "TextShxRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/GroupExtension.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Base/Color.h>
#include <Base/Console.h>
#include <Base/Tools.h>  // Base::ObjectStatusLocker
#include <Mod/Part/App/PartFeature.h>

#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_Trsf.hxx>
#include <gp_XYZ.hxx>

namespace SWDWG
{



// ===== 临时调试：阶段日志写文件（崩溃时缓冲区会丢，所以直接落盘）=====


namespace
{

//! Tiny RAII accumulator that feeds the per-import diagnostics printed at the
//! end of importDWG (which stage of expand/create eats the time).
class ImpStopwatch
{
public:
    explicit ImpStopwatch(double& out)
        : out_(out), start_(std::chrono::steady_clock::now())
    {
    }
    ~ImpStopwatch()
    {
        out_ += std::chrono::duration<double>(std::chrono::steady_clock::now()
                                              - start_).count();
    }

private:
    double& out_;
    std::chrono::steady_clock::time_point start_;
};

double g_textCreateSec = 0.0;  // createAnnotation() total (incl. block texts)
double g_geomBuildSec = 0.0;   // top-level LINE / POLYLINE / HATCH builds
double g_insertSec = 0.0;      // top-level INSERT expansion
double g_groupSec = 0.0;       // run() tail: line-group objects + L2 cut
double g_runRecomputeSec = 0.0;  // ... of which the internal doc->recompute()

//! Vertex key for 3DFACE shared-vertex deduplication (bit-exact doubles, so
//! adjacent grid faces that came from the same source value collapse).
struct FacePtKey
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool operator==(const FacePtKey& o) const
    {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct FacePtKeyHash
{
    std::size_t operator()(const FacePtKey& k) const noexcept
    {
        auto mix = [](double d) {
            std::uint64_t u;
            std::memcpy(&u, &d, sizeof(u));
            u ^= u >> 33;
            u *= 0xff51afd7ed558ccdULL;
            u ^= u >> 33;
            return u;
        };
        std::uint64_t h = mix(k.x);
        h ^= mix(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= mix(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};
}  // namespace

//! ASCII case-insensitive comparison.
bool eqIgnoreCaseAscii(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

//! Human-readable name for common LibreDWG fixed object types (diagnostics).
const char* fixedTypeName(int t)
{
    switch (t) {
        case DWG_TYPE_ATTRIB:
            return "ATTRIB";
        case DWG_TYPE_ATTDEF:
            return "ATTDEF";
        case DWG_TYPE_INSERT:
            return "INSERT";
        case DWG_TYPE_POLYLINE_2D:
            return "POLYLINE_2D";
        case DWG_TYPE_LINE:
            return "LINE";
        case DWG_TYPE_POINT:
            return "POINT";
        case DWG_TYPE_SOLID:
            return "SOLID";
        case DWG_TYPE_BLOCK_HEADER:
            return "BLOCK_HEADER";
        // The rest of the fixed types that actually turn up in civil drawings:
        // without these the report printed "?" for entities that are simply
        // not geometry the importer handles (a layout VIEWPORT is not missing
        // line work), which reads like data loss.
        case DWG_TYPE_TEXT:
            return "TEXT";
        case DWG_TYPE_TRACE:
            return "TRACE";
        case DWG_TYPE_SHAPE:
            return "SHAPE";
        case DWG_TYPE_VIEWPORT:
            return "VIEWPORT";
        case DWG_TYPE_ELLIPSE:
            return "ELLIPSE";
        case DWG_TYPE_SPLINE:
            return "SPLINE";
        case DWG_TYPE_REGION:
            return "REGION";
        case DWG_TYPE__3DSOLID:
            return "3DSOLID";
        case DWG_TYPE_BODY:
            return "BODY";
        case DWG_TYPE_RAY:
            return "RAY";
        case DWG_TYPE_OLEFRAME:
            return "OLEFRAME";
        case DWG_TYPE_MTEXT:
            return "MTEXT";
        case DWG_TYPE_LEADER:
            return "LEADER";
        case DWG_TYPE_MLINE:
            return "MLINE";
        case DWG_TYPE_MLINESTYLE:
            return "MLINESTYLE";
        case DWG_TYPE_LWPOLYLINE:
            return "LWPOLYLINE";
        case DWG_TYPE_HATCH:
            return "HATCH";
        case DWG_TYPE_LAYOUT:
            return "LAYOUT";
        case DWG_TYPE_PROXY_ENTITY:
            return "PROXY_ENTITY";
        case DWG_TYPE_PROXY_OBJECT:
            return "PROXY_OBJECT";
        default:
            return "?";
    }
}

//! Compact bounding-box string of a shape for diagnostics.
std::string shapeBBox(const TopoDS_Shape& s)
{
    if (s.IsNull()) {
        return "null";
    }
    Bnd_Box box;
    BRepBndLib::Add(s, box);
    if (box.IsVoid()) {
        return "void";
    }
    double x1 = 0.0, y1 = 0.0, z1 = 0.0, x2 = 0.0, y2 = 0.0, z2 = 0.0;
    box.Get(x1, y1, z1, x2, y2, z2);
    std::ostringstream os;
    os << "[" << x1 << ", " << y1 << ", " << z1 << "] - [" << x2 << ", "
       << y2 << ", " << z2 << "]";
    return os.str();
}

//! Shapes that land near the origin (instead of the drawing area) indicate a
//! lost INSERT transform.  Log the first few with their provenance.
int s_originShapes = 0;
void checkOriginShape(const char* what, const std::string& layer,
                      const TopoDS_Shape& s, const Vec3* insPt,
                      const Vec3* scale, double rotation)
{
    if (s.IsNull()) {
        return;
    }
    Bnd_Box box;
    BRepBndLib::Add(s, box);
    if (box.IsVoid()) {
        return;
    }
    double x1 = 0.0, y1 = 0.0, z1 = 0.0, x2 = 0.0, y2 = 0.0, z2 = 0.0;
    box.Get(x1, y1, z1, x2, y2, z2);
    if (x2 < 1000.0 && y2 < 1000.0 && x2 > -100000.0 && y2 > -100000.0) {
        // Diagnostics (commented out): per-shape ORIGIN-AREA detail.
        // if (s_originShapes < 20) {
        //     if (insPt) {
        //         SWDWG_LOG(
        //             "SWDWG:   ORIGIN-AREA %s layer='%s' ins=(%.3f, %.3f, "
        //             "%.3f) scale=(%.3f, %.3f, %.3f) rot=%.3f deg bbox %s\n",
        //             what, layer.c_str(), insPt->x, insPt->y, insPt->z,
        //             scale ? scale->x : 0.0, scale ? scale->y : 0.0,
        //             scale ? scale->z : 0.0, rotation * 180.0 / PI,
        //             shapeBBox(s).c_str());
        //     }
        //     else {
        //         SWDWG_LOG(
        //             "SWDWG:   ORIGIN-AREA %s layer='%s' bbox %s\n", what,
        //             layer.c_str(), shapeBBox(s).c_str());
        //     }
        // }
        ++s_originShapes;
    }
}

//! Report an entity whose built geometry is far larger than a drawing sheet
//! could be.  A mis-built boundary - a hatch path landing at the origin while
//! its siblings sit at the block's absolute coordinates, an arc read with the
//! wrong sweep - otherwise shows up only as an anonymous cluster of stray
//! lines thousands of units away from the sheet (see docs/32), which is
//! exactly how it was reported.
void reportOversized(const char* where, const std::string& owner,
                     EntType type, const std::string& layer,
                     const TopoDS_Shape& s, bool warn = true,
                     double refSpan = 0.0)
{
    // NOTES (heuristics, both of them):
    //  * the threshold is relative to the drawing extent when one is known,
    //    so it means the same thing in a 5-unit detail and a 5 km survey;
    //  * the block-child call passes warn=false, because a child is built in
    //    block-local units and a detail block legitimately measures tens of
    //    thousands of them (block 'A$C7d896937' of the 辅道 plan reports
    //    children of 70000 units while the inserted result stays inside the
    //    5831 x 2509 sheet - verified in the document).  What such a block
    //    does in world space is checked at the INSERT itself, in real units.
    if (s.IsNull()) {
        return;
    }
    Bnd_Box box;
    BRepBndLib::Add(s, box);
    if (box.IsVoid()) {
        return;
    }
    double x1 = 0.0, y1 = 0.0, z1 = 0.0, x2 = 0.0, y2 = 0.0, z2 = 0.0;
    box.Get(x1, y1, z1, x2, y2, z2);
    const double span = std::max(std::max(x2 - x1, y2 - y1), z2 - z1);
    // Relative, not absolute: "20 m" is nothing in a 4.8 km road survey (the
    // dxt drawing flagged 21 ordinary kerb polylines of 28 m each), while the
    // mis-built boundaries that motivated this check measured 430000 x
    // 2884000 in a drawing only 5831 units across - hundreds of times the
    // whole drawing.  Flag only a shape that is a large fraction of the
    // drawing's own extent; with no reference available, fall back to the old
    // absolute floor (20 m in document units).
    const double limit = std::max(2.0 * refSpan, 20000.0);
    if (span <= limit) {
        return;
    }
    static std::set<std::string> s_seen;
    std::ostringstream key;
    key << owner << '|' << (int)type << '|' << (long)x1 << ',' << (long)y1
        << ',' << (long)x2 << ',' << (long)y2;
    if (s_seen.size() < 20 && s_seen.insert(key.str()).second) {
        if (warn) {
            Base::Console().warning(
                "[SWDWG] oversized %s in block '%s': type=%s layer='%s' bbox "
                "%s\n",
                where, owner.c_str(), typeLabel(type), layer.c_str(),
                shapeBBox(s).c_str());
        }
        else {
            Base::Console().message(
                "[SWDWG] oversized %s in block '%s': type=%s layer='%s' bbox "
                "%s (block-local units; the INSERT is checked in world "
                "units)\n",
                where, owner.c_str(), typeLabel(type), layer.c_str(),
                shapeBBox(s).c_str());
        }
    }
}

// ---------------------------------------------------------------------------
//  Import runner: parse -> expand blocks -> create objects
// ---------------------------------------------------------------------------

constexpr int MAX_BLOCK_DEPTH = 64;

namespace
{

//! Wrap expanded MTEXT lines into a layout width (defined with the other text
//! helpers at the bottom of this file).
static std::vector<std::string> wrapMtextLines(
    const std::vector<std::string>& in, double width, double fontHeight,
    const std::string& fontFile, const std::string& bigFontFile, int codepage,
    double widthFactor);

class Importer
{
public:
    explicit Importer(Dwg_Data* d)
        : dwg(d)
    {}

    // Diagnostics: per-type counters (key = (int)EntType).
    std::map<int, int> parsedCount;
    std::map<int, int> createdCount;
    std::map<int, int> failedCount;
    // Raw fixedtypes that failed extractEntity (key = obj->fixedtype).
    std::map<int, int> unsupportedCount;
    // Windows codepage declared by the DWG header (e.g. 936 for GBK).
    int dwgCodepage = 0;

    //! Style attributes of a DWG LAYER table entry.
    struct LayerStyle
    {
        int colorIndex = 7;
        unsigned long rgb = 0;
        std::string ltype = "Continuous";
        int linewt = 25;  // 1/100 mm (default 0.25 mm)
        bool off = false;     //!< layer is off (not displayed)
        bool frozen = false;  //!< layer is frozen (not displayed)
    };
    // DWG layer name -> layer style, filled in parse().
    std::map<std::string, LayerStyle> layerStyles;
    // DWG text style name -> STYLE table record, filled in parse().
    // TEXT/MTEXT entities reference these to inherit the default font, width
    // factor, oblique angle and (optional) fixed height.
    std::map<std::string, TextStyle> textStyles;
    // DWG dimension style name -> DIMSTYLE table record, filled in parse().
    // DIMENSION entities use these for arrow size, text height, extension
    // line offsets and colours (see DWGDimstyle.cpp).
    std::map<std::string, DimStyle> dimStyles;
    // DWG linetype name -> LINETYPE table record (dash pattern), filled in
    // parse().  The GUI quantizes the pattern to a 16-bit line pattern (L1);
    // the raw dashes are kept for the exact geometry-level expansion (L2).
    std::map<std::string, LineTypeInfo> linetypes;
    // Global linetype scale header variables.
    double ltscale = 1.0;    //!< LTSCALE
    double celtscale = 1.0;  //!< CELTSCALE

    //! Groups geometry by (layer, color, linetype, line weight) so that
    //! every group becomes one Part::Feature whose view properties can be
    //! set to match the original DWG appearance.  Entities that share a
    //! style are aggregated into a single compound shape, which turns tens
    //! of thousands of document objects into a handful and keeps the
    //! document, the view provider scene graph and the memory footprint
    //! small.
    struct StyleKey
    {
        std::string layer;
        unsigned long color = 0xFFFFFF;  // resolved 0xRRGGBB
        std::string ltype = "Continuous";
        int linewt = 25;  // 1/100 mm
        //! Effective entity-level linetype scale (resolved CELTSCALE).
        double ltypeScale = 1.0;
        bool operator<(const StyleKey& o) const
        {
            if (layer != o.layer) {
                return layer < o.layer;
            }
            if (color != o.color) {
                return color < o.color;
            }
            if (ltype != o.ltype) {
                return ltype < o.ltype;
            }
            if (ltypeScale != o.ltypeScale) {
                return ltypeScale < o.ltypeScale;
            }
            return linewt < o.linewt;
        }
    };
    std::map<StyleKey, std::vector<TopoDS_Shape>> layerShapes;

    //! One style run of a block definition, in block-local coordinates.
    //!
    //! A block is not one style: its content keeps its own layers, colours and
    //! linetypes, and AutoCAD displays them as such.  Only content on layer 0,
    //! and attributes that are ByBlock, follow the reference that places the
    //! block - so this keeps the *unresolved* form and lets one definition serve
    //! references that are styled differently.
    struct BlockPart
    {
        std::string layer;       //!< "" = layer 0: follows the reference
        int colorIndex = 256;    //!< ACI; 256 = ByLayer, 0 = ByBlock
        unsigned long rgb = 0;   //!< explicit true color (wins over the ACI)
        std::string ltype;       //!< "" = ByLayer, "ByBlock" = ByBlock
        //! Linetype scale contributions from the reference chain above this run
        //! (each INSERT's own DXF 48).  AutoCAD scales a pattern by LTSCALE x
        //! CELTSCALE x every entity and reference in the chain, so the dash
        //! length must come from the definition times those, never from a
        //! constant.
        double ltypeScale = 1.0;
        TopoDS_Shape shape;
    };
    //! Block name -> its style runs, filled on first use (see getBlockParts).
    std::map<std::string, std::vector<BlockPart>> blockParts;
    //! 3DFACE fast path: one style bucket (layer + resolved colour; faces are
    //! never cut by linetypes) holding the raw corners of every top-level
    //! 3DFACE.  Materialised later as a single DWGSurfaceMesh per bucket
    //! (shared vertices + one SoIndexedFaceSet), avoiding per-face OCC/Coin
    //! overhead on terrain-sized face counts.
    struct FaceKey
    {
        std::string layer;
        unsigned color;
        bool operator<(const FaceKey& o) const
        {
            if (layer != o.layer) {
                return layer < o.layer;
            }
            return color < o.color;
        }
    };
    std::map<FaceKey, std::vector<std::array<Vec3, 4>>> faceQuads;
    // Block definitions built once and reused by every INSERT through shared
    // OCC locations instead of being exploded into duplicated geometry.
    std::map<std::string, TopoDS_Shape> blockShapes;
    // Diagnostics: INSERTs whose block produced no geometry (name -> count).
    std::map<std::string, int> failedInserts;
    // POLYLINE_2D/3D -> its VERTEX_2D/3D chain, built from owner handles.
    PolyVertMap polyVerts;
    //! Optional progress callback: fraction (0..1) + phase text, driven from
    //! the GUI thread by DWGImporter::importDWG.
    std::function<void(double, const std::string&)> m_progress;

    //! Millimetres per drawing unit, from $INSUNITS (1000.0 = the drawing
    //! declares no unit, so the civil/road metres default applies - see
    //! drawingUnitToMm() and the comment block in parse()).
    //!
    //! Applied once, as the root matrix of the expand pass - every geometric
    //! quantity goes through it (entity points, INSERT chains, 3D face corners
    //! and the display polylines DWGPointBuilder produces) - plus to the
    //! length-valued scalars that never see a matrix: the text height and the
    //! linetype pattern lengths.  Line weight is NOT scaled: DWG stores it in
    //! 1/100 mm regardless of the drawing unit.
    double unitScale = 1.0;

    //! What $INSUNITS alone implies, before the sheet-plausibility check in
    //! run() may overrule it.  Civil drawings get $INSUNITS wrong often enough
    //! that the declaration alone is not safe to act on (see run()).
    double declaredUnitScale = 1.0;
    //! Name of the declared unit, for the messages.
    std::string declaredUnitName = "Unspecified";

    //! Extent of the drawing's own model-space content (see run()), and whether
    //! it could be established at all.  Used to notice an INSERT whose world
    //! extent falls outside it.
    double refX0 = 0.0, refX1 = 0.0, refY0 = 0.0, refY1 = 0.0;
    bool refValid = false;

    //! Robust extent of the same content, in document units: the 2..98
    //! percentile box of the model entities' representative points.  The plain
    //! union above is dragged towards the origin by a single mis-built entity -
    //! which is exactly the anomaly a "far too large" report is looking for -
    //! so it cannot serve as the yardstick.  Set in run().
    double robSpan = 0.0;

    //! Extend the box with the entity's defining points (a cheap approximation
    //! of its footprint: full geometry is not built yet at this stage).
    static void accumulateEntityBox(const Entity& e, double& x0, double& y0,
                                    double& x1, double& y1)
    {
        auto add = [&](const Vec3& p) {
            x0 = std::min(x0, p.x);
            x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y);
            y1 = std::max(y1, p.y);
        };
        for (const Vec3& p : e.pts) {
            add(p);
        }
        if (e.type == EntType::ARC || e.type == EntType::CIRCLE
            || e.type == EntType::ELLIPSE) {
            const double r = std::max(e.radius, norm(e.smAxis));
            add(Vec3{e.center.x - r, e.center.y - r, e.center.z});
            add(Vec3{e.center.x + r, e.center.y + r, e.center.z});
        }
        if (e.type == EntType::INSERT || e.type == EntType::TEXT
            || e.type == EntType::MTEXT) {
            add(e.insPt);
        }
    }

    void parse()
    {
        // Determine the declared string codepage so TEXT/MTEXT strings can be
        // decoded correctly (Chinese drawings usually declare ANSI_936 / GBK).
        if (dwg->header_vars.DWGCODEPAGE) {
            std::string cp = dwg->header_vars.DWGCODEPAGE;
            if (cp.compare(0, 5, "ANSI_") == 0) {
                dwgCodepage = std::atoi(cp.substr(5).c_str());
            }
            else {
                dwgCodepage = std::atoi(cp.c_str());
            }
        }

        // Collect the LAYER table: name -> (color, linetype, line weight).
        // Entities with ByLayer attributes inherit these values.
        for (unsigned long i = 0; i < dwg->num_objects; ++i) {
            const Dwg_Object* obj = &dwg->object[i];
            if (obj->supertype != DWG_SUPERTYPE_OBJECT
                || obj->fixedtype != DWG_TYPE_LAYER) {
                continue;
            }
            const Dwg_Object_LAYER* lay = obj->tio.object->tio.LAYER;
            if (!lay || !lay->name) {
                continue;
            }
            LayerStyle& ls = layerStyles[decodeDwgString(lay->name, dwgCodepage)];
            if (lay->color.method == DWG_COLOR_METHOD_TRUECOLOR) {
                // Some DWG writers store an ACI index with method=TRUECOLOR
                // and the small index value (1..255) in the rgb field.  Treat
                // it as an ACI, exactly like the entity path does, otherwise a
                // red layer (ACI 1) would decode as RGB 0x000001 (black).
                unsigned long rgb = lay->color.rgb & 0xFFFFFFUL;
                if (rgb > 0UL && rgb <= 255UL) {
                    ls.colorIndex = static_cast<int>(rgb);
                    ls.rgb = 0;
                }
                else {
                    ls.colorIndex = 0;
                    ls.rgb = rgb;
                }
            }
            else if (lay->color.method == DWG_COLOR_METHOD_ACI) {
                ls.colorIndex = lay->color.index;
                ls.rgb = 0;
            }
            else if (lay->color.rgb != 0) {
                unsigned long rgb = lay->color.rgb & 0xFFFFFFUL;
                if (rgb > 0UL && rgb <= 255UL) {
                    ls.colorIndex = static_cast<int>(rgb);
                    ls.rgb = 0;
                }
                else {
                    ls.colorIndex = 0;
                    ls.rgb = rgb;
                }
            }
            else if (lay->color.index > 0 && lay->color.index < 256) {
                ls.colorIndex = lay->color.index;
                ls.rgb = 0;
            }
            else {
                ls.colorIndex = 7;
                ls.rgb = 0;
            }
            ls.linewt = lay->linewt;
            ls.off = (lay->off != 0);
            ls.frozen = (lay->frozen != 0);
            if (lay->ltype) {
                int err = 0;
                // The layer's linetype field is a *reference*: it has to be
                // resolved against the object map before it has a name, exactly
                // like the shape-style reference in the complex-linetype code
                // above.  Reading lay->ltype->obj directly left every layer on
                // the default "Continuous" - only entities that carry a literal
                // linetype name came out right - so all ByLayer linetypes inside
                // blocks were drawn solid (reported on the bridge plan: the pile
                // content on layer 虚线 must be dashed like AutoCAD shows it).
                Dwg_Object* lt =
                    dwg_ref_object(const_cast<Dwg_Data*>(dwg), lay->ltype);
                char* n = lt ? dwg_obj_table_get_name(lt, &err) : nullptr;
                if (n) {
                    ls.ltype = decodeDwgString(n, dwgCodepage);
                }
            }
        }

        // Collect the STYLE table (text styles): name -> (font file, big
        // font, width factor, oblique, generation, optional fixed height).
        // TEXT/MTEXT entities inherit these defaults; entity-level fields
        // override them.
        for (unsigned long i = 0; i < dwg->num_objects; ++i) {
            const Dwg_Object* obj = &dwg->object[i];
            if (obj->supertype != DWG_SUPERTYPE_OBJECT
                || obj->fixedtype != DWG_TYPE_STYLE) {
                continue;
            }
            const Dwg_Object_STYLE* st = obj->tio.object->tio.STYLE;
            if (!st || !st->name) {
                continue;
            }
            TextStyle& ts = textStyles[decodeDwgString(st->name, dwgCodepage)];
            ts.name = decodeDwgString(st->name, dwgCodepage);
            ts.fontFile = st->font_file ? st->font_file : "";
            ts.bigFontFile = st->bigfont_file ? st->bigfont_file : "";
            ts.textSize = st->text_size;
            ts.widthFactor = (st->width_factor > 0.0) ? st->width_factor : 1.0;
            ts.oblique = st->oblique_angle;
            ts.generation = static_cast<int>(st->generation);
            ts.isVertical = (st->is_vertical != 0) || ((st->flag & 1) != 0);
            if (!ts.isVertical && st->font_file) {
                std::string ff = st->font_file;
                std::transform(ff.begin(), ff.end(), ff.begin(), ::tolower);
                if (!ff.empty() && ff[0] == '@') {
                    ts.isVertical = true;
                }
            }
            SWDWG_LOG(
                "SWDWG: STYLE '%s' is_vertical=%d flag=%d font_file='%s'\n",
                ts.name.c_str(),
                ts.isVertical ? 1 : 0,
                static_cast<int>(st->flag),
                ts.fontFile.c_str());
        }

        // Collect the DIMSTYLE table (dimension styles).  DIMENSION entities
        // reference a style by name; missing attributes fall back to the
        // AutoCAD defaults baked into DimStyle.
        dimStyles = parseDimStyles(dwg, dwgCodepage);
        if (!dimStyles.empty()) {
            std::ostringstream oss;
            oss << "SWDWG: DIMSTYLEs:";
            for (const auto& kv : dimStyles) {
                oss << " '" << kv.first << "'";
            }
            SWDWG_LOG("%s\n", oss.str().c_str());
        }

        // Global linetype scale header variables.  The effective dash length
        // of a pattern element is definition * LTSCALE * CELTSCALE (the
        // per-entity override lives in the entity's ltype_scale / DXF 48).
        if (dwg->header_vars.LTSCALE > 0.0) {
            ltscale = dwg->header_vars.LTSCALE;
        }
        if (dwg->header_vars.CELTSCALE > 0.0) {
            celtscale = dwg->header_vars.CELTSCALE;
        }
        SWDWG_LOG(
            "[SWDWG LTS] rawLTSCALE=%.6g rawCELTSCALE=%.6g "
            "effLTSCALE=%.6g version=%d\n",
            dwg->header_vars.LTSCALE, dwg->header_vars.CELTSCALE, ltscale,
            (int)dwg->header.version);

        // Declared drawing unit ($INSUNITS, with the $MEASUREMENT fallback).
        // Always reported, so every import states what the file claims: a
        // drawing that declares metres but is read as if the numbers were
        // millimetres comes in 1000x small, and this line is where that shows.
        // The conversion itself is applied when the geometry is built, through
        // the unit matrix of the expand pass.
        //
        // A drawing that omits $INSUNITS (insunits==0) defaults to metres
        // (drawingUnitToMm -> 1000.0 mm).  Civil/road DWG without
        // $INSUNITS is overwhelmingly authored with metres in mind, and the
        // millimetre default would import it 1000x too small.
        {
            const int insunits = static_cast<int>(dwg->header_vars.INSUNITS);
            const int measurement =
                static_cast<int>(dwg->header_vars.MEASUREMENT);
            const int units = effectiveDrawingUnit(insunits);
            unitScale = drawingUnitToMm(units);
            declaredUnitScale = unitScale;
            declaredUnitName = drawingUnitName(units);
            // NOTE: the linetype patterns are scaled (and the scale is possibly
            // overruled) in run(), once the paper-space sheet size is known -
            // $INSUNITS is only a declaration.  Reported here as declared.
            if (insunits == 0) {
                Base::Console().message(
                    "[SWDWG] unit: INSUNITS=0 (unspecified, defaulting to "
                    "metres) MEASUREMENT=%d -> 1 drawing unit = %.6g mm "
                    "(geometry, text height and linetype lengths scaled)\n",
                    measurement, unitScale);
            }
            else {
                Base::Console().message(
                    "[SWDWG] unit: INSUNITS=%d (%s) MEASUREMENT=%d -> 1 drawing "
                    "unit = %.6g mm%s\n",
                    insunits, declaredUnitName.c_str(), measurement, unitScale,
                    unitScale == 1.0
                        ? " (no scaling)"
                        : " (geometry, text height and linetype lengths scaled)");
            }
        }

        // Collect the LINETYPE table: name -> dash pattern.  The GUI maps
        // the quantized 16-bit pattern onto the FreeCAD DrawStyle enum (L1);
        // the raw dash list is kept for the geometry-level expansion (L2)
        // that honours LTSCALE / CELTSCALE in world units.
        for (unsigned long i = 0; i < dwg->num_objects; ++i) {
            const Dwg_Object* obj = &dwg->object[i];
            if (obj->supertype != DWG_SUPERTYPE_OBJECT
                || obj->fixedtype != DWG_TYPE_LTYPE) {
                continue;
            }
            const Dwg_Object_LTYPE* lt = obj->tio.object->tio.LTYPE;
            if (!lt || !lt->name) {
                continue;
            }
            LineTypeInfo li;
            li.name = decodeDwgString(lt->name, dwgCodepage);
            li.desc = lt->description
                ? decodeDwgString(lt->description, dwgCodepage)
                : "";
            li.alignment = static_cast<char>(lt->alignment);
            li.patternLen = lt->pattern_len;
            li.dashes.reserve(lt->numdashes);
            for (int k = 0; k < lt->numdashes && lt->dashes; ++k) {
                const Dwg_LTYPE_dash& sd = lt->dashes[k];
                LineTypeDash dd;
                dd.length = sd.length;
                // libredwg shape_flag bits: 1 = absolute rotation, 2 = text
                // element, 4 = shape element.  Our internal LineTypeDash uses
                // 2 = shape, 4 = text so that formatDashList() / addSymbolGeometry()
                // can branch unambiguously.
                if (sd.shape_flag & LTYPE_SHAPE_FLAG_IS_SHAPE) {
                    dd.shapeFlag = 2;
                }
                else if (sd.shape_flag & LTYPE_SHAPE_FLAG_IS_TEXT) {
                    dd.shapeFlag = 4;
                }
                else {
                    dd.shapeFlag = 0;
                }
                dd.complexShapecode = sd.complex_shapecode;
                dd.text = sd.text ? decodeDwgString(sd.text, dwgCodepage) : "";
                dd.scale = sd.scale;
                dd.rotation = sd.rotation;
                dd.xOffset = sd.x_offset;
                dd.yOffset = sd.y_offset;
                // The embedded shape lives in the .shx file named by the
                // dash' STYLE reference (DXF 9 of a complex linetype).
                if (sd.style) {
                    Dwg_Object* styleObj = dwg_ref_object(
                        const_cast<Dwg_Data*>(dwg), sd.style);
                    if (styleObj && styleObj->supertype == DWG_SUPERTYPE_OBJECT
                        && styleObj->fixedtype == DWG_TYPE_STYLE) {
                        const Dwg_Object_STYLE* st =
                            styleObj->tio.object->tio.STYLE;
                        if (st && st->font_file) {
                            dd.shapefile = st->font_file;
                            dd.widthFactor =
                                (st->width_factor > 0.0) ? st->width_factor : 1.0;
                        }
                    }
                }
                // A zero-length shape dash reserves a run equal to the
                // shape's bounding-box X span x S (AutoCAD computes it from
                // the .shx; the DWG stores no length).  Resolve the file once
                // here so dashLayoutWidth() can size the run without
                // re-parsing at cut time.
                if (dd.shapeFlag == 2 && !dd.shapefile.empty()) {
                    if (const auto sfx = loadShapeFile(dd.shapefile)) {
                        dd.symWidth = shapeXSpan(*sfx, dd.complexShapecode);
                    }
                }
                li.dashes.push_back(std::move(dd));
            }
            if (li.dashes.empty()) {
                // CONTINUOUS or malformed entry: keep a sentinel dash so the
                // table lookup still distinguishes it from a missing entry.
                li.dashes.push_back(LineTypeDash{});
            }
            std::ostringstream dss;
            dss << "[SWDWG LINETYPE] name='" << li.name << "' pattern_len=" << li.patternLen
                << " numdashes=" << lt->numdashes << " dashes=";
            for (const auto& d : li.dashes) {
                dss << d.length << " ";
            }
            SWDWG_LOG("%s\n", dss.str().c_str());
            // Detailed dump for complex dashes so we can see what libredwg
            // actually decoded (shape_flag bits, shape number / string index,
            // text payload and resolved .shx file).
            for (const auto& d : li.dashes) {
                if (d.shapeFlag != 0) {
                    SWDWG_LOG(
                        "  [SWDWG LTYPE DASH] len=%.6g shapeFlag=%d "
                        "code=%d text='%s' shapefile='%s' scale=%.4g rot=%.4g "
                        "off=(%.4g,%.4g) symWidth=%.4g\n",
                        d.length, d.shapeFlag, d.complexShapecode,
                        d.text.c_str(), d.shapefile.c_str(), d.scale,
                        d.rotation, d.xOffset, d.yOffset, d.symWidth);
                }
            }
            linetypes[li.name] = std::move(li);
        }

        // Collect VERTEX_2D/VERTEX_3D into their owning POLYLINE.  We resolve
        // the owner through each vertex' owner handle (DXF 330); the generic
        // owned-entity chain of a POLYLINE is not reliably decoded by libredwg.
        for (unsigned long i = 0; i < dwg->num_objects; ++i) {
            const Dwg_Object* obj = &dwg->object[i];
            if (obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity) {
                continue;
            }
            const Dwg_Object_Entity* ent = obj->tio.entity;
            if (obj->fixedtype == DWG_TYPE_VERTEX_2D && ent->ownerhandle) {
                Dwg_Object* owner =
                    dwg_ref_object(const_cast<Dwg_Data*>(dwg), ent->ownerhandle);
                if (owner && owner->fixedtype == DWG_TYPE_POLYLINE_2D) {
                    const Dwg_Entity_VERTEX_2D* V = ent->tio.VERTEX_2D;
                    polyVerts[owner].push_back(PolyVert{
                        Vec3{V->point.x, V->point.y, V->point.z}, V->bulge,
                        static_cast<unsigned char>(V->flag)});
                }
            }
            else if (obj->fixedtype == DWG_TYPE_VERTEX_3D && ent->ownerhandle) {
                Dwg_Object* owner =
                    dwg_ref_object(const_cast<Dwg_Data*>(dwg), ent->ownerhandle);
                if (owner && owner->fixedtype == DWG_TYPE_POLYLINE_3D) {
                    const Dwg_Entity_VERTEX_3D* V = ent->tio.VERTEX_3D;
                    polyVerts[owner].push_back(PolyVert{
                        Vec3{V->point.x, V->point.y, V->point.z}, 0.0,
                        static_cast<unsigned char>(V->flag)});
                }
            }
        }

        // A curve/spline-fit POLYLINE stores two kinds of vertices: the frame
        // control points (which define the fitted curve) and the extra
        // fit points (points on the fitted curve).  Only the control points
        // must be used to rebuild the spline.  If a fit polyline carries no
        // control points at all, keep the full list as a fallback.
        for (auto& kv : polyVerts) {
            const Dwg_Object* owner = kv.first;
            if (!owner || owner->supertype != DWG_SUPERTYPE_ENTITY
                || !owner->tio.entity) {
                continue;
            }
            const Dwg_Object_Entity* ent = owner->tio.entity;
            bool splineFit = false;
            bool curveFit = false;
            if (owner->fixedtype == DWG_TYPE_POLYLINE_2D) {
                const Dwg_Entity_POLYLINE_2D* P = ent->tio.POLYLINE_2D;
                splineFit = (P->flag & 0x4) != 0;
                curveFit = (P->flag & 0x2) != 0;
            }
            else if (owner->fixedtype == DWG_TYPE_POLYLINE_3D) {
                const Dwg_Entity_POLYLINE_3D* P = ent->tio.POLYLINE_3D;
                splineFit = (P->flag & 0x4) != 0;
            }
            if (!splineFit && !curveFit) {
                continue;
            }
            std::vector<PolyVert> filtered;
            filtered.reserve(kv.second.size());
            for (const PolyVert& v : kv.second) {
                if (splineFit && (v.flag & 0x10) == 0) {
                    continue;  // fit point, not a frame control point
                }
                if (curveFit && !splineFit && (v.flag & 0x1) != 0) {
                    continue;  // extra vertex created by curve fitting
                }
                filtered.push_back(v);
            }
            if (!filtered.empty()) {
                kv.second = std::move(filtered);
            }
        }

        // Map every entity to the block it is owned by.
        for (unsigned long i = 0; i < dwg->num_objects; ++i) {
            const Dwg_Object* obj = &dwg->object[i];
            if (obj->supertype != DWG_SUPERTYPE_OBJECT
                || obj->fixedtype != DWG_TYPE_BLOCK_HEADER) {
                continue;
            }
            int err = 0;
            char* name = dwg_obj_table_get_name(obj, &err);
            if (!name) {
                continue;
            }
            // Decode the name exactly the way extractEntity() decodes the
            // INSERT's block reference (Entity::blockName).  Using the raw
            // bytes as the key here meant the two never matched unless the
            // name was pure ASCII: getBlockShape() looks itself up by the
            // decoded name, found nothing, and returned a null shape, so every
            // block with a non-ASCII name silently lost all of its geometry
            // (measured on a bridge plan: +571 edges, and an entire layer's
            // worth of inserted content, appeared once the two sides agreed).
            const std::string blockName = decodeDwgString(name, dwgCodepage);
            const Dwg_Object* e = get_first_owned_entity(obj);
            while (e) {
                if (e->supertype == DWG_SUPERTYPE_ENTITY) {
                    entBlock[e] = blockName;
                }
                e = get_next_owned_entity(obj, e);
            }
        }

        // Extract entities into per-block definitions / model space.
        for (unsigned long i = 0; i < dwg->num_objects; ++i) {
            const Dwg_Object* obj = &dwg->object[i];
            if (obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity) {
                continue;
            }
            Entity e;
            if (!extractEntity(dwg, obj, e, polyVerts, dwgCodepage,
                               &textStyles)) {
                // Sub-entities and block bookkeeping objects are consumed by
                // their parent (POLYLINE, BLOCK_HEADER); don't flag them.
                switch (obj->fixedtype) {
                    case DWG_TYPE_VERTEX_2D:
                    case DWG_TYPE_VERTEX_3D:
                    case DWG_TYPE_VERTEX_MESH:
                    case DWG_TYPE_VERTEX_PFACE:
                    case DWG_TYPE_VERTEX_PFACE_FACE:
                    case DWG_TYPE_SEQEND:
                    case DWG_TYPE_BLOCK:
                    case DWG_TYPE_ENDBLK:
                    // Invisible ATTRIB are deliberately skipped in extraction
                    // (AutoCAD hides them unless ATTDISP is forced on); they
                    // are not "unsupported".
                    case DWG_TYPE_ATTRIB:
                        break;
                    default:
                        ++unsupportedCount[(int)obj->fixedtype];
                }
                continue;
            }
            ++parsedCount[(int)e.type];
            auto it = entBlock.find(obj);
            if (it != entBlock.end()) {
                blockDefs[it->second].push_back(std::move(e));
            }
            else {
                modelEnts.push_back(std::move(e));
            }
        }
    }

    //! Expand all model-space entities (including INSERT references) and
    //! create the corresponding FreeCAD objects in the document:
    //!  - geometry is aggregated per layer into a single Part::Feature each;
    //!  - block definitions are built once and reused through shared OCC
    //!    locations, so inserts only pay for their transformation;
    //!  - text annotations stay individual objects (they must).
    bool run(App::Document* doc, int& created, int& skipped)
    {
        this->doc = doc;
        std::map<std::string, int> counters;
        // Root matrix of the whole import: a pure scale when the drawing
        // declares a unit other than millimetres.  Every geometric quantity is
        // built through it - entity points, INSERT chains, 3D face corners and
        // the display polylines - so the model, what the display strokes and
        // what SWDTM reads stay at one scale.  (Block definitions keep their
        // own identity matrix and are scaled at the INSERT site, through this
        // same root.)
        // $INSUNITS is a declaration, and civil drawings get it wrong often
        // enough not to act on it blindly: the 条件图 road plan of this project
        // declares metres (INSUNITS=6) while its content is in millimetres -
        // its plan spans 848305 units and its paper-space sheet measures
        // 1383 x 238 units, i.e. a 1383 x 238 mm strip, whereas the
        // declaration would make the sheet 1.4 km long.  Honouring it produced
        // an 848 km document in which every entity box came out 1000x the
        // model-space reference and every INSERT was reported as landing
        // outside the drawing.
        //
        // Two independent sanity signals, and only a declaration that BOTH
        // reject is overruled - one polluted signal must not be enough, since
        // a stray entity can stretch the model box and a title-block insert
        // can stretch the paper box:
        //   * the model-space content, expressed in the declared unit, would
        //     span more than 10 km across (no sheet in this project does);
        //   * the paper-space sheet, expressed in the declared unit, would be
        //     shorter than 50 mm or longer than 20 m (not a sheet size).
        // The file's own numbers are then read as millimetres, which is what
        // the software that wrote them drew.
        if (declaredUnitScale != 1.0) {
            double sx0 = 1e30, sy0 = 1e30, sx1 = -1e30, sy1 = -1e30;
            // The model-space content is the *Model_Space block plus the
            // entities outside every block - the same set the expand pass
            // builds (todo), assembled here because todo is not built yet.
            for (const auto& kv : blockDefs) {
                if (kv.first != "*Model_Space") {
                    continue;
                }
                for (const Entity& e : kv.second) {
                    accumulateEntityBox(e, sx0, sy0, sx1, sy1);
                }
            }
            for (const Entity& e : modelEnts) {
                accumulateEntityBox(e, sx0, sy0, sx1, sy1);
            }
            const bool haveModel = (sx0 <= sx1 && sy0 <= sy1);
            double px0 = 1e30, py0 = 1e30, px1 = -1e30, py1 = -1e30;
            for (const auto& kv : blockDefs) {
                if (kv.first.rfind("*Paper_Space", 0) != 0) {
                    continue;
                }
                for (const Entity& pe : kv.second) {
                    accumulateEntityBox(pe, px0, py0, px1, py1);
                }
            }
            const bool haveSheet = (px0 <= px1 && py0 <= py1);
            const double modelMm =
                haveModel ? std::max(sx1 - sx0, sy1 - sy0) * declaredUnitScale
                          : 0.0;
            const double sheetMm =
                haveSheet ? std::max(px1 - px0, py1 - py0) * declaredUnitScale
                          : 0.0;
            const bool modelImplausible = haveModel && modelMm > 1.0e7;
            const bool sheetImplausible =
                haveSheet && (sheetMm < 50.0 || sheetMm > 20000.0);
            // BOTH signals must exist and reject.  With only one available the
            // rule would act on a single view of the file, and a metre drawing
            // of, say, a 15 km corridor (no paper space at all, model signal
            // alone) would be mis-read as millimetres.  The 条件图 this rule
            // was written for has both (128 paper-space layouts), so requiring
            // both keeps that fix and removes the risk.  The 辅道/dxt style
            // drawings that declare mm are unaffected; drawings that declare
            // nothing are now also caught by the rule (declaredUnitScale=1000)
            // and read as mm if the sheet and model are both implausible at
            // metre scale - i.e. they were authored in mm without saying so.
            if (haveModel && haveSheet && modelImplausible
                && sheetImplausible) {
                std::string why = "its model space would span "
                    + std::to_string(static_cast<long long>(modelMm / 1000.0))
                    + " m and its paper-space sheet would be "
                    + std::to_string(static_cast<long long>(sheetMm))
                    + " mm long";
                unitScale = 1.0;
                // Reported as a message, not a warning: nothing is wrong with
                // the import - the file's own $INSUNITS is what is
                // inconsistent, and the drawing is read at the scale it was
                // drawn at.  The "effective" line below states the result.
                Base::Console().message(
                    "[SWDWG] unit: the file declares %s (1 unit = %.6g mm) but "
                    "%s - neither is a drawing sheet.  Reading the drawing as "
                    "millimetres (1 unit = 1 mm) instead.\n",
                    declaredUnitName.c_str(), declaredUnitScale, why.c_str());
            }
        }
        if (unitScale != 1.0) {
            // A linetype pattern is authored in drawing units too, and the
            // geometry-level (L2) expansion lays it out in world units, so the
            // same factor has to reach it or every dash comes out unitScale
            // times too short.
            ltscale *= unitScale;
            celtscale *= unitScale;
        }

        Mat4 id = (unitScale != 1.0)
            ? insertMatrix(Vec3{0.0, 0.0, 0.0},
                           Vec3{unitScale, unitScale, unitScale}, 0.0)
            : matIdentity();

        // Diagnostics: block bookkeeping (commented out).
        // SWDWG_LOG("SWDWG: %zu block definition(s), %zu stray "
        //                     "model-space entity(ies)\n",
        //                     blockDefs.size(), modelEnts.size());
        // for (const auto& kv : blockDefs) {
        //     if (kv.first == "*Model_Space") {
        //         SWDWG_LOG("SWDWG:   [*Model_Space] -> %zu "
        //                             "entity(ies)\n",
        //                             kv.second.size());
        //     }
        //     else {
        //         SWDWG_LOG("SWDWG:   block '%s' -> %zu "
        //                             "entity(ies)\n",
        //                             kv.first.c_str(), kv.second.size());
        //     }
        // }

        std::vector<Entity> todo;
        // Model space = "*Model_Space" block + stray entities.
        // Model space is looked up case-insensitively: AutoCAD uses the
        // canonical "*Model_Space", but files written by some other tools
        // store it all-caps ("*MODEL_SPACE"); content there is the drawing
        // itself and must be expanded.
        auto ms = blockDefs.end();
        for (auto it = blockDefs.begin(); it != blockDefs.end(); ++it) {
            if (eqIgnoreCaseAscii(it->first, "*Model_Space")) {
                ms = it;
                break;
            }
        }
        if (ms != blockDefs.end()) {
            todo.insert(todo.end(), ms->second.begin(), ms->second.end());
        }
        todo.insert(todo.end(), modelEnts.begin(), modelEnts.end());

        // Space/block diagnostics: when the import creates nothing this is the
        // first place to look (content may live in *Paper_Space or a
        // non-standard "model space" block instead of *Model_Space).
        auto blockSizeOf = [&](const char* key) {
            auto it = blockDefs.find(key);
            return it == blockDefs.end() ? std::size_t(0) : it->second.size();
        };
        Base::Console().log(
            "[SWDWG] space: defs=%zu *Model_Space=%s(%zu) "
            "*Paper_Space=%zu stray=%zu\n",
            blockDefs.size(), ms != blockDefs.end() ? "yes" : "NO",
            blockSizeOf("*Model_Space"), blockSizeOf("*Paper_Space"),
            modelEnts.size());
        if (ms == blockDefs.end() && modelEnts.empty()) {
            std::string names;
            int shown = 0;
            for (const auto& kv : blockDefs) {
                if (shown >= 30) {
                    names += " ...";
                    break;
                }
                names += " '" + kv.first + "'="
                    + std::to_string(kv.second.size());
                ++shown;
            }
            Base::Console().log(
                "[SWDWG] no *Model_Space content; blocks:%s\n",
                names.c_str());
        }

        // Expand phase progress: from ~0.35 to ~0.90 across the top-level
        // entities (each INSERT recursion is counted once, at its top level).
        const double expStart = 0.35;
        const double expSpan = 0.55;
        const std::size_t total = todo.empty() ? 1 : todo.size();
        double lastReport = -1.0;
        std::size_t done = 0;
        auto reportExpand = [&]() {
            if (!m_progress) {
                return;
            }
            double f = expStart + expSpan * (double)done / (double)total;
            if (f - lastReport >= 0.02 || done == total) {  // throttle ~2%
                lastReport = f;
                m_progress(f, "Creating document objects (" + std::to_string(done)
                                  + "/" + std::to_string(total) + ")");
            }
        };
        // $INSUNITS is a declaration, and civil drawings get it wrong: the
        // 条件图 road plan of this project declares metres (INSUNITS=6) while
        // its content is in millimetres - the plan spans 848305 units and its
        // paper-space sheet measures 1383 x 238 units, i.e. a 1383 x 238 mm
        // strip, while the declaration would make it a 1.4 km long sheet.
        // Honouring it produced an 848 km document: every entity box came out
        // 1000x the model-space reference and every INSERT was reported as
        // landing outside the drawing.  The sheet in paper space is the tie
        // breaker - a real sheet is 50..20000 mm on its long side - so a
        // declared factor that makes the sheet fall outside that window is
        // ignored and the file's numbers are read as millimetres, which is
        // what the software that wrote them drew.
        if (unitScale != declaredUnitScale) {
            Base::Console().message(
                "[SWDWG] unit: effective 1 drawing unit = %.6g mm "
                "(the %s declaration was not usable)\n",
                unitScale, declaredUnitName.c_str());
        }

        // Reference extent for the "landed outside the drawing" check in the
        // INSERT branch: the model-space entities are drawn where they belong,
        // so their union is where inserted content is expected to land.  Point
        // positions are enough for that purpose.
        //
        // Scaled into document units: every shape is built through unitScale,
        // so comparing it against raw drawing units made every INSERT of a
        // metre-declared, millimetre-content drawing look like it landed
        // 1000x outside the sheet.
        refX0 = refY0 = 1e30;
        refX1 = refY1 = -1e30;
        for (const Entity& e : todo) {
            accumulateEntityBox(e, refX0, refY0, refX1, refY1);
        }
        refValid = (refX0 <= refX1 && refY0 <= refY1);
        if (refValid && unitScale != 1.0) {
            refX0 *= unitScale;
            refX1 *= unitScale;
            refY0 *= unitScale;
            refY1 *= unitScale;
        }

        // Robust yardstick for the "far too large" report: the union box above
        // is dragged to the origin by the very kind of mis-built entity that
        // report is meant to catch (the 辅道 plan's broken hatch spanned from
        // (0,0) to the site, so the union it created could not be used to
        // judge it).  The 2..98 percentile box of the entities' representative
        // points describes where the drawing actually is instead, and it is
        // what reportOversized() compares against - so the limit means "twice
        // the drawing" in a 5-unit detail and in a 4.8 km survey alike, with
        // the old 20 m floor only as a last resort when no reference exists.
        {
            std::vector<double> xs;
            std::vector<double> ys;
            auto addRep = [&](const Entity& e) {
                if (!e.pts.empty()) {
                    xs.push_back(e.pts.front().x);
                    ys.push_back(e.pts.front().y);
                }
            };
            for (const auto& kv : blockDefs) {
                if (kv.first != "*Model_Space") {
                    continue;
                }
                for (const Entity& e : kv.second) {
                    addRep(e);
                }
            }
            for (const Entity& e : modelEnts) {
                addRep(e);
            }
            if (xs.size() >= 8) {
                std::sort(xs.begin(), xs.end());
                std::sort(ys.begin(), ys.end());
                const std::size_t lo = xs.size() / 50;
                const std::size_t hi = xs.size() - 1 - lo;
                robSpan = std::max(xs[hi] - xs[lo], ys[hi] - ys[lo])
                    * unitScale;
            }
            Base::Console().log(
                "[SWDWG] extent: model entities=%zu robustSpan=%.0f "
                "union x=[%.0f,%.0f] y=[%.0f,%.0f] (document units, 1 unit = "
                "%.6g mm); the oversize report uses max(2*robustSpan, 20000)\n",
                xs.size(), robSpan, refX0, refX1, refY0, refY1, unitScale);
        }

        reportExpand();
        for (const Entity& e : todo) {
            expand(doc, e, id, 0, counters, created, skipped);
            ++done;
            reportExpand();
        }

        // Text annotations are individually complete the moment they are
        // created (their layout was computed inside createAnnotation), so they
        // must NOT be re-executed by the doc->recompute() below.  Leaving
        // 17k+ touched annotation objects dirty made that single recompute
        // take ~20 s of per-object work.  Clear their touched flag here; the
        // following recompute then only re-executes the geometry groups.
        for (App::DocumentObject* o : doc->getObjects()) {
            if (dynamic_cast<SWDWG::DWGText*>(o)) {
                o->purgeTouched();
            }
        }

        // Materialise each style group's geometry as one DWGLineGroup (a
        // Part::Feature subclass).  One shared DWGLineType definition object
        // per non-continuous LINETYPE name; every group using the linetype
        // links to it through DWGLineTypeRef, so editing the definition
        // updates all linked lines uniformly (geometry + style, see
        // DWGLineType and DWGLineGroup).
        ImpStopwatch _grp(g_groupSec);
        std::map<std::string, App::DocumentObject*> linetypeObjects;
        int layerIdx = 0;
        int nGroups = 0;
        std::size_t nShapes = 0;
        for (auto& kv : layerShapes) {
            if (kv.second.empty()) {
                continue;
            }
            ++nGroups;
            nShapes += kv.second.size();
            const StyleKey& k = kv.first;
            TopoDS_Shape shape;
            if (kv.second.size() == 1) {
                shape = kv.second.front();
            }
            else {
                BRep_Builder B;
                TopoDS_Compound comp;
                B.MakeCompound(comp);
                for (const TopoDS_Shape& s : kv.second) {
                    B.Add(comp, s);
                }
                shape = comp;
            }
            std::string objName = "Layer_" + std::to_string(layerIdx++);
            auto* feat = doc->addObject<SWDWG::DWGLineGroup>(objName.c_str());
            if (!feat) {
                continue;
            }
            // SWDWG_LOG("SWDWG:   layer '%s' %s -> %zu shape(s), bbox %s\n",
            //                     k.layer.c_str(), k.ltype.c_str(),
            //                     kv.second.size(), shapeBBox(shape).c_str());
            // Raw geometry is stored un-cut; execute() re-cuts it with the
            // linetype pattern on every recompute (L2 geometry cutting).
            //
            // DWGSourceShape (binary BRep) is the persisted geometry; Shape is
            // the transient mirror that Part tools / scripts read (see
            // DWGLineGroup).  Both are filled because the display view provider
            // and SWDTM's contour extraction read DWGSourceShape while the rest
            // of FreeCAD reads Shape - and a TopoDS_Shape is a handle, so the
            // second assignment shares the geometry instead of copying it.
            // Flatten the location before handing the shape to FreeCAD.
            // Part::Feature moves a shape's own TopLoc_Location into the
            // object's Placement on assignment, and the group compound can carry
            // one (a rigid insert transform is a location, see transformShape).
            // With per-style runs inside one group that hoisted Placement is
            // then applied to every run a second time, which is what put content
            // away from the sheet: measured on the bridge drawing, group
            // 'Layer - 标注' reported Placement (-571.0, 3372.53) while its
            // geometry sat elsewhere.  Baking the location leaves Placement at
            // zero and the display identical.
            TopoDS_Shape flat = shape;
            if (!shape.Location().IsIdentity()) {
                try {
                    // Copying with an identity transform bakes the shape's own
                    // location into its geometry and leaves Location() identity.
                    BRepBuilderAPI_Transform tf(shape, gp_Trsf(),
                                                Standard_True);
                    if (!tf.Shape().IsNull()) {
                        flat = tf.Shape();
                    }
                }
                catch (const Standard_Failure&) {
                    flat = shape;
                }
            }
            feat->DWGSourceShape.setValue(flat);
            feat->Shape.setValue(flat);
            feat->Label.setValue(("Layer - " + k.layer).c_str());
            feat->Layer.setValue(k.layer.c_str());
            feat->EntityKind.setValue("LINE");
            // Resolved style, consumed by the GUI to apply it to the view
            // provider (line color / weight / linetype).
            feat->DWGColor.setValue(Base::Color(
                ((k.color >> 16) & 0xFF) / 255.0f,
                ((k.color >> 8) & 0xFF) / 255.0f,
                (k.color & 0xFF) / 255.0f));
            feat->DWGLineWidth.setValue(k.linewt / 100.0);
            feat->DWGLineType.setValue(k.ltype.c_str());
            // Quantized 16-bit line pattern (mapping level L1): the LINETYPE
            // table pattern normalized to the Coin3D SoDrawStyle pattern.
            // 0xffff = CONTINUOUS; a missing table entry also leaves 0xffff
            // so the GUI falls back to name matching in that case.
            unsigned short pattern = 0xffff;
            auto lit = linetypes.find(k.ltype);
            if (lit != linetypes.end()) {
                pattern = quantizeLinePattern(lit->second.dashes);
            }
            feat->DWGLinePattern.setValue((int)pattern);
            // This line's own linetype scale (LTSCALE x the entity's scale).
            // It is the base every renderer scales the pattern by - the shared
            // DWGLineType only contributes ScaleOverride on top - so it stays a
            // per-group value (see effectiveLineScale()).
            feat->DWGLineScale.setValue(ltscale * k.ltypeScale);
            // Link the group to its shared DWGLineType definition so that
            // editing the pattern / scale updates every line that uses this
            // linetype at once.  CONTINUOUS (or unknown) linetypes stay
            // unlinked.
            App::DocumentObject* ltObj = nullptr;
            auto lti = linetypes.find(k.ltype);
            // A linetype gets an L2 definition object when its pattern is not
            // CONTINUOUS - OR when it carries an embedded complex element.
            // TRACKS ("3.81,3.81[TRACK1,...]") quantizes to 0xffff because
            // both dashes are pen-down, but it still needs L2 expansion to
            // stamp the TRACK1 shape; without this it falls back to a raw
            // (uncut) line.
            bool hasComplex = false;
            if (lti != linetypes.end()) {
                for (const LineTypeDash& d : lti->second.dashes) {
                    if (d.shapeFlag != 0) {
                        hasComplex = true;
                        break;
                    }
                }
            }
            if (lti != linetypes.end()
                && (quantizeLinePattern(lti->second.dashes) != 0xffff
                    || hasComplex)) {
                auto mit = linetypeObjects.find(k.ltype);
                if (mit != linetypeObjects.end()) {
                    ltObj = mit->second;
                }
                else {
                    auto* lt = doc->addObject<SWDWG::DWGLineType>();
                    if (lt) {
                        lt->Label.setValue(
                            ("LineType - " + k.ltype).c_str());
                        lt->Name.setValue(k.ltype.c_str());
                        lt->EntityKind.setValue("LINETYPE");
                        // Stored pattern length (drawing units): the cutter
                        // uses it to keep a line of exactly N x pattern_len
                        // a whole number of periods (see DWGLineCut.h).
                        lt->setPatternLen(lti->second.patternLen);
                        const std::string patStr =
                            formatDashList(lti->second.dashes);
                        SWDWG_LOG(
                            "[SWDWG LTCREATE] '%s' pattern='%s' rawN=%zu\n",
                            k.ltype.c_str(), patStr.c_str(),
                            lti->second.dashes.size());
                        lt->Pattern.setValue(patStr.c_str());
                        // Only the drawing-global factor belongs on a shared
                        // definition.  k.ltypeScale is this entity's own linetype
                        // scale, and entities sharing a linetype routinely have
                        // different ones (0.2 .. 10 in this drawing) - storing
                        // the first one here is what made the renderer cut every
                        // other group at that foreign scale.  The per-entity
                        // value stays on the group (DWGLineScale below).
                        lt->Scale.setValue(ltscale);
                        ltObj = lt;
                        linetypeObjects[k.ltype] = lt;
                    }
                }
            }
            {
                static int s_linkLogs = 0;
                const bool logLink = s_linkLogs < 12;
                if (logLink) {
                    ++s_linkLogs;
                }
                else if (s_linkLogs == 12) {
                    ++s_linkLogs;
                    SWDWG_LOG("[SWDWG LINK] further group link logs "
                              "suppressed\n");
                }
                if (ltObj) {
                    feat->DWGLineTypeRef.setValue(ltObj);
                }
                if (logLink) {
                    if (ltObj) {
                        SWDWG_LOG(
                            "[SWDWG LINK] group '%s' ltype='%s' -> ltObj "
                            "'%s' (ref=%p)\n",
                            objName.c_str(), k.ltype.c_str(),
                            ltObj->getNameInDocument(),
                            static_cast<void*>(ltObj));
                    }
                    else {
                        SWDWG_LOG(
                            "[SWDWG LINK] group '%s' ltype='%s' -> no ltObj "
                            "(pattern=0x%x)\n",
                            objName.c_str(), k.ltype.c_str(), pattern);
                    }
                }
            }
        }

        // Materialise the L2 geometry: every DWGLineGroup::execute() cuts its
        // raw shape with the linked linetype pattern into world-unit ink
        // segments.  DWGLineType objects were created above with no links, so
        // their Pattern.setValue() did not recompute anything.
        Base::Console().log(
            "[SWDWG] line-group materialise: %d group(s), %zu shape(s)\n",
            nGroups, nShapes);

        // Materialise every collected 3DFACE bucket as ONE DWGSurfaceMesh
        // object (shared vertices + flat 4-per-quad index list), so terrain
        // face counts do not scale per-face OCC/Coin work.  Built after all
        // entities are expanded so the deduplication map is reserved once.
        if (!faceQuads.empty()) {
            int surfIdx = 0;
            std::size_t surfFaces = 0;
            for (auto& kv : faceQuads) {
                const std::string& layer = kv.first.layer;
                std::vector<std::array<Vec3, 4>>& quads = kv.second;
                if (quads.empty()) {
                    continue;
                }
                std::vector<Base::Vector3d> pts;
                std::vector<long> idxList;
                pts.reserve(quads.size());
                idxList.reserve(quads.size() * 4);
                std::unordered_map<FacePtKey, int, FacePtKeyHash> seen;
                seen.reserve(quads.size());
                for (const auto& q : quads) {
                    for (const Vec3& p : q) {
                        FacePtKey k;
                        k.x = p.x;
                        k.y = p.y;
                        k.z = p.z;
                        auto it = seen.find(k);
                        int vi;
                        if (it == seen.end()) {
                            vi = static_cast<int>(pts.size());
                            seen.emplace(k, vi);
                            pts.emplace_back(p.x, p.y, p.z);
                        }
                        else {
                            vi = it->second;
                        }
                        idxList.push_back(vi);
                    }
                }
                ++surfIdx;
                surfFaces += quads.size();
                auto* feat = doc->addObject<SWDWG::DWGSurfaceMesh>(
                    ("Surface_" + std::to_string(surfIdx)).c_str());
                if (!feat) {
                    continue;
                }
                feat->Label.setValue(("Surface - " + layer).c_str());
                feat->Layer.setValue(layer.c_str());
                feat->EntityKind.setValue("SURFACEMESH");
                feat->MeshColor.setValue(Base::Color(
                    ((kv.first.color >> 16) & 0xFF) / 255.0f,
                    ((kv.first.color >> 8) & 0xFF) / 255.0f,
                    (kv.first.color & 0xFF) / 255.0f));
                feat->Points.setValues(pts);
                feat->FaceIndex.setValues(idxList);
                SWDWG_LOG("SWDWG: surface '%s': %zu quad(s), %zu "
                          "vertex(es)\n",
                          feat->Label.getValue(), quads.size(), pts.size());
            }
            faceQuads.clear();
            Base::Console().log(
                "[SWDWG] surface materialise: %d surface(s), %zu face(s)\n",
                surfIdx, surfFaces);
        }

        // EXPERIMENT (revertible): run-internal recompute disabled so the one
        // at the end of importDWG (after organise, outside the Importing lock)
        // does the work instead.  Measures whether the ~23 s belongs to the
        // recompute itself (would just move) or to its position under the lock.
        (void)doc;
        return true;
    }

private:
    //! Estimate rendered width of a single-line text string in world units.
    static double estimateTextWidth(const std::string& text, double fontHeight);

    //! Compute the per-line local offsets (left-baseline origin relative to
    //! the anchor, in the un-rotated text plane, y up).  For TEXT there is
    //! exactly one line; for MTEXT each rendered line gets an offset.  Also
    //! applies the Aligned(3)/Fit(5) two-point rotation and, for Fit, the
    //! height correction.  \a fontHeight / \a rotation are in/out.
    void computeTextLayout(const Entity& e, double& fontHeight,
                           double& rotation,
                           const std::vector<std::string>& lines,
                           std::vector<Vec3>& lineOffsets,
                           const Vec3& anchor, const Vec3& alignPt2W,
                           bool hasAlignPt2, double wfMul = 1.0) const;

    //! True if the given DWG layer is off or frozen, i.e. its geometry must
    //! not be displayed.  Unknown layers are treated as visible.
    bool layerHidden(const std::string& layer) const
    {
        std::string key = layer.empty() ? "0" : layer;
        auto it = layerStyles.find(key);
        return it != layerStyles.end()
            && (it->second.off || it->second.frozen);
    }

    //! Resolve the display style of an entity: color (true color / ACI /
    //! ByLayer), linetype and line weight (explicit / ByLayer).  ByBlock
    //! attributes fall back to the layer style, which is the usual
    //! appearance of block content drawn with the current layer.
    StyleKey makeKey(const Entity& e) const
    {
        StyleKey k;
        k.layer = e.layer.empty() ? "0" : e.layer;
        auto it = layerStyles.find(k.layer);
        const LayerStyle* ls = it != layerStyles.end() ? &it->second : nullptr;

        // Color.  DWGExtract.cpp now resolves the method field, so a non-zero
        // rgb is always an explicit truecolor and a valid index is an ACI.
        if (e.rgb) {
            k.color = e.rgb & 0xFFFFFF;
        }
        else if (e.colorIndex > 0 && e.colorIndex != 256) {
            k.color = aciRgb(e.colorIndex);
        }
        else {
            k.color = ls ? (ls->rgb ? (ls->rgb & 0xFFFFFF)
                                    : aciRgb(ls->colorIndex))
                         : 0xFFFFFF;
        }

        // Linetype.
        k.ltype = e.ltype;
        if (k.ltype.empty() || k.ltype == "ByLayer" || k.ltype == "ByBlock"
            || k.ltype == "BYLAYER" || k.ltype == "BYBLOCK") {
            k.ltype = ls ? ls->ltype : "Continuous";
        }
        if (k.ltype.empty()) {
            k.ltype = "Continuous";
        }

        // Line weight (1/100 mm).
        k.linewt = e.lineWeight;
        if (k.linewt <= 0) {
            k.linewt = ls ? ls->linewt : 25;
        }
        if (k.linewt <= 0) {
            k.linewt = 25;
        }

        // Linetype scale: entity override (DXF 48) wins, then the global
        // CELTSCALE header variable, then 1.0.
        //
        // The unit factor is NOT applied here, and that is deliberate: the
        // group's DWGLineScale is ltscale * k.ltypeScale, and ltscale already
        // carries the factor (run() scales the raw $LTSCALE) - applying it
        // again here squared it.  Measured on the DXT survey (a metres drawing,
        // so unitScale = 1000, raw LTSCALE = CELTSCALE = 1): every group came
        // out with DWGLineScale = 1e6, which makes one dash longer than the
        // whole drawing, so every dashed linetype rendered solid - reported as
        // "this polyline's linetype is not restored" (handle f9ac, layer
        // 透明图层, linetype '1161').  Millimetre drawings hid it because
        // 1 x 1 = 1.
        k.ltypeScale = e.ltypeScale > 0.0
            ? e.ltypeScale
            : ((celtscale > 0.0) ? celtscale : 1.0);
        if (k.ltypeScale <= 0.0) {
            k.ltypeScale = 1.0;
        }
        return k;
    }

    //! Entity as it goes into buildShape().  A LEADER that carries no arrow
    //! size of its own takes it from the DIMSTYLE it references: DIMASZ x
    //! DIMSCALE, exactly the way createDimensionObject() resolves a
    //! dimension's arrows (the tested file stores 0 on the entity itself).
    //! The unit factor is deliberately not applied here - it lives in the
    //! parent matrix, which buildShape already folds into every vertex.
    Entity buildInput(const Entity& e) const
    {
        if (e.type != EntType::LEADER || e.leaderArrowSize > 1e-9
            || e.dimStyleName.empty()) {
            return e;
        }
        auto it = dimStyles.find(e.dimStyleName);
        if (it == dimStyles.end()) {
            return e;
        }
        Entity out = e;
        const DimStyle& st = it->second;
        out.leaderArrowSize = st.arrowSize * (st.scale > 0.0 ? st.scale : 1.0);
        return out;
    }

    void expand(App::Document* doc, const Entity& e, const Mat4& parent,
                int depth, std::map<std::string, int>& counters, int& created,
                int& skipped)
    {
        // Entities on a hidden (off/frozen) layer are not displayed.
        if (layerHidden(e.layer)) {
            ++skipped;
            return;
        }
        if (e.type == EntType::INSERT) {
            ImpStopwatch _ins(g_insertSec);
            if (depth >= MAX_BLOCK_DEPTH) {
                ++skipped;
                return;
            }
            // Composition order matters: the convention here is the row-vector
            // one (p' = p * M, see DWGTypes.h), so the LEFT factor is applied
            // first.  The parent carries the root unit scale, and that has to
            // be applied LAST - after the insertion point has been added.
            // matMul(parent, insert) instead scales the block's own
            // coordinates and then adds the *unscaled* insertion point, which
            // puts two different scales inside one layer group.
            Mat4 M = matMul(
                matMul(insertMatrix(e.insPt, e.scale, e.rotation),
                       ocsToWorld(e.extrusion)),
                parent);
            // Geometry: reuse the block's cached shape through a shared OCC
            // location (zero-copy for rigid transforms).
            TopoDS_Shape bs = getBlockShape(e.blockName, depth + 1);
            if (bs.IsNull()) {
                ++skipped;  // unknown / empty / failed block
                ++failedInserts[e.blockName];
                return;
            }
            TopoDS_Shape s = transformShape(bs, M);
            if (s.IsNull()) {
                ++skipped;
                ++failedInserts[e.blockName];
                return;
            }
            // An INSERT whose world extent falls outside the region the
            // drawing's own model-space entities occupy is either a block the
            // file legitimately placed elsewhere on the sheet, or a transform
            // that went wrong.  Either way it is worth naming: unknown, it
            // reads as anonymous stray geometry far from the drawing (docs/32).
            {
                Bnd_Box box;
                BRepBndLib::Add(s, box);
                if (!box.IsVoid()) {
                    double x1 = 0.0, y1 = 0.0, z1 = 0.0, x2 = 0.0, y2 = 0.0,
                           z2 = 0.0;
                    box.Get(x1, y1, z1, x2, y2, z2);
                    const double mx = 0.05 * std::max(1.0, refX1 - refX0);
                    const double my = 0.05 * std::max(1.0, refY1 - refY0);
                    if (refValid
                        && (x2 < refX0 - mx || x1 > refX1 + mx || y2 < refY0 - my
                            || y1 > refY1 + my)) {
                        static int s_outside = 0;
                        if (++s_outside <= 10) {
                            Base::Console().warning(
                                "[SWDWG] INSERT '%s' layer='%s' lands outside "
                                "the drawing: ins=(%.1f, %.1f) rot=%.4g bbox %s "
                                "(model space is x=[%.0f,%.0f] y=[%.0f,%.0f])\n",
                                e.blockName.c_str(), e.layer.c_str(), e.insPt.x,
                                e.insPt.y, e.rotation, shapeBBox(s).c_str(),
                                refX0, refX1, refY0, refY1);
                        }
                    }
                }
            }
            // Block content keeps its own layers, colours and linetypes: place
            // each style run into its own bucket, resolved against this
            // reference (content on layer 0 and ByBlock attributes follow the
            // reference, ByLayer follows the run's own layer).  The whole-block
            // compound `s` above remains for the extent / origin diagnostics.
            const StyleKey insKey = makeKey(e);
            const std::vector<BlockPart>& parts =
                getBlockParts(e.blockName, depth + 1);
            if (parts.empty()) {
                layerShapes[insKey].push_back(s);
            }
            else {
                for (const BlockPart& p : parts) {
                    TopoDS_Shape ps = transformShape(p.shape, M);
                    if (ps.IsNull()) {
                        continue;
                    }
                    layerShapes[resolvePart(p, insKey)].push_back(std::move(ps));
                }
            }
            // Text cannot live inside a compound shape; emit annotations.
            createBlockTexts(e, M, depth + 1, counters, created, skipped);
            ++created;
            ++createdCount[(int)e.type];
            checkOriginShape(
                (std::string("INSERT '") + e.blockName + "'").c_str(),
                e.layer, s, &e.insPt, &e.scale, e.rotation);
            // if (insDiagPrinted < 30) {
            //     ++insDiagPrinted;
            //     SWDWG_LOG(
            //         "SWDWG:   INSERT '%s' @(%.3f, %.3f, %.3f) "
            //         "scale=(%.3f, %.3f, %.3f) rot=%.3f deg -> bbox %s\n",
            //         e.blockName.c_str(), e.insPt.x, e.insPt.y, e.insPt.z,
            //         e.scale.x, e.scale.y, e.scale.z,
            //         e.rotation * 180.0 / PI, shapeBBox(s).c_str());
            // }
            return;
        }

        if (e.type == EntType::TEXT || e.type == EntType::MTEXT) {
            createAnnotation(doc, e, parent, counters, created, skipped);
            return;
        }

        if (e.type == EntType::DIMENSION) {
            // Dimension geometry + text lives in its own object (see
            // DWGDimension.cpp); it is not part of the layer compound.
            // Resolve ByLayer / ByBlock colour fallback before handing the
            // entity to the dimension builder, because DIMSTYLE colours may
            // inherit from the entity / layer colour.
            Entity dimE = e;
            if ((e.colorIndex == 0 || e.colorIndex == 256) && e.rgb == 0) {
                auto it = layerStyles.find(e.layer.empty() ? "0" : e.layer);
                if (it != layerStyles.end()) {
                    dimE.colorIndex = it->second.colorIndex;
                    dimE.rgb = it->second.rgb;
                }
            }
            // Count the dimension in createdCount too: createDimensionObject
            // only bumps the grand total, so the per-type report read
            // "Dimension=113(0/0)" for a drawing where all 113 were built -
            // verified in the document (129 DWGDimension objects for the
            // bridge plan, 295 for the road plan, both matching the parsed
            // counts).  A delta keeps a genuinely skipped one out of the count.
            const int dimBefore = created;
            createDimensionObject(doc, dimE, parent, dimStyles, textStyles,
                                  dwgCodepage, counters, created, skipped);
            if (created != dimBefore) {
                ++createdCount[(int)e.type];
            }
            return;
        }

        if (e.type == EntType::FACE3D && e.pts.size() >= 4) {
            // Surface fast path: collect the corners instead of building an
            // OCC face per entity; they become one DWGSurfaceMesh per style
            // bucket in materialiseFaces() below.
            StyleKey k = makeKey(e);
            FaceKey fk;
            fk.layer = k.layer.empty() ? "0" : k.layer;
            fk.color = k.color;
            std::array<Vec3, 4>& c = faceQuads[fk].emplace_back();
            for (int i = 0; i < 4; ++i) {
                c[i] = xform(e.pts[i], parent);
            }
            ++created;
            ++createdCount[(int)e.type];
            return;
        }

        ImpStopwatch _geom(g_geomBuildSec);
        TopoDS_Shape shape;
        try {
            shape = buildShape(buildInput(e), parent);
        }
        catch (const Standard_Failure&) {
            shape.Nullify();
        }
        if (shape.IsNull()) {
            ++skipped;
            ++failedCount[(int)e.type];
            return;
        }
        reportOversized("entity",
                        e.blockName.empty() ? std::string("<model space>")
                                            : e.blockName,
                        e.type, e.layer, shape, true, robSpan);
        layerShapes[makeKey(e)].push_back(shape);
        ++created;
        ++createdCount[(int)e.type];
        checkOriginShape(typeLabel(e.type), e.layer, shape);
    }

    //! Build (once) the geometry of a block definition in block-local
    //! coordinates and cache it.  Nested INSERTs are applied inside the
    //! block.  Text entities are excluded here; they are emitted separately
    //! by createBlockTexts.  A null shape is cached for unknown/empty/failed
    //! blocks so they are not re-built for every INSERT.
    //! Unresolved style of a block child entity (see BlockPart).
    static BlockPart partOf(const Entity& e)
    {
        BlockPart p;
        p.layer = (e.layer.empty() || e.layer == "0") ? std::string() : e.layer;
        p.colorIndex = e.colorIndex;
        p.rgb = e.rgb;
        p.ltype = e.ltype;
        p.ltypeScale = (e.ltypeScale > 0.0) ? e.ltypeScale : 1.0;
        return p;
    }

    //! Fold a nested reference's own attributes into a part of its definition.
    //! ByBlock in the definition means "follow the reference that places it", so
    //! such a part takes the reference's own value - which may itself be ByLayer
    //! or ByBlock and is resolved the same way one level further up.
    void foldPart(BlockPart& p, const Entity& ref) const
    {
        if (p.layer.empty() && !(ref.layer.empty() || ref.layer == "0")) {
            p.layer = ref.layer;  // layer 0 content shows on the reference layer
        }
        if (p.colorIndex == 0 && p.rgb == 0) {  // ByBlock
            p.colorIndex = ref.colorIndex;
            p.rgb = ref.rgb;
        }
        // An empty linetype means ByLayer as often as the literal string does,
        // so only inherit the reference when the part has no layer of its own
        // (content on layer 0) or says ByBlock explicitly.
        if (p.ltype == "ByBlock" || p.ltype == "BYBLOCK"
            || (p.ltype.empty() && p.layer.empty())) {
            p.ltype = ref.ltype;
        }
        // A colour that is still ByLayer here belongs to the nested reference's
        // own layer, not to the layer of the reference above it.
        if (p.colorIndex == 256 && p.rgb == 0 && !p.layer.empty()) {
            auto ls = layerStyles.find(p.layer);
            if (ls != layerStyles.end()) {
                if (ls->second.rgb) {
                    p.rgb = ls->second.rgb & 0xFFFFFF;
                }
                else if (ls->second.colorIndex > 0) {
                    p.colorIndex = ls->second.colorIndex;
                }
            }
        }
    }

    //! Resolve a part's style against the reference that places its block.
    StyleKey resolvePart(const BlockPart& p, const StyleKey& ins) const
    {
        StyleKey k = ins;
        if (!p.layer.empty()) {
            k.layer = p.layer;
        }
        const auto ls = layerStyles.find(k.layer);
        const LayerStyle* l = ls != layerStyles.end() ? &ls->second : nullptr;
        if (p.rgb) {
            k.color = p.rgb & 0xFFFFFF;
        }
        else if (p.colorIndex == 0) {
            k.color = ins.color;  // ByBlock still unresolved: the reference's
        }
        else if (p.colorIndex == 256) {  // ByLayer
            k.color = l ? (l->rgb ? (l->rgb & 0xFFFFFF) : aciRgb(l->colorIndex))
                        : ins.color;
        }
        else if (p.colorIndex > 0) {
            k.color = aciRgb(p.colorIndex);
        }
        std::string lt = p.ltype;
        if (lt.empty() || lt == "ByBlock" || lt == "BYBLOCK") {
            // Empty means ByLayer for content that has a layer of its own (the
            // reader leaves the field empty when the entity stores the default);
            // only layer 0 content follows the reference.  Measured on the
            // bridge plan: the pile content on layer 虚线 must come out HIDDEN2
            // (dashed) like AutoCAD draws it, not the sheet reference's
            // Continuous.
            lt = p.layer.empty() ? ins.ltype : "ByLayer";
        }
        if (lt == "ByLayer" || lt == "BYLAYER") {
            lt = (l && !l->ltype.empty()) ? l->ltype : ins.ltype;
        }
        k.ltype = lt.empty() ? "Continuous" : lt;
        // Effective linetype scale: LTSCALE x CELTSCALE (already in ins) times
        // the reference chain above this run and the run's own DXF 48.  The dash
        // length is the definition times this, so nothing is hardcoded.
        k.ltypeScale = ins.ltypeScale * p.ltypeScale;
        return k;
    }

    //! Build (once) the style runs of a block definition in block-local
    //! coordinates and cache them.  Text and dimensions are excluded - they are
    //! emitted by createBlockTexts / createDimensionObject - and nested INSERTs
    //! are applied inside the block, their own attributes folded into the parts
    //! they contribute.  An empty result is cached for unknown/empty/failed
    //! blocks so they are not re-built for every reference.
    const std::vector<BlockPart>& getBlockParts(const std::string& name,
                                               int depth)
    {
        auto it = blockParts.find(name);
        if (it != blockParts.end()) {
            return it->second;
        }
        std::vector<BlockPart> parts;
        if (depth < MAX_BLOCK_DEPTH) {
            auto def = blockDefs.find(name);
            if (def != blockDefs.end()) {
                Mat4 id = matIdentity();
                for (const Entity& child : def->second) {
                    if (child.type == EntType::TEXT
                        || child.type == EntType::MTEXT
                        || child.type == EntType::DIMENSION) {
                        continue;
                    }
                    if (layerHidden(child.layer)) {
                        continue;  // hidden layer geometry is not displayed
                    }
                    if (child.type == EntType::INSERT) {
                        const std::vector<BlockPart>& inner =
                            getBlockParts(child.blockName, depth + 1);
                        if (inner.empty()) {
                            continue;
                        }
                        // The nested reference's insertion point and rotation
                        // live in its own OCS, exactly like a top-level one (see
                        // the identity of the two paths in expand()).
                        Mat4 M = matMul(
                            insertMatrix(child.insPt, child.scale,
                                         child.rotation),
                            ocsToWorld(child.extrusion));
                        for (const BlockPart& ip : inner) {
                            BlockPart p = ip;
                            p.shape = transformShape(ip.shape, M);
                            if (p.shape.IsNull()) {
                                continue;
                            }
                            // The nested reference's own linetype scale is part
                            // of the chain, exactly like LTSCALE (measured on the
                            // bridge plan: the exploded content reports 8, which
                            // lives on a reference, not in the LINETYPE table).
                            p.ltypeScale =
                                ip.ltypeScale
                                * ((child.ltypeScale > 0.0) ? child.ltypeScale
                                                            : 1.0);
                            foldPart(p, child);
                            parts.push_back(std::move(p));
                        }
                        continue;
                    }
                    TopoDS_Shape s;
                    try {
                        s = buildShape(buildInput(child), id);
                    }
                    catch (const Standard_Failure&) {
                        s.Nullify();
                    }
                    if (s.IsNull()) {
                        continue;
                    }
                    reportOversized("child", name, child.type, child.layer, s,
                                    false);
                    BlockPart p = partOf(child);
                    p.shape = std::move(s);
                    parts.push_back(std::move(p));
                }
            }
        }
        return blockParts.emplace(name, std::move(parts)).first->second;
    }

    //! The whole block as one compound, for the extent / origin diagnostics.
    TopoDS_Shape getBlockShape(const std::string& name, int depth)
    {
        auto it = blockShapes.find(name);
        if (it != blockShapes.end()) {
            return it->second;
        }
        TopoDS_Shape result;
        const std::vector<BlockPart>& parts = getBlockParts(name, depth);
        if (!parts.empty()) {
            BRep_Builder B;
            TopoDS_Compound comp;
            B.MakeCompound(comp);
            bool added = false;
            for (const BlockPart& p : parts) {
                if (!p.shape.IsNull()) {
                    B.Add(comp, p.shape);
                    added = true;
                }
            }
            if (added) {
                result = comp;
            }
        }
        blockShapes[name] = result;
        return result;
    }

    //! Apply transform M to a shape.  Rigid transforms (translation,
    //! rotation, uniform scaling) reuse the underlying TShape through a
    //! TopLoc_Location so that a block inserted hundreds of times only costs
    //! one shared geometry.  Non-uniform scaling / mirroring performs a real
    //! (copied) geometric transform instead.
    static TopoDS_Shape transformShape(const TopoDS_Shape& shape,
                                       const Mat4& M)
    {
        if (shape.IsNull()) {
            return shape;
        }
        Vec3 c0{M.m[0][0], M.m[0][1], M.m[0][2]};
        Vec3 c1{M.m[1][0], M.m[1][1], M.m[1][2]};
        Vec3 c2{M.m[2][0], M.m[2][1], M.m[2][2]};
        double l0 = norm(c0);
        double l1 = norm(c1);
        double l2 = norm(c2);
        bool rigid =
            (std::fabs(l0 - l1) < 1e-9 && std::fabs(l1 - l2) < 1e-9);
        if (rigid) {
            if (std::fabs(dot(c0, c1)) > 1e-6 * l0 * l1
                || std::fabs(dot(c0, c2)) > 1e-6 * l0 * l2
                || std::fabs(dot(c1, c2)) > 1e-6 * l1 * l2) {
                rigid = false;  // sheared
            }
        }
        if (rigid) {
            double det = dot(c0, cross(c1, c2));
            if (det < 0.0) {
                rigid = false;  // mirrored -> gp_Trsf cannot represent it
            }
        }
        // Diagnostics (commented out): transform matrix + rigid classification.
        // {
        //     static int s_transDiag = 0;
        //     static int s_rigidDiag = 0;
        //     if (s_transDiag < 40 || (rigid && s_rigidDiag < 20)) {
        //         ++s_transDiag;
        //         if (rigid) {
        //             ++s_rigidDiag;
        //         }
        //         SWDWG_LOG(
        //             "SWDWG:   TS[%d] rigid=%d in=%s\n"
        //             "SWDWG:      M=[[%.4g %.4g %.4g %.4g]\n"
        //             "SWDWG:         [%.4g %.4g %.4g %.4g]\n"
        //             "SWDWG:         [%.4g %.4g %.4g %.4g]\n"
        //             "SWDWG:         [%.4g %.4g %.4g %.4g]]\n",
        //             s_transDiag, rigid ? 1 : 0, shapeBBox(shape).c_str(),
        //             M.m[0][0], M.m[0][1], M.m[0][2], M.m[0][3],
        //             M.m[1][0], M.m[1][1], M.m[1][2], M.m[1][3],
        //             M.m[2][0], M.m[2][1], M.m[2][2], M.m[2][3],
        //             M.m[3][0], M.m[3][1], M.m[3][2], M.m[3][3]);
        //     }
        // }
        if (rigid && l0 > 1e-12) {
            try {
                // gp_Trsf::SetValues takes one row of the 4x4 matrix per
                // argument group: (a11 a12 a13 a14, a21 a22 a23 a24,
                // a31 a32 a33 a34), i.e. x' = a11*x + a12*y + a13*z + a14.
                // Our Mat4 is row-major, so the rows map directly; the
                // translation goes into column 4 (a14/a24/a34).
                // SetValues() itself normalizes the matrix by the cube root
                // of the determinant and orthogonalizes it, so we must NOT
                // pre-divide by l0 or call SetScaleFactor() afterwards.
                // gp_Trsf applies its matrix to a *column* vector
                // (p' = A p + t) whereas Mat4 is the row-vector form used
                // everywhere else in this module (p' = p M), so the linear part
                // must be transposed on the way in - exactly as the gp_GTrsf
                // branch below already does it.  Passing the rows through
                // unchanged turned every rigidly placed shape the opposite way:
                // for a pure rotation the transpose is its inverse, so a block
                // inserted at -13.5 degrees came out at +13.5 and, when its
                // content sits far from its base point, landed tens of
                // thousands of units from the sheet (docs/32).  It stayed
                // hidden for so long because a pure scale is diagonal and most
                // inserts carry no rotation at all.
                gp_Trsf t;
                t.SetValues(M.m[0][0], M.m[1][0], M.m[2][0], M.m[3][0],
                            M.m[0][1], M.m[1][1], M.m[2][1], M.m[3][1],
                            M.m[0][2], M.m[1][2], M.m[2][2], M.m[3][2]);
                // Moved(), not Located(): Located() *replaces* a shape's
                // location, Moved() composes it.  Block geometry is now carried
                // in per-style runs (see getBlockParts), and a run produced by a
                // nested reference already carries that reference's location -
                // replacing it here threw the nested transform away and dropped
                // the content back to block-local coordinates, i.e. next to the
                // origin (measured on the bridge plan: 1172 vertices within 5000
                // of the origin, and block 推荐方案0号台平面示意 visibly sitting
                // at the origin instead of at AutoCAD's 34184.28 / 47737.83).
                // For a shape without a location the two calls are identical, so
                // the rest of the importer is unaffected.
                return shape.Moved(TopLoc_Location(t));
            }
            catch (const Standard_Failure&) {
                // fall through to the copied transform
            }
        }
        try {
            gp_Mat mat(M.m[0][0],
                       M.m[1][0],
                       M.m[2][0],
                       M.m[0][1],
                       M.m[1][1],
                       M.m[2][1],
                       M.m[0][2],
                       M.m[1][2],
                       M.m[2][2]);
            gp_GTrsf gt(mat, gp_XYZ(M.m[3][0], M.m[3][1], M.m[3][2]));
            BRepBuilderAPI_GTransform tr(shape, gt, Standard_True);
            return tr.Shape();
        }
        catch (const Standard_Failure&) {
            return TopoDS_Shape();
        }
    }

    //! Create a DWGText object for a TEXT/MTEXT entity.  DWGText carries the
    //! full set of AutoCAD text attributes: anchor point (Position), height,
    //! rotation, style, width factor, oblique angle, mirror flags, alignment,
    //! line spacing and the per-line local offsets.  Its view provider
    //! renders every line with SoAsciiText at true world size.
    void createAnnotation(App::Document* doc, const Entity& e,
                          const Mat4& parent,
                          std::map<std::string, int>& counters, int& created,
                          int& skipped)
    {
        ImpStopwatch _ann(g_textCreateSec);
        std::string name = typeLabel(e.type);
        std::string utf8Text = toUtf8(e.text);
        std::string label = name + std::to_string(++counters[name]);
        if (!utf8Text.empty()) {
            label = std::string(e.type == EntType::TEXT ? "Text - " : "MText - ")
                + truncateUtf8(utf8Text, 60);
        }
        // Open the bulk window BEFORE addObject(): attaching the new object
        // already makes FreeCAD touch every property (alphabetically), which
        // would otherwise rebuild / re-layout the empty text once per property.
        // Deferring to the single recomputeLayoutOnce() at the end runs the
        // layout exactly once and triggers the one view-provider rebuild.
        DWGText::enterBulkCreate();
        auto* ann = doc->addObject<DWGText>(name.c_str());
        if (!ann) {
            DWGText::leaveBulkCreate();
            ++skipped;
            ++failedCount[(int)e.type];
            return;
        }

        // Persist the DWG layer + entity kind so the document tree can be
        // organised per layer / per type after the import (EntityKind also
        // distinguishes TEXT from MTEXT although both share the DWGText type).
        ann->Layer.setValue(e.layer.c_str());
        ann->EntityKind.setValue(
            (e.type == EntType::MTEXT) ? "MTEXT" : "TEXT");

        // The anchor (rotation centre) is the DWG insertion/alignment point
        // transformed into world space.  Alignment offsets never move it;
        // they are stored per line and applied inside the rotated frame.
        Vec3 pos = e.pts.empty() ? Vec3{} : e.pts[0];
        Vec3 anchor = xform(pos, parent);

        // Resolve the font height: entity height wins, then the STYLE fixed
        // height, then a sensible default.
        double fontHeight = e.textHeight > 0.0 ? e.textHeight : 2.5;
        if (e.textHeight <= 0.0) {
            auto it = textStyles.find(e.textAttrs.styleName);
            if (it != textStyles.end() && it->second.textSize > 0.0) {
                fontHeight = it->second.textSize;
            }
        }
        // Text height (and the STYLE fixed height) is a length in drawing
        // units.  The anchor point goes through the matrix chain, so the height
        // is the one thing that has to be scaled explicitly - and it must be
        // scaled by the WHOLE chain, not just the import's unit factor: a text
        // that lives inside an INSERT at scale 0.001 (a 1:1000 detail block)
        // carries its height in block-local units, and scaling it by the unit
        // factor alone renders it 1000x larger than the block it belongs to.
        // The dimension path has always done this (see DWGDimension.cpp).
        const double parentScale = uniformScaleOf(parent);
        fontHeight *= parentScale;
        const double wf = e.textAttrs.widthFactor > 0.0
            ? e.textAttrs.widthFactor : 1.0;
        const double obliqueDeg = e.textAttrs.oblique;
        const int generation = e.textAttrs.generation;
        // The placement rotates the whole text: its angle in world space is the
        // entity's own (block-local) angle plus the rotation the insert chain
        // applies.  That angle is the one taking the local +X axis to world
        // space, i.e. atan2(m[0][1], m[0][0]) in this row-vector layout
        // (insertMatrix: m[0][0] = cos*sx, m[0][1] = sin*sx).  Only the anchor
        // used to go through the matrix, so a text inside a block inserted at
        // 20 degrees stayed horizontal while the block's own geometry turned -
        // reported as "the text rotation direction is different".  For the
        // identity placement (model space, and every drawing in mm) this adds
        // exactly 0.  computeTextLayout() below overwrites the angle from the
        // already world-space baseline for Aligned/Fit text, so those keep the
        // angle they had.
        double rotation = e.rotation + std::atan2(parent.m[0][1], parent.m[0][0]);
        std::string fontName =
            mapTextFont(e.textAttrs.fontFile, e.textAttrs.bigFontFile,
                        utf8Text);

        // Aligned(3)/Fit(5): the second baseline point in world space.
        Vec3 alignPt2W;
        const bool hasAlignPt2 = e.textAttrs.hasAlignPt2;
        if (hasAlignPt2) {
            alignPt2W = xform(e.textAttrs.alignPt2, parent);
        }

        // MTEXT: expand the embedded formatting codes, extracting the
        // dominant font / height / width overrides.
        std::string fmtFont;
        double hMul = 1.0, wMul = 1.0;
        bool hAbs = false;
        std::vector<std::string> lines;
        if (e.type == EntType::MTEXT) {
            std::string plain =
                expandMtextCodes(utf8Text, fmtFont, hMul, hAbs, wMul);
            lines = splitLines(plain);
            if (hAbs && hMul > 0.0) {
                fontHeight = hMul * parentScale;  // \H absolute height wins
            }
            else if (hMul > 0.0 && hMul != 1.0) {
                fontHeight *= hMul;  // \H relative multiplier
            }
            if (!fmtFont.empty()) {
                fontName = fmtFont;  // \f embedded font wins
            }
        }
        else {
            lines = splitText(utf8Text);
        }

        // MTEXT with a layout width wraps like AutoCAD: the text is formatted
        // into rect_width instead of running on as one long line.  This is
        // what makes the road labels of this bridge plan stack - their box is
        // about one character wide (MTEXT 230省道, handle 7c78: rect 9.4663,
        // height 6.0), so they break one character per line.  Those labels
        // used to be forced "vertical" from the stored extents, which was the
        // reported MTEXT vertical parsing problem; wrapping reproduces the
        // same on-screen result with AutoCAD's real semantics (flow_dir stays
        // 5 = by style, and the attachment point keeps its meaning).
        if (e.type == EntType::MTEXT && e.textAttrs.mtextRectW > 0.0) {
            lines = wrapMtextLines(lines, e.textAttrs.mtextRectW, fontHeight,
                                   e.textAttrs.fontFile,
                                   e.textAttrs.bigFontFile, dwgCodepage,
                                   wf * wMul);
        }

        std::vector<Vec3> lineOffsets;
        // TEXT stores its final layout as plain properties and re-runs it once
        // through recomputeLayoutOnce() below, so the importer layout pass is
        // only needed when its result has to be baked into a property that the
        // runtime layout does NOT write back (Fit shrinks FontHeight to span
        // the two alignment points).  MTEXT keeps the importer-computed
        // per-line offsets because embedded \W overrides and vertical stacking
        // are not visible to recomputeLayout().
        const bool importLayout =
            (e.type == EntType::MTEXT)
            || (hasAlignPt2 && e.textHAlign == 5);  // Fit: bake scaled height
        if (importLayout) {
            computeTextLayout(e, fontHeight, rotation, lines, lineOffsets,
                              anchor, alignPt2W, hasAlignPt2,
                              (e.type == EntType::MTEXT) ? wMul : 1.0);
        }

        ann->Position.setValue(anchor.x, anchor.y, anchor.z);
        // ===================== 导出功能 BEGIN =====================
        // 原始插入点（DXF 10）：跟着块变换一起映射，导出时按左/基线原样写回。
        if (e.type == EntType::TEXT) {
            const Vec3 rawIns = xform(e.insPt, parent);
            ann->InsPt.setValue(Base::Vector3d(rawIns.x, rawIns.y, rawIns.z));
        }
        // ===================== 导出功能 END =======================
        ann->LabelText.setValue(lines);
        ann->Label.setValue(label.c_str());
        ann->FontHeight.setValue(fontHeight);
        ann->Rotation.setValue(rotation);
        ann->FontName.setValue(fontName.c_str());
        ann->StyleName.setValue(e.textAttrs.styleName.c_str());
        // Bake the MTEXT \W width override into the property, so the runtime
        // re-layout (WidthFactor) matches the importer offsets (wf * wMul) and
        // the rendered glyphs are actually scaled by the override.
        ann->WidthFactor.setValue(
            (float)(wf * (e.type == EntType::MTEXT ? wMul : 1.0)));
        ann->ObliqueAngle.setValue(obliqueDeg);
        ann->Mirror.setValue(generation);
        ann->HorAlign.setValue(e.textHAlign);
        ann->VertAlign.setValue(e.textVAlign);
        ann->Attachment.setValue(e.mtextAttachment);
        ann->LineSpacing.setValue((float)e.textAttrs.lsFactor);
        ann->LineSpacingStyle.setValue(e.textAttrs.lsStyle);
        ann->IsMText.setValue(e.type == EntType::MTEXT);
        ann->IsVertical.setValue(e.textAttrs.isVertical);
        ann->AlignPt2.setValue(alignPt2W.x, alignPt2W.y, alignPt2W.z);
        // The AlignPt2 set above already triggered DWGText::recomputeLayout,
        // which stored the final per-line offsets (and, for Aligned/Fit text,
        // the resolved rotation / font height).  Relying on that result for
        // TEXT guarantees the import-time layout exactly matches a later
        // re-layout (e.g. re-applying the alignment in the property editor),
        // so centred/right-aligned text cannot display off-centre.  It also
        // preserves the per-character offsets of vertical text.  MTEXT (both
        // normal and vertical) keeps the importer-computed offsets here, so
        // embedded \W width overrides are respected (recomputeLayout cannot
        // see wfMul) and the import-time display does not depend on the
        // recomputeLayout trigger order during object creation.
        if (e.type == EntType::MTEXT) {
            std::vector<Base::Vector3d> offs;
            offs.reserve(lineOffsets.size());
            for (const Vec3& v : lineOffsets) {
                offs.emplace_back(v.x, v.y, v.z);
            }
            ann->LabelOffsets.setValues(offs);
        }
        ann->FontFile.setValue(e.textAttrs.fontFile.c_str());
        ann->BigFontFile.setValue(e.textAttrs.bigFontFile.c_str());
        ann->Codepage.setValue(dwgCodepage);
        {
            // Per-text diagnostics can flood the console on a 10k-text
            // drawing; keep only the first few, then a single summary line.
            static int s_annLogs = 0;
            if (s_annLogs < 8) {
                ++s_annLogs;
                SWDWG_LOG(
                    "SWDWG IMP createAnnotation '%s' -> FontFile='%s' "
                    "BigFontFile='%s' (readback='%s' '%s')\n",
                    label.c_str(),
                    e.textAttrs.fontFile.c_str(),
                    e.textAttrs.bigFontFile.c_str(),
                    ann->FontFile.getValue(),
                    ann->BigFontFile.getValue());
            }
            else if (s_annLogs == 8) {
                ++s_annLogs;
                SWDWG_LOG("[SWDWG IMP] further createAnnotation logs "
                          "suppressed\n");
            }
        }
        // Text color, resolved the same way as for geometry.
        StyleKey k = makeKey(e);
        ann->TextColor.setValue(Base::Color(
            ((k.color >> 16) & 0xFF) / 255.0f,
            ((k.color >> 8) & 0xFF) / 255.0f,
            (k.color & 0xFF) / 255.0f));
        if (!e.layer.empty()) {
            App::Property* prop = ann->getPropertyByName("Layer");
            if (prop
                && prop->getTypeId().isDerivedFrom(
                    App::PropertyString::getClassTypeId())) {
                static_cast<App::PropertyString*>(prop)->setValue(e.layer);
            }
        }
        // All properties are set: close the bulk window.  TEXT runs the
        // layout exactly once (its per-line offsets depend on the alignment /
        // AlignPt2 that were written above); MTEXT keeps the importer-computed
        // offsets (which honour the embedded \W override baked into
        // WidthFactor) and only forces the deferred view-provider rebuild.
        DWGText::leaveBulkCreate();
        if (e.type == EntType::TEXT) {
            ann->recomputeLayoutOnce();
        }
        else {
            ann->LabelOffsets.touch();
        }
        ++created;
        ++createdCount[(int)e.type];
    }

    //! Recursively walk a block definition and emit Annotation objects for
    //! every TEXT/MTEXT inside it (including nested INSERTs), applying the
    //! accumulated transform to their position.
    void createBlockTexts(const Entity& ins, const Mat4& parent, int depth,
                          std::map<std::string, int>& counters, int& created,
                          int& skipped)
    {
        auto it = blockDefs.find(ins.blockName);
        if (it == blockDefs.end() || it->second.empty()) {
            return;
        }
        for (const Entity& child : it->second) {
            if (layerHidden(child.layer)) {
                continue;  // hidden layer text is not displayed
            }
            if (child.type == EntType::TEXT || child.type == EntType::MTEXT) {
                createAnnotation(doc, child, parent, counters, created, skipped);
            }
            else if (child.type == EntType::DIMENSION) {
                // Inside a block the entity colour may be ByBlock / ByLayer.
                // Fall back to the child entity's layer style so the DIMSTYLE
                // inheritance in resolveDimColor() has a concrete colour to use.
                Entity dimE = child;
                if ((child.colorIndex == 0 || child.colorIndex == 256)
                    && child.rgb == 0) {
                    auto it =
                        layerStyles.find(child.layer.empty() ? "0" : child.layer);
                    if (it != layerStyles.end()) {
                        dimE.colorIndex = it->second.colorIndex;
                        dimE.rgb = it->second.rgb;
                    }
                }
                const int dimBefore = created;
                createDimensionObject(doc, dimE, parent, dimStyles, textStyles,
                                      dwgCodepage, counters, created, skipped);
                if (created != dimBefore) {
                    ++createdCount[(int)child.type];
                }
            }
            else if (child.type == EntType::INSERT && depth < MAX_BLOCK_DEPTH) {
                // Same order as expand(): insert first, parent (root unit
                // scale) last.
                Mat4 M = matMul(
                    matMul(insertMatrix(child.insPt, child.scale,
                                        child.rotation),
                           ocsToWorld(child.extrusion)),
                    parent);
                createBlockTexts(child, M, depth + 1, counters, created, skipped);
            }
        }
    }

    static std::string truncateUtf8(const std::string& s, size_t maxBytes)
    {
        if (s.size() <= maxBytes) {
            return s;
        }
        std::string out = s.substr(0, maxBytes);
        // Avoid splitting a multi-byte UTF-8 sequence.
        while (!out.empty() && (out.back() & 0xC0) == 0x80) {
            out.pop_back();
        }
        return out;
    }

    //! Convert a raw DWG string to UTF-8.
    std::string toUtf8(const std::string& raw) const
    {
        return decodeDwgString(raw, dwgCodepage);
    }

    //! Expand AutoCAD MTEXT formatting codes into plain text.
    //!
    //! Line breaks (\\P) become '\\n'.  The first meaningful format overrides
    //! found in the text are reported back through \a fontOverride (\\f font
    //! name), \a heightMul / \a heightAbs (\\H: relative multiplier with 'x'
    //! suffix, or absolute height) and \a widthMul (\\W multiplier).  These
    //! let the importer apply the dominant embedded format on top of the
    //! text style defaults.
    static std::string expandMtextCodes(const std::string& text,
                                        std::string& fontOverride,
                                        double& heightMul, bool& heightAbs,
                                        double& widthMul)
    {
        std::string out;
        out.reserve(text.size() + 16);
        bool heightSet = false;
        bool widthSet = false;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size()) {
                char n = text[i + 1];
                if (n == 'P' || n == 'p') {
                    out += '\n';
                    ++i;
                    continue;
                }
                if (n == '~') {
                    out += ' ';
                    ++i;
                    continue;
                }
                if (n == '\\') {
                    out += '\\';
                    ++i;
                    continue;
                }
                if (n == '{' || n == '}') {
                    ++i;  // ignore grouping braces
                    continue;
                }
                if (n == 'f' || n == 'F') {
                    // \fFontName|b0|i0|c134;  -> font override
                    size_t end = text.find(';', i + 2);
                    if (end != std::string::npos) {
                        if (fontOverride.empty()) {
                            std::string fn =
                                text.substr(i + 2, end - (i + 2));
                            size_t bar = fn.find('|');
                            if (bar != std::string::npos) {
                                fn = fn.substr(0, bar);
                            }
                            // Strip a Windows path prefix, keep the family.
                            size_t slash = fn.find_last_of("/\\");
                            if (slash != std::string::npos) {
                                fn = fn.substr(slash + 1);
                            }
                            fontOverride = fn;
                        }
                        i = end;
                        continue;
                    }
                }
                if (n == 'H' || n == 'h') {
                    // \H2.5x; (relative) or \H2.5; (absolute)
                    size_t end = text.find(';', i + 2);
                    if (end != std::string::npos) {
                        if (!heightSet) {
                            std::string v =
                                text.substr(i + 2, end - (i + 2));
                            const bool rel =
                                !v.empty()
                                && (v.back() == 'x' || v.back() == 'X');
                            if (rel) {
                                v.pop_back();
                            }
                            char* stop = nullptr;
                            const double val = std::strtod(v.c_str(), &stop);
                            if (stop != v.c_str() && val > 0.0) {
                                heightMul = val;
                                heightAbs = !rel;
                            }
                            heightSet = true;
                        }
                        i = end;
                        continue;
                    }
                }
                if (n == 'W' || n == 'w') {
                    // \W0.8x; width multiplier
                    size_t end = text.find(';', i + 2);
                    if (end != std::string::npos) {
                        if (!widthSet) {
                            std::string v =
                                text.substr(i + 2, end - (i + 2));
                            const bool rel =
                                !v.empty()
                                && (v.back() == 'x' || v.back() == 'X');
                            if (rel) {
                                v.pop_back();
                            }
                            char* stop = nullptr;
                            const double val = std::strtod(v.c_str(), &stop);
                            if (stop != v.c_str() && val > 0.0) {
                                widthMul = val;
                            }
                            widthSet = true;
                        }
                        i = end;
                        continue;
                    }
                }
                // \Q...; \A...; \C...; \S...; toggles: consume the code.
                if (n == 'Q' || n == 'q' || n == 'A' || n == 'a' || n == 'C'
                    || n == 'c' || n == 'S' || n == 's') {
                    size_t end = text.find(';', i + 2);
                    if (end != std::string::npos) {
                        i = end;
                        continue;
                    }
                }
                if (n == 'L' || n == 'l' || n == 'O' || n == 'o' || n == 'K'
                    || n == 'k' || n == 'U' || n == 'u') {
                    ++i;  // underline / overline toggles
                    continue;
                }
            }
            else if (c == '%' && i + 2 < text.size() && text[i + 1] == '%') {
                char n = text[i + 2];
                if (n == 'C' || n == 'c') {
                    out += "\xE2\x8C\x80";  // U+2300 diameter sign
                    i += 2;
                    continue;
                }
                if (n == 'D' || n == 'd') {
                    out += "\xC2\xB0";  // U+00B0 degree sign
                    i += 2;
                    continue;
                }
                if (n == 'P' || n == 'p') {
                    out += "\xC2\xB1";  // U+00B1 plus-minus sign
                    i += 2;
                    continue;
                }
                if (n == '%') {
                    out += '%';
                    i += 2;
                    continue;
                }
            }
            out += c;
        }
        return out;
    }

    //! Split a plain-text string into lines on '\\n' / '\\r'.
    static std::vector<std::string> splitLines(const std::string& text)
    {
        if (text.empty()) {
            return {" "};
        }
        std::vector<std::string> lines;
        std::string cur;
        for (char c : text) {
            if (c == '\n' || c == '\r') {
                lines.push_back(cur);
                cur.clear();
            }
            else {
                cur += c;
            }
        }
        if (!cur.empty() || lines.empty()) {
            lines.push_back(cur);
        }
        if (lines.empty()) {
            lines.emplace_back(" ");
        }
        return lines;
    }

    std::vector<std::string> splitText(const std::string& text) const
    {
        std::string fo;
        double hm = 1.0;
        bool ha = false;
        double wm = 1.0;
        return splitLines(expandMtextCodes(text, fo, hm, ha, wm));
    }

    Dwg_Data* dwg;
    std::map<const Dwg_Object*, std::string> entBlock;
    std::map<std::string, std::vector<Entity>> blockDefs;
    std::vector<Entity> modelEnts;
    // Diagnostics: number of INSERT expansions already logged (commented out).
    // int insDiagPrinted = 0;
    // Target document, captured by run() for the annotation helpers.
    App::Document* doc = nullptr;
};

double Importer::estimateTextWidth(const std::string& text, double fontHeight)
{
    // Rough estimate: ASCII ~0.6 em, CJK/fullwidth ~1.0 em.
    size_t ascii = 0, wide = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        if ((c & 0x80) == 0) {
            extra = 0;
        }
        else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        }
        else {
            ++i;
            continue;
        }
        if (extra >= 2) {
            ++wide;
        }
        else {
            ++ascii;
        }
        i += extra + 1;
    }
    return fontHeight * (ascii * 0.6 + wide * 1.0);
}

//! Wrap expanded MTEXT lines into \a width (world units) the way AutoCAD
//! formats a multi-line text into its layout rectangle.
//!
//! The advance of each character is the width the renderer will actually use,
//! so the break positions follow the glyphs:
//!
//!  * .shx styles are stroked from the shape file, so their advance is read
//!    from the shape engine itself (renderShxLine().width).  The big-font
//!    cells are one em by convention but slightly less in practice, which is
//!    what decides borderline labels: MTEXT 科泰路 (h=4, rect_width 11.852,
//!    style FSDB) is ONE row in AutoCAD - three cells there measure 11.76, just
//!    inside the box, while a flat 1 em would have broken it into two rows.
//!
//!  * TrueType styles keep the documented estimate (fullwidth = one em, other
//!    characters about half an em), because the mapped family is measured
//!    through face->height and is not trustworthy for line breaking.  MTEXT
//!    230省道 (h=6, rect 9.4663, style 宋体 wf 0.75) wraps to three rows in
//!    AutoCAD: "230" (3 x 0.5 em = 9.0 fits), then 省 and 道 alone, as 省道
//!    would need 12.0.
//!
//! The width factor multiplies the drawn glyphs, not the break decision.
//!
//! A word is only split when it alone exceeds the box, and a single character
//! wider than the box still gets its own line, so the loop always progresses.
static std::vector<std::string> wrapMtextLines(
    const std::vector<std::string>& in, double width, double fontHeight,
    const std::string& fontFile, const std::string& bigFontFile, int codepage,
    double widthFactor)
{
    std::vector<std::string> out;
    if (width <= 0.0 || fontHeight <= 0.0) {
        return in;
    }
    const bool shx = isShxFont(fontFile, bigFontFile);
    const double wf = (widthFactor > 0.0) ? widthFactor : 1.0;
    auto advance = [&](const std::string& ch) {
        if (shx) {
            const ShxLineRender r = renderShxLine(fontFile, bigFontFile,
                                                  codepage, ch, fontHeight, wf);
            if (r.ok && r.width > 0.0) {
                return r.width;  // what the strokes really occupy
            }
        }
        return fontHeight * ((ch.size() >= 3) ? 1.0 : 0.5);
    };
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& c : v) {
            s += c;
        }
        return s;
    };
    for (const std::string& line : in) {
        std::vector<std::string> cur;
        double curW = 0.0;
        int lastSpace = -1;
        for (const std::string& c : splitUtf8Chars(line)) {
            const double a = advance(c);
            if (!cur.empty() && curW + a > width + 1e-9) {
                std::vector<std::string> rest;
                if (lastSpace > 0) {
                    // Break at the last space: the remainder runs on.
                    rest.assign(cur.begin() + lastSpace + 1, cur.end());
                    cur.resize(lastSpace);
                }
                out.push_back(join(cur));
                cur = rest;
                curW = 0.0;
                lastSpace = -1;
                for (std::size_t k = 0; k < cur.size(); ++k) {
                    curW += advance(cur[k]);
                    if (cur[k] == " ") {
                        lastSpace = static_cast<int>(k);
                    }
                }
            }
            if (c == " ") {
                lastSpace = static_cast<int>(cur.size());
            }
            cur.push_back(c);
            curW += a;
        }
        out.push_back(join(cur));
    }
    return out;
}

void Importer::computeTextLayout(const Entity& e, double& fontHeight,
                                 double& rotation,
                                 const std::vector<std::string>& lines,
                                 std::vector<Vec3>& lineOffsets,
                                 const Vec3& anchor, const Vec3& alignPt2W,
                                 bool hasAlignPt2, double wfMul) const
{
    lineOffsets.clear();
    const double wf = (e.textAttrs.widthFactor > 0.0
                           ? e.textAttrs.widthFactor : 1.0)
        * (wfMul > 0.0 ? wfMul : 1.0);

    // Precise advance-width measurement for one expanded text line.  The
    // STYLE font is mapped to a TrueType family and measured with FreeType
    // (width scales with the same width factor the glyphs are drawn with);
    // the rough ASCII/CJK estimate is only a fallback when the family cannot
    // be resolved (e.g. FreeType disabled).
    auto lineWidth = [&](const std::string& text) -> double {
        const std::string family = mapTextFont(
            e.textAttrs.fontFile, e.textAttrs.bigFontFile, text);
        const double w = measureTextWidth(family, text, fontHeight, wf);
        return w >= 0.0 ? w : estimateTextWidth(text, fontHeight) * wf;
    };

    if (e.type == EntType::TEXT) {
        // Single line.  The anchor stays at the DWG anchor point; the
        // alignment offsets shift the left baseline inside the rotated frame.
        // Use the expanded text (identical to what is stored in LabelText and
        // used by DWGText::recomputeLayout) for the width estimate, so the
        // import-time offsets exactly match the runtime layout.  Computing the
        // width from the raw DWG string (with embedded %% / \ codes) makes
        // centered text display off-centre until the alignment is re-applied.
        const std::string& textW = lines.empty() ? e.text : lines[0];
        if (hasAlignPt2) {
            // Aligned(3) / Fit(5): both points define the baseline direction
            // and extent; the first point is the anchor.
            Vec3 d = sub(alignPt2W, anchor);
            double len = norm(d);
            if (len > 1e-9) {
                rotation = std::atan2(d.y, d.x);
                // AutoCAD reverses the text when the second point lies left.
                if (d.x < 0.0) {
                    rotation += PI;
                }
                if (e.textHAlign == 5) {  // Fit: shrink/grow to fill span
                    double w = lineWidth(textW);
                    if (w > 1e-9) {
                        fontHeight = fontHeight * len / w;
                    }
                }
            }
            lineOffsets.emplace_back(0.0, 0.0, 0.0);
            return;
        }
        double w = lineWidth(textW);
        double dx = 0.0;
        double dy = 0.0;
        switch (e.textHAlign) {
            case 1:  // center
            case 4:  // middle is horizontally centered as well
                dx = -0.5 * w;
                break;
            case 2:  // right
                dx = -w;
                break;
            default:
                break;
        }
        switch (e.textVAlign) {
            case 1:  // bottom (descender line): baseline must sit above anchor
                dy = 0.25 * fontHeight;
                break;
            case 2:  // middle: anchor at bbox centre, baseline sits below it
                dy = -0.35 * fontHeight;
                break;
            case 3:  // top (cap height): baseline sits below anchor
                dy = -0.7 * fontHeight;
                break;
            default:
                break;
        }
        if (e.textHAlign == 4) {  // middle is vertically centered too
            dy = -0.35 * fontHeight;
        }
        lineOffsets.emplace_back(dx, dy, 0.0);
        return;
    }

    // MTEXT: per-line layout.  AutoCAD's default line spacing is 1.66 x the
    // text height ("3-on-5"), scaled by the linespace factor.  "At least"
    // clamps the factor at 1.0.  Vertical text stacks characters with the
    // plain text height as the step.
    const bool vertical = e.textAttrs.isVertical;
    std::vector<std::string> layoutLines = lines;
    if (vertical && !lines.empty()) {
        std::string all;
        for (const std::string& ln : lines) {
            all += ln;
        }
        layoutLines = splitUtf8Chars(all);
    }
    const size_t rows = std::max<size_t>(layoutLines.size(), 1);
    const double lsFactor = e.textAttrs.lsFactor > 0.0
        ? e.textAttrs.lsFactor : 1.0;
    // AutoCAD's "single" line spacing is 5/3 of the character height (its
    // "3-on-5" rule), which is what the file's own stored extents boxes are
    // built from see kSingleLineSpacing in DWGText.h.
    double ls = vertical ? fontHeight * lsFactor
                         : fontHeight * kSingleLineSpacing * lsFactor;
    // No "at least single spacing" clamp: AutoCAD reports this drawing's road
    // labels as the plain multiple and lays the rows out at exactly that pitch.
    // MTEXT 浒光运河 (h=4, line spacing 0.800000x, style FSDB) is listed as
    // "行距: 多重 (0.800000x = 5.3333)" and its own stored extents box is
    // 3 * 5.3333 + 4 = 20.000 - while the clamp forced 1.6667*h = 6.640, giving
    // a block of 23.9 units that ran past the leader arrow (19.83 units) in the
    // rendering, reported as the label overrunning the arrow.
    std::vector<double> widths(rows, 0.0);
    std::vector<double> tops(rows, fontHeight);
    std::vector<double> bots(rows, 0.0);
    for (size_t i = 0; i < rows; ++i) {
        widths[i] = lineWidth(layoutLines[i]);
    }
    // Metrics of the strokes the view provider will actually draw (see
    // DWGText::recomputeLayout, which keeps the same rule): a .shx style is
    // stroked from the shape file, and the mapped TrueType family measures
    // 1.5 em per fullwidth cell against the shape file's 1.0 em, which used to
    // shift every centred MTEXT by a quarter of its width.  Vertical text
    // keeps the font-based metrics.
    if (!vertical) {
        const std::string& ff = e.textAttrs.fontFile;
        const std::string& bf = e.textAttrs.bigFontFile;
        if (isShxFont(ff, bf)) {
            for (size_t i = 0; i < rows; ++i) {
                const ShxLineRender r = renderShxLine(
                    ff, bf, dwgCodepage, layoutLines[i], fontHeight, wf);
                if (r.ok && r.width > 0.0) {
                    widths[i] = r.width;
                    tops[i] = r.ascender;
                    bots[i] = r.descender;
                }
            }
        }
    }
    int ap = e.mtextAttachment;
    if (ap < 1 || ap > 9) {
        ap = 1;
    }
    const int col = (ap - 1) % 3;  // 0 left, 1 center, 2 right
    const int row = (ap - 1) / 3;  // 0 top, 1 middle, 2 bottom
    // Same placement as DWGText::recomputeLayout() (keep the two in step):
    // the block - from baseline_0 + tops[0] down to baseline_{rows-1} +
    // bots[rows-1] - is placed against the attachment-point anchor, so centre
    // / right / bottom attachments move the whole block.  See that function
    // for the reported case (MTEXT "道路中线", attachment 5, drawn half a text
    // width right of its anchor).
    const double span = (rows > 0) ? static_cast<double>(rows - 1) * ls : 0.0;
    const double top0 = tops.empty() ? fontHeight : tops[0];
    const double botN = bots.empty() ? 0.0 : bots[rows - 1];
    const double base0 = (row == 0) ? -top0
        : (row == 1) ? 0.5 * (span - top0 - botN)
        : span - botN;
    for (size_t i = 0; i < rows; ++i) {
        double x = 0.0;
        if (col == 1) {
            x = -0.5 * widths[i];
        }
        else if (col == 2) {
            x = -widths[i];
        }
        const double y = base0 - static_cast<double>(i) * ls;
        lineOffsets.emplace_back(x, y, 0.0);
    }
}

//! Build the "layer x kind" document tree for the objects one import created.
//!
//! Pure organisation: only lightweight container objects are added and nothing
//! touches the scene graph or the geometry, so rendering and view interaction
//! are unaffected.  The tree mirrors what every imported element already
//! stores on itself (EntityKind + Layer), so afterwards the FreeCAD model tree
//! lets you browse / pick objects by layer or by kind:
//!
//!   one top-level "DWG" App::Part wrapping the whole import
//!     one "定义" App::Part for shared DWGLineType definitions
//!       (kind Part "线型" holding every DWGLineType object)
//!     one "图层: <layer>" App::Part per DWG layer  (line groups, texts,
//!       dimensions)
//!       + one kind App::Part per EntityKind inside each layer Part
//!         (线/文字/多行文字/尺寸/网格面)
//!
//! Every container is an App::Part - the assembly-style container, no plain
//! Groups.  A Part carries a Placement, so any level of the tree can be
//! moved / rotated as a whole, and FreeCAD attaches an Origin (reference
//! planes / axes) to every Part automatically.  The imported geometry itself
//! is untouched: the group shapes have their location baked in, and every
//! Part's Placement stays identity, so nothing shifts on screen.
//!
//! \a before lists the objects that already existed before the import, so only
//! objects created by this import are organised.  Returns the number of layer
//! containers created.
int organizeDocumentTree(App::Document* doc,
                         const std::vector<App::DocumentObject*>& before)
{
    if (!doc) {
        return 0;
    }
    std::set<App::DocumentObject*> pre(before.begin(), before.end());
    std::vector<App::DocumentObject*> created;
    for (App::DocumentObject* o : doc->getObjects()) {
        if (o && pre.find(o) == pre.end()) {
            created.push_back(o);
        }
    }
    if (created.empty()) {
        return 0;
    }

    auto kindLabel = [](const std::string& kind) -> const char* {
        if (kind == "LINE") return "线";
        if (kind == "TEXT") return "文字";
        if (kind == "MTEXT") return "多行文字";
        if (kind == "DIMENSION") return "尺寸";
        if (kind == "LINETYPE") return "线型";
        if (kind == "SURFACEMESH") return "网格面";
        return nullptr;
    };
    auto getString = [](App::DocumentObject* o,
                        const char* propName) -> std::string {
        App::Property* p = o ? o->getPropertyByName(propName) : nullptr;
        if (p && p->isDerivedFrom(App::PropertyString::getClassTypeId())) {
            return static_cast<App::PropertyString*>(p)->getValue();
        }
        return std::string();
    };
    // Allocate an internal name that is still free in this document.
    auto freeName = [doc](const std::string& base) -> std::string {
        std::string cand = base;
        for (int n = 1; doc->getObject(cand.c_str()); ++n) {
            cand = base + "_" + std::to_string(n);
        }
        return cand;
    };
    // Container factory: EVERY container of the import tree is an App::Part
    // (assembly container) - no plain Groups anywhere.  FreeCAD attaches an
    // Origin (reference planes / axes) to every Part automatically; it is
    // kept, per the standard assembly workflow.  Every Part's Placement stays
    // identity, so the imported geometry (whose locations are baked into the
    // shapes) does not shift.
    auto makePart = [&](const std::string& base,
                        const char* label) -> App::Part* {
        App::Part* p = doc->addObject<App::Part>(freeName(base).c_str());
        if (p) {
            p->Label.setValue(label);
        }
        return p;
    };
    // Child Part created directly inside its parent Part.
    auto makeChildPart = [&](App::Part* parent,
                             const std::string& base,
                             const char* label) -> App::Part* {
        if (!parent) {
            return nullptr;
        }
        App::Part* p = doc->addObject<App::Part>(freeName(base).c_str());
        if (p) {
            p->Label.setValue(label);
            parent->addObject(p);
        }
        return p;
    };

    App::Part* defsRoot = nullptr;  // "定义"
    App::Part* defsKind = nullptr;  // "线型" under 定义
    int layerOrdinal = 0;

    // Top-level Part that holds every container this import builds.  Two
    // reasons:
    //   1. A single "DWG" entry point under the document root makes the
    //      imported tree self-contained - one dwg, one Part - so re-imports
    //      stay disjoint even when the user keeps the same document open.
    //   2. The "定义" + "图层: <name>" Parts are now CHILDREN of "DWG"
    //      instead of direct siblings under the document.  An import that
    //      produces no layer (e.g. only a DWGLineType) still creates the
    //      wrapper, so the document never shows a flat loose list at root.
    // The free objects - those with no EntityKind and that aren't LINETYPE -
    // stay at the document root on purpose: they aren't produced by the
    // SWDWG importer, so promoting them under "DWG" would silently swallow
    // pre-existing user objects in the active document.
    App::Part* dwgPart = makePart("SWDWG_Root", "DWG");
    if (!dwgPart) {
        return 0;
    }

    // Bucket every imported object by its (layer, kind) leaf group first, then
    // attach each leaf's whole list in ONE Group.setValues() call.  The old
    // per-object addObject() ran a separate property write + signal round trip
    // per child, which dominated the organise phase on multi-thousand-object
    // imports.
    std::vector<App::DocumentObject*> defsMembers;  // "定义" -> "线型"
    std::vector<std::string> layerOrder;            // first-use order
    std::map<std::string, std::size_t> layerIndex;  // layer -> layerOrder slot
    std::vector<std::map<std::string,
                         std::vector<App::DocumentObject*>>>
        kindMembers;  // parallel to layerOrder: kind -> members

    for (App::DocumentObject* o : created) {
        const std::string kind = getString(o, "EntityKind");
        if (kind.empty()) {
            continue;
        }
        const char* kLabel = kindLabel(kind);
        if (!kLabel) {
            continue;  // not one of the SWDWG kinds -> leave at document root
        }
        if (kind == "LINETYPE") {
            defsMembers.push_back(o);
            continue;
        }
        std::string layer = getString(o, "Layer");
        if (layer.empty()) {
            layer = "(no layer)";
        }
        auto it = layerIndex.find(layer);
        if (it == layerIndex.end()) {
            layerIndex.emplace(layer, layerOrder.size());
            layerOrder.push_back(layer);
            kindMembers.emplace_back();
        }
        kindMembers[layerIndex[layer]][kind].push_back(o);
    }

    auto attachAll = [](App::Part* p,
                        std::vector<App::DocumentObject*>& members) {
        if (!p || members.empty()) {
            return;
        }
        p->getExtensionByType<App::GroupExtension>()->addObjects(members);
        members.clear();
    };

    if (!defsMembers.empty()) {
        defsRoot = makeChildPart(dwgPart, "SWDWG_Definitions", "定义");
        defsKind = makeChildPart(defsRoot, "SWDWG_Kind_LINETYPE", "线型");
        attachAll(defsKind, defsMembers);
    }

    for (std::size_t li = 0; li < layerOrder.size(); ++li) {
        App::Part* root = makeChildPart(
            dwgPart, "SWDWG_Layer_" + std::to_string(layerOrdinal),
            (std::string("图层: ") + layerOrder[li]).c_str());
        ++layerOrdinal;
        for (auto& bk : kindMembers[li]) {
            const char* kLabel = kindLabel(bk.first);
            App::Part* kg =
                makeChildPart(root, "SWDWG_Kind_" + bk.first, kLabel);
            attachAll(kg, bk.second);
        }
    }

    SWDWG_LOG("SWDWG: organised %zu object(s) into %zu layer part(s)\n",
              created.size(), layerOrder.size());
    return static_cast<int>(layerOrder.size() + (defsRoot ? 1 : 0));
}

}  // namespace

// ---------------------------------------------------------------------------
//  Public entry point
// ---------------------------------------------------------------------------

bool DWGImporter::importDWG(App::Document* doc, const std::string& filename,
                            std::string& error, int& created, int& skipped,
                            ImportProgressFn progress)
{
    auto report = [&](double f, const std::string& phase) {
        if (progress) {
            progress(f, phase);
        }
    };
    // Identifies the build in every pasted log (message level: it shows in the
    // GUI report view, unlike the SWDWG_LOG diagnostics).
    Base::Console().message("[SWDWG] SWDWG %s importing '%s'\n", SWDWG_VERSION,
                            filename.c_str());
    created = 0;
    skipped = 0;
    s_fitPolyTried = 0;
    s_fitPolyQuadOk = 0;
    s_fitPolyCubicOk = 0;
    s_fitPolyNotDone = 0;
    s_fitPolyCurveNull = 0;
    s_fitPolyEdgeNull = 0;
    s_fitPolyException = 0;
    s_originShapes = 0;
    g_textCreateSec = 0.0;
    g_geomBuildSec = 0.0;
    g_insertSec = 0.0;
    g_groupSec = 0.0;
    g_runRecomputeSec = 0.0;

    // Coarse stage timers so a single import run shows where the seconds go
    // (read / parse / expand+create / organise / recompute).
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto tRead = t0;
    auto tParse = t0;
    auto tRun = t0;
    auto tOrganize = t0;
    auto tRecalc = t0;
    auto sec = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(a - b).count();
    };

    if (!doc) {
        error = "SWDWG: no target document";
        return false;
    }

    // Remember which objects already existed: the tree organiser below must
    // only touch the objects this import creates, never pre-existing content
    // when the user imports into an already populated document.
    std::vector<App::DocumentObject*> preExisting = doc->getObjects();

    Dwg_Data* dwg = new Dwg_Data;
    std::memset(dwg, 0, sizeof(Dwg_Data));

    report(0.02, "Reading DWG file...");
    int err = dwg_read_file(SWDWG::toLocalPath(filename).c_str(), dwg);
    if (!dwg->num_objects) {
        error = "dwg_read_file failed with error code " + std::to_string(err)
            + " (no objects could be read from the file)";
        dwg_free(dwg);
        report(1.0, "Import failed");
        return false;
    }
    tRead = Clock::now();

    Importer imp(dwg);
    bool ok = false;
    {
        // Bulk import: suppress the per-object signals / recomputes that a
        // 45k-object import would otherwise trigger.  It also makes the whole
        // import a single undo step.
        Base::ObjectStatusLocker<App::Document::Status, App::Document> lock(
            App::Document::Status::Importing, doc);
        report(0.20, "Parsing DWG tables and entities...");
        imp.parse();
        tParse = Clock::now();
        imp.m_progress = progress;  // run() reports 0.35..0.90 (entity counts)
        ok = imp.run(doc, created, skipped);
        tRun = Clock::now();
    }

    dwg_free(dwg);

    if (ok) {
        // Wrap the tree organisation in an Importing window as well: creating
        // the group containers and linking every imported object into them
        // would otherwise trigger one tree refresh per object.  The single
        // recompute below then materialises everything at once.
        {
            Base::ObjectStatusLocker<App::Document::Status, App::Document>
                lockTree(App::Document::Status::Importing, doc);
            report(0.93, "Organising layers...");
            organizeDocumentTree(doc, preExisting);
            tOrganize = Clock::now();
        }

        // View providers whose scene graph is built from geometry must do that
        // AFTER the importing lock is released: property writes made while the
        // Importing status was set are not pushed to the GUI.  This used to
        // happen only as a side effect of execute() writing Shape - a coupling
        // that silently broke the display whenever execute() found nothing to
        // change (the importer already stores the same geometry in Shape, so
        // execute() has nothing to write).  Make the trigger explicit:
        //  - surfaces rebuild their single SoIndexedFaceSet from FaceIndex,
        //  - line groups rebuild their SoIndexedLineSet from the geometry.
        for (App::DocumentObject* o : doc->getObjects()) {
            if (auto* s = dynamic_cast<SWDWG::DWGSurfaceMesh*>(o)) {
                if (s->FaceIndex.getValues().size() >= 4) {
                    s->FaceIndex.touch();
                }
            }
            else if (auto* g = dynamic_cast<SWDWG::DWGLineGroup*>(o)) {
                g->Shape.touch();
            }
        }

        report(0.95, "Recomputing document...");
        {
            // Only recompute the objects that actually need it.  The group
            // features display through the lightweight SWDWG view provider, so
            // their execute() (~1 s total) is the only real work left; passing
            // the mustRecompute subset keeps the exact same execution set as a
            // full doc->recompute() without walking the whole document.
            std::vector<App::DocumentObject*> need;
            for (App::DocumentObject* o : doc->getObjects()) {
                if (o->mustRecompute()) {
                    need.push_back(o);
                }
            }
            if (!need.empty()) {
                doc->recompute(need);
            }
        }
        tRecalc = Clock::now();

        Base::Console().log(
            "[SWDWG] importDWG stages: read=%.2fs parse=%.2fs "
            "expand/create=%.2fs organise=%.2fs recompute=%.2fs "
            "(import total=%.2fs, %d created)\n",
            sec(tRead, t0), sec(tParse, tRead), sec(tRun, tParse),
            sec(tOrganize, tRun), sec(tRecalc, tOrganize), sec(tRecalc, t0),
            created);

        // Where do the created objects come from?  Helps decide whether to
        // attack per-object cost or the object count itself.
        std::string byType;
        for (const auto& p : imp.createdCount) {
            byType += " " + std::string(typeLabel(static_cast<EntType>(p.first)))
                + "=" + std::to_string(p.second);
        }
        Base::Console().log("[SWDWG] created by type:%s\n", byType.c_str());
        Base::Console().log(
            "[SWDWG] expand detail: annot=%.2fs geom=%.2fs insert=%.2fs "
            "group=%.2fs (block texts are counted in both annot and insert)\n",
            g_textCreateSec, g_geomBuildSec, g_insertSec, g_groupSec);
        Base::Console().log(
            "[SWDWG]   group build=%.2fs runRecompute=%.2fs\n",
            g_groupSec - g_runRecomputeSec, g_runRecomputeSec);

        // Parsed -> created(failed) per entity type, to spot entities that
        // were read but then dropped (Face3D/SOLID are read; 3DSOLID and
        // other fixedtypes show up as unsupported instead).
        std::string parsedDiag;
        for (const auto& p : imp.parsedCount) {
            const int t = p.first;
            int c = 0;
            int f = 0;
            auto itc = imp.createdCount.find(t);
            if (itc != imp.createdCount.end()) {
                c = itc->second;
            }
            auto itf = imp.failedCount.find(t);
            if (itf != imp.failedCount.end()) {
                f = itf->second;
            }
            parsedDiag += " "
                + std::string(typeLabel(static_cast<EntType>(t))) + "="
                + std::to_string(p.second) + "(" + std::to_string(c) + "/"
                + std::to_string(f) + ")";
        }
        Base::Console().log("[SWDWG] parsed by type:%s\n",
                            parsedDiag.c_str());
        if (!imp.unsupportedCount.empty()) {
            std::string uns = "[SWDWG] unsupported fixedtypes:";
            for (const auto& p : imp.unsupportedCount) {
                // The code used to be printed with a "0x" prefix on the DECIMAL
                // value, so 498 (PROXY_ENTITY) showed up as "0x498" and read
                // like 0x498.
                char code[16];
                std::snprintf(code, sizeof(code), "0x%X", p.first);
                uns += " " + std::string(fixedTypeName(p.first)) + "(" + code
                    + ")=" + std::to_string(p.second);
            }
            Base::Console().log("%s\n", uns.c_str());
        }
        int layerCount = 0;
        for (const auto& kv : imp.layerShapes) {
            if (!kv.second.empty()) {
                ++layerCount;
            }
        }
        int annotationCount = imp.createdCount[(int)EntType::TEXT]
            + imp.createdCount[(int)EntType::MTEXT];
        int objectCount = layerCount + annotationCount;
        // for (const auto& f : imp.failedInserts) {
        //     SWDWG_LOG("SWDWG:   INSERT of block '%s' skipped %d "
        //                         "time(s) (no geometry)\n",
        //                         f.first.c_str(), f.second);
        // }
        // SWDWG_LOG("SWDWG: imported %d entity(ies) as %d object(s) "
        //                     "(%d layer group(s), %d annotation(s)) from '%s' "
        //                     "(%d skipped)\n",
        //                     created, objectCount, layerCount, annotationCount,
        //                     filename.c_str(), skipped);
        // Diagnostics (commented out): per-type parsed -> created counts.
        // std::string diag = "SWDWG: per-type:";
        // for (const auto& p : imp.parsedCount) {
        //     int t = p.first;
        //     int c = 0;
        //     int f = 0;
        //     auto itc = imp.createdCount.find(t);
        //     if (itc != imp.createdCount.end()) {
        //         c = itc->second;
        //     }
        //     auto itf = imp.failedCount.find(t);
        //     if (itf != imp.failedCount.end()) {
        //         f = itf->second;
        //     }
        //     diag += " ";
        //     diag += typeLabel(static_cast<EntType>(t));
        //     diag += "=" + std::to_string(p.second) + "->" + std::to_string(c)
        //         + "(" + std::to_string(f) + ")";
        // }
        // SWDWG_LOG("%s\n", diag.c_str());
        //
        // if (!imp.unsupportedCount.empty()) {
        //     std::string uns = "SWDWG: unsupported fixedtypes:";
        //     for (const auto& p : imp.unsupportedCount) {
        //         uns += " " + std::to_string(p.first) + "=" + std::to_string(p.second);
        //     }
        //     SWDWG_LOG("%s\n", uns.c_str());
        // }
        // SWDWG_LOG(
        //     "SWDWG: spline-fit polyline: tried=%d quadOk=%d cubicOk=%d "
        //     "notDone=%d curveNull=%d edgeNull=%d exception=%d\n",
        //     s_fitPolyTried,
        //     s_fitPolyQuadOk,
        //     s_fitPolyCubicOk,
        //     s_fitPolyNotDone,
        //     s_fitPolyCurveNull,
        //     s_fitPolyEdgeNull,
        //     s_fitPolyException);
        // SWDWG_LOG(
        //     "SWDWG: %d shape(s) landed near the origin (lost transform?)\n",
        //     s_originShapes);
    }
    else {
        error = "SWDWG: import failed";
        report(1.0, "Import failed");
    }
    if (ok) {
        report(1.0, "Done");
    }
    return ok;
}

}  // namespace SWDWG
