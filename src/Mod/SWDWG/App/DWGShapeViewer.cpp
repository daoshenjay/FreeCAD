//! SHX shape / font file visualiser - see DWGShapeViewer.h.
//!
//! Parses a .shx with the shared parser (DWGShapeFile) and lays every shape
//! out on a grid as OCCT edges inside Part::Feature objects.  Layout rules:
//!   - shapes are ordered by their number (for fonts that is the ASCII code,
//!     so the printable character table is shown in order);
//!   - 16 glyphs per row;
//!   - every shape is scaled to a common height (5 mm) and anchored with its
//!     y = 0 baseline / minimum onto the grid cell, so glyphs sit on a shared
//!     baseline exactly like in AutoCAD's shape editor;
//!   - files with many shapes (> 400) collapse into one feature to keep the
//!     document tree responsive.

#include "PreCompiled.h"

#include "DWGShapeViewer.h"

#include "DWGShapeFile.h"

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>
#include <Mod/Part/App/PartFeature.h>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace SWDWG
{

namespace
{

//! Axis-aligned bounds of a shape outline (empty when the shape has no segs).
struct ShapeBox
{
    double xmin = 0.0;
    double xmax = 0.0;
    double ymin = 0.0;
    double ymax = 0.0;
    bool valid = false;
};

ShapeBox boxOf(const ShapeDef& sd)
{
    ShapeBox b;
    for (const ShapeSeg& s : sd.segs) {
        if (!b.valid) {
            b.xmin = std::min(s.x0, s.x1);
            b.xmax = std::max(s.x0, s.x1);
            b.ymin = std::min(s.y0, s.y1);
            b.ymax = std::max(s.y0, s.y1);
            b.valid = true;
        }
        else {
            b.xmin = std::min(b.xmin, std::min(s.x0, s.x1));
            b.xmax = std::max(b.xmax, std::max(s.x0, s.x1));
            b.ymin = std::min(b.ymin, std::min(s.y0, s.y1));
            b.ymax = std::max(b.ymax, std::max(s.y0, s.y1));
        }
    }
    return b;
}

//! Append the outline of one shape into \a cmp, scaled by \a scale and
//! translated so the shape's y = 0 baseline lands at \a oy and its cell origin
//! at \a ox.  The shape's strokes (pen-up-separated draw runs) are each
//! closed into a wire and turned into a planar face so glyphs read as filled
//! characters instead of a tangled polyline.  Sub-shape (code 7) segments
//! kept only in \a sd.segs fall back to edge-only output so older / unusual
//! shapes still render.
//! \return true when at least one wire/edge was added.
bool buildGlyphCompound(const ShapeDef& sd, double ox, double oy,
                        double scale, TopoDS_Compound& cmp)
{
    const ShapeBox b = boxOf(sd);
    if (!b.valid) {
        return false;
    }
    bool added = false;
    BRep_Builder builder;

    // Render as outline only.
    //
    // 1) Every *straight* stroke becomes an OCCT edge.  Segments that are
    //    merely sampled chords of a circular arc are skipped - the exact arc
    //    below replaces them, so circles stay circular instead of being
    //    chopped into 70+ tiny line segments.
    // 2) Every ShapeArc becomes one real circular OCCT edge.
    //
    // Earlier revisions tried to close each stroke into a wire and make a
    // face, but for several glyphs the closing segment crosses the rest of
    // the outline (e.g. the bottom of an ellipse) and MakeFace either
    // rejects the self-intersecting wire or builds an unintended filled
    // polygon that occludes the actual character shape.  Edges show every
    // stroke faithfully with no occlusion, at the cost of no fill.  (Faces
    // can be turned back on once per-glyph face construction is reliable.)
    for (const ShapeSeg& s : sd.segs) {
        if (s.arcChord) {
            continue;  // represented by a ShapeArc below
        }
        const double dx = (s.x1 - s.x0) * scale;
        const double dy = (s.y1 - s.y0) * scale;
        if (dx * dx + dy * dy < 1e-12) {
            continue;  // OCCT cannot curve tiny segments
        }
        BRepBuilderAPI_MakeEdge me(
            gp_Pnt(ox + s.x0 * scale, oy + s.y0 * scale, 0.0),
            gp_Pnt(ox + s.x1 * scale, oy + s.y1 * scale, 0.0));
        if (!me.IsDone()) {
            continue;
        }
        builder.Add(cmp, me.Edge());
        added = true;
    }
    const double kPi = 3.14159265358979323846;
    for (const ShapeArc& a : sd.arcs) {
        const double rr = a.r * scale;
        if (rr < 1e-9) {
            continue;
        }
        const gp_Pnt centre(ox + a.cx * scale, oy + a.cy * scale, 0.0);
        const gp_Circ circ(gp_Ax2(centre, gp_Dir(0.0, 0.0, 1.0)), rr);
        BRepBuilderAPI_MakeEdge me = (a.sweepDeg >= 359.999)
            ? BRepBuilderAPI_MakeEdge(circ)  // whole circle
            : BRepBuilderAPI_MakeEdge(
                  circ, a.startDeg * kPi / 180.0,
                  (a.startDeg + a.sweepDeg) * kPi / 180.0);
        if (!me.IsDone()) {
            continue;
        }
        builder.Add(cmp, me.Edge());
        added = true;
    }
    return added;
}

//! Absolute coordinate beyond which a decoded outline is treated as corrupt.
//! Well-formed shape / font files keep their coordinates in a handful of
//! shape units (ltypeshp shapes span 1..56, font glyphs the 8-unit cell).
//! Broken decodes (misaligned stream from unsupported instructions, bad
//! arc/scale handling) produce astronomical coordinates (1e6 .. 1e11);
//! drawing them would blow the view up to kilometres, so they are skipped.
//! NOTE: this is deliberately *not* a size cap - symbol libraries such as
//! AAA.shx legitimately contain shapes whose native box spans thousands of
//! units (they are inserted scaled down by the caller).  isLargeShape()
//! below handles those by giving them their own per-cell scale.
bool outOfSaneRange(const ShapeBox& b)
{
    const double w = b.xmax - b.xmin;
    const double h = b.ymax - b.ymin;
    return std::fabs(b.xmax) > 1e5 || std::fabs(b.xmin) > 1e5
        || std::fabs(b.ymax) > 1e5 || std::fabs(b.ymin) > 1e5
        || w > 1e6 || h > 1e6;
}

//! A decoded shape whose native box is much larger than a normal glyph.
//! Valid large symbols are drawn into their own grid cell with a per-shape
//! scale instead of the shared font scale, so they neither vanish nor make
//! every sane neighbour tiny.
bool isLargeShape(const ShapeBox& b)
{
    const double w = b.xmax - b.xmin;
    const double h = b.ymax - b.ymin;
    return w > 2000.0 || h > 2000.0;
}

//! A short label for the tree view: ASCII printable glyph -> "A (#65)",
//! otherwise the numeric code and (if any) the internal shape name.
std::string glyphLabel(int number, const std::string& name)
{
    char buf[48];
    if (number >= 32 && number <= 126) {
        std::snprintf(buf, sizeof(buf), "%c (#%d)", (char)number, number);
        return buf;
    }
    if (!name.empty()) {
        std::snprintf(buf, sizeof(buf), "%s (#%d)", name.c_str(), number);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "(#%d)", number);
    return buf;
}

}  // namespace

bool viewShxFont(App::Document* doc, const std::string& path,
                 std::string& error)
{
    if (!doc) {
        error = "SWDWG: no target document";
        return false;
    }

    const ShapeFile sf = parseShapeFile(path);
    if (sf.empty()) {
        error = "无法解析 SHX 文件（缺失、损坏或格式不支持）: " + path;
        return false;
    }

    // Only shapes that actually draw something; the map is already sorted by
    // shape number (ASCII order for font files).  If nothing drew (e.g. a
    // big-font file whose entries are all stub subshape references), still
    // create a marker Feature so the user sees the file was loaded.
    std::vector<const ShapeDef*> glyphs;
    glyphs.reserve(sf.size());
    for (const auto& kv : sf) {
        if (!kv.second.segs.empty()) {
            glyphs.push_back(&kv.second);
        }
    }
    bool hasAnyEntry = !sf.empty();

    // Basename of the picked file for the labels / log.
    std::string base = path;
    const std::string::size_type slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }

    // Grid layout: 16 columns, every glyph scaled to a 5 mm display height.
    constexpr int kCols = 16;
    constexpr double kTarget = 5.0;   // mm
    const double kCellW = kTarget * 1.5;
    const double kCellH = kTarget * 1.8;

    const bool perGlyph = glyphs.size() <= 400;  // keep the tree readable

    // First pass: drop the corrupt glyphs and pick the largest non-large
    // vertical span.  Normal glyphs share that scale so widths stay
    // proportional and baselines line up; very large symbols (see
    // isLargeShape) get their own per-cell scale in the draw loop below.
    double maxSpanY = 0.0;
    int saneCount = 0;
    int largeCount = 0;
    for (const ShapeDef* sd : glyphs) {
        const ShapeBox b = boxOf(*sd);
        if (outOfSaneRange(b)) {
            continue;
        }
        ++saneCount;
        if (isLargeShape(b)) {
            ++largeCount;
            continue;  // excluded from the shared scale
        }
        const double spanY = b.ymax - b.ymin;
        if (spanY > maxSpanY) {
            maxSpanY = spanY;
        }
    }
    if (saneCount == 0) {
        error = "SHX 文件中没有可绘制的字形（解码后的字形均被识别为 big-font 扩展）";
        return false;
    }
    if (maxSpanY <= 1e-12) {
        maxSpanY = 10.0;  // only large symbols present: shared scale fallback
    }
    const double scale = kTarget / maxSpanY;
    // In the 8-unit SHX cell, the baseline (y = 0) sits 2 units below the
    // bottom of the cap height; we reserve that fraction of the target cell
    // so every glyph's baseline lands on the same horizontal line.
    const double kBaselinePad = kTarget * (2.0 / 7.0);

    int skippedSanity = 0;
    int skippedEmpty = 0;
    if (perGlyph) {
        // One Part::Feature per shape so individual glyphs can be selected /
        // hidden / inspected from the tree.  Shapes that end up with no real
        // edge (e.g. whitespace glyphs / control codes made of moves only)
        // keep their grid cell but do not create an empty feature.
        //
        // If no glyph has any visible outline (e.g. a big-font file whose
        // entries are stub subshape references), create a single marker
        // Feature so the user sees that the file was parsed.
        if (glyphs.empty()) {
            auto* feat = doc->addObject<Part::Feature>();
            if (!feat) {
                error = "SWDWG: 无法创建 Part::Feature（Part 模块未加载？）";
                return false;
            }
            feat->Label.setValue(("SHX: " + base).c_str());
        }
        else {
            int count = 0;
            for (const ShapeDef* sd : glyphs) {
                const int cell = count++;
                const int row = cell / kCols;
                const int col = cell % kCols;
                const double ox = col * kCellW;
                const double oy = -row * kCellH + kBaselinePad;

                const ShapeBox b = boxOf(*sd);
                if (outOfSaneRange(b)) {
                    ++skippedSanity;
                    Base::Console().warning(
                        "[SWDWG SHX] glyph (#%d) outline out of sane range "
                        "(%.4g x %.4g) - unsupported/corrupt codes? skipped\n",
                        sd->number, b.xmax - b.xmin, b.ymax - b.ymin);
                    continue;
                }
                // Very large but structurally valid symbols (AAA.shx etc.)
                // get their own scale so they fit one grid cell instead of
                // either vanishing under the shared glyph scale or blowing
                // the whole layout up.
                double glyScale = scale;
                double glyOy = oy;
                if (isLargeShape(b)) {
                    const double spanX = b.xmax - b.xmin;
                    const double spanY = b.ymax - b.ymin;
                    glyScale = std::min(
                        kTarget / std::max(spanY, 1e-9),
                        (kCellW * 0.9) / std::max(spanX, 1e-9));
                    glyOy = -row * kCellH + kTarget;  // top-align in the cell
                    SWDWG_LOG(
                        "[SWDWG SHX] glyph (#%d) large symbol %.0f x %.0f units"
                        " - drawn with its own scale %.5g\n",
                        sd->number, spanX, spanY, glyScale);
                }
                TopoDS_Compound cmp;
                BRep_Builder builder;
                builder.MakeCompound(cmp);
                const bool added =
                    buildGlyphCompound(*sd, ox, glyOy, glyScale, cmp);
                if (!added) {
                    // Reserve the cell (visual gap) but create no object.
                    ++skippedEmpty;
                    SWDWG_LOG(
                        "[SWDWG SHX] glyph (#%d) has no drawable outline - "
                        "skipped\n",
                        sd->number);
                    continue;
                }
                auto* feat = doc->addObject<Part::Feature>();
                if (!feat) {
                    error =
                        "SWDWG: 无法创建 Part::Feature（Part 模块未加载？）";
                    return false;
                }
                feat->Label.setValue(glyphLabel(sd->number, sd->name).c_str());
                feat->Shape.setValue(cmp);
            }
        }
    }
    else {
        // Very large files (symbol / big-font libraries): keep a single object.
        auto* feat = doc->addObject<Part::Feature>();
        if (!feat) {
            error = "SWDWG: 无法创建 Part::Feature（Part 模块未加载？）";
            return false;
        }
        feat->Label.setValue(("SHX: " + base).c_str());
        TopoDS_Compound cmp;
        BRep_Builder builder;
        builder.MakeCompound(cmp);
        int count = 0;
        for (const ShapeDef* sd : glyphs) {
            const ShapeBox b = boxOf(*sd);
            if (outOfSaneRange(b)) {
                ++skippedSanity;
                continue;
            }
            const int cell = count++;
            const int row = cell / kCols;
            const int col = cell % kCols;
            buildGlyphCompound(*sd, col * kCellW, -row * kCellH + kBaselinePad,
                               scale, cmp);
        }
        feat->Shape.setValue(cmp);
    }

    SWDWG_LOG(
        "[SWDWG SHX] file='%s' parsed=%zu drawn=%zu skipped(out-of-range=%d, "
        "empty=%d) layout=grid(%d cols, %.1f x %.1f mm cells, target height "
        "%.1f mm)\n",
        base.c_str(), sf.size(), glyphs.size(), skippedSanity, skippedEmpty,
        kCols, kCellW, kCellH, kTarget);
    doc->recompute();
    return true;
}

}  // namespace SWDWG
