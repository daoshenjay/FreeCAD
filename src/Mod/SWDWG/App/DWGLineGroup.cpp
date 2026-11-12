//! L2 linetype geometry cutting implementation.
//!
//! The pure 'A'-alignment cutting algorithm (which pattern intervals fall on
//! a line of a given length) lives in DWGLineCut.h - a zero-dependency header
//! shared with the standalone unit test.  This file only turns the resulting
//! arc-length ink intervals into real TopoDS_Edge sub-edges.
//!
//! Cut is edge-based (see docs/18-线形映射体系.md, 18.10): each TopoDS_Edge
//! is re-parameterised by arc length with GCPnts_AbscissaPoint and split into
//! ink sub-edges; faces (SOLID / FACE3D / HATCH fill) keep their boundaries
//! untouched.  The group stays a single Part::Feature, no object explosion.

#include "PreCompiled.h"

#include "DWGLineGroup.h"

#include "DWGLineCut.h"
#include "DWGLineType.h"
#include "DWGShapeFile.h"
#include "FontResolver.h"   // shared .shx parse cache (ShapeFileCache)
#include "DWGInternal.h"  // mapTextFont() for CJK TrueType fallback
#include "FTTextRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <App/Application.h>
#include <Base/Parameter.h>
#include <BRep_Builder.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <Geom_Curve.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

using namespace SWDWG;

PROPERTY_SOURCE(SWDWG::DWGLineGroup, Part::Feature)

//! Load a .shx by STYLE file name for the L2 (complex linetype) path.
//! Resolution (name -> path) keeps its own negative cache so a missing shape
//! file is only scanned for once; the actual file parse goes through the
//! process-wide ShapeFileCache so the same font / ltypeshp.shx is decoded at
//! most once even when the text SHX path uses it too.
std::shared_ptr<const ShapeFile> SWDWG::loadShapeFile(const std::string& name)
{
    static std::mutex mtx;
    static std::map<std::string, std::string> resolved;  // name -> path
    std::lock_guard<std::mutex> lk(mtx);
    auto rit = resolved.find(name);
    if (rit != resolved.end()) {
        if (rit->second.empty()) {
            return nullptr;  // searched before, not found
        }
        return ShapeFileCache::instance().get(rit->second);
    }
    std::vector<std::string> extra;
    // FreeCAD install root (bin/../data often holds support files); the
    // shape file may also live in the standard AutoCAD Fonts directories,
    // which findShapeFile() probes on its own.
    extra.push_back(App::Application::getHomePath());
    const std::string res = findShapeFile(name, extra);
    resolved[name] = res;
    if (res.empty()) {
        Base::Console().warning(
            "SWDWG: shape file '%s' not found - the symbols embedded in the "
            "complex linetypes that use it are left out (no placeholder is "
            "drawn). Put the .shx next to FreeCAD or in an AutoCAD Fonts "
            "directory to get them.\n",
            name.c_str());
        return nullptr;
    }
    std::shared_ptr<const ShapeFile> sf = ShapeFileCache::instance().get(res);
    if (sf) {
        SWDWG_LOG(
            "SWDWG: shape file '%s' resolved to '%s' (%zu shapes)\n",
            name.c_str(), res.c_str(), sf->size());
    }
    return sf;
}

namespace
{

//! Render embedded text with the STYLE font's .shx glyphs.  AutoCAD simple
//! fonts number their shapes by ASCII code and author them 8 shape units tall
//! for a nominal text height, so every shape unit maps to S/8 drawing units
//! (S = the complex-linetype scale).  The whole run is centred on the
//! insertion point, matching the placeholder box it replaces.
//!
//! Returns true when at least one glyph was drawn; the caller then keeps the
//! glyphs.  Returns false (caller falls back to the placeholder box) when no
//! font file is available or no character has a glyph.
bool addTtfTextGlyphs(const InkSeg& ink, const gp_Pnt& c, const gp_Dir& base,
                      const gp_Vec& norm, double h,
                      std::vector<TopoDS_Edge>& out)
{
    // Resolve the font family: for CJK text (most Chinese drawings) map the
    // AutoCAD style font through the shared mapper (STFangsong / SimSun etc.);
    // otherwise resolve the style font file name directly ("arial.ttf").
    std::string family = ink.shapefile;
    bool cjk = false;
    for (unsigned char ch : ink.text) {
        if (ch >= 0x80) {
            cjk = true;
            break;
        }
    }
    if (cjk) {
        family = mapTextFont(ink.shapefile, "", ink.text);
    }
    SWDWG_LOG(
        "[SWDWG TTF] text='%s' shapefile='%s' family='%s' cjk=%d h=%.4f\n",
        ink.text.c_str(), ink.shapefile.c_str(), family.c_str(), (int)cjk, h);
    const std::string fontFile = resolveFontFile(family);
    SWDWG_LOG("[SWDWG TTF] fontFile='%s'\n", fontFile.c_str());
    if (fontFile.empty()) {
        return false;
    }
    // Apply the STYLE width factor (DXF 41): AutoCAD stretches embedded
    // linetype text horizontally by it (Standard = 1.5), so the rendered
    // run is wider than the raw font metrics.
    const double wf = (ink.widthFactor > 0.0) ? ink.widthFactor : 1.0;
    const TTFRenderResult res = renderTextLine(ink.text, fontFile, h, wf);
    SWDWG_LOG(
        "[SWDWG TTF] render: ok=%d contours=%zu width=%.4f asc=%.4f wf=%.3f\n",
        (int)res.ok, res.contours.size(), res.width, res.ascender, wf);
    if (!res.ok || res.contours.empty()) {
        return false;
    }
    const double yMid = 0.5 * (res.ascender + res.descender);
    auto addEdge = [&out](const gp_Pnt& a, const gp_Pnt& b) {
        BRepBuilderAPI_MakeEdge me(a, b);
        if (me.IsDone()) {
            out.push_back(me.Edge());
        }
    };
    // AutoCAD's complex-linetype text element: the "insertion point" computed
    // by addSymbolGeometry (c = sCenter + X/Y offset along the line frame)
    // is the LEFT baseline of the text run, not its centre.  Drawing the run
    // from c (shift = 0) places the first glyph exactly where AutoCAD does,
    // so the ink-to-text spacing matches.
    const double shift = 0.0;
    for (const TTFPolyline& poly : res.contours) {
        const std::size_t n = poly.pts.size();
        if (n < 2) {
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const gp_Pnt a =
                c.Translated(gp_Vec(base) * (shift + poly.pts[i].first))
                 .Translated(norm * (poly.pts[i].second - yMid));
            const gp_Pnt b =
                c.Translated(gp_Vec(base) * (shift + poly.pts[j].first))
                 .Translated(norm * (poly.pts[j].second - yMid));
            addEdge(a, b);
        }
    }
    return true;
}

bool addTextGlyphs(const InkSeg& ink, const gp_Pnt& c, const gp_Dir& base,
                   const gp_Vec& norm, double h,
                   std::vector<TopoDS_Edge>& out)
{
    if (ink.shapefile.empty() || ink.text.empty()) {
        return false;
    }
    const std::string& f = ink.shapefile;
    const std::size_t dot = f.find_last_of('.');
    std::string ext = (dot == std::string::npos)
        ? ""
        : f.substr(dot + 1);
    for (char& ch : ext) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    if (ext == "ttf" || ext == "ttc" || ext == "otf") {
        // TrueType / OpenType style font: render the real glyph outlines with
        // FreeType (FTTextRenderer) instead of the placeholder box.
        return addTtfTextGlyphs(ink, c, base, norm, h, out);
    }
    if (ext != "shx" && ext != "shp") {
        return false;
    }
    const auto sf = loadShapeFile(ink.shapefile);
    if (!sf) {
        return false;
    }
    const double k = h / 8.0;  // 8 shape units == text height S
    struct Glyph
    {
        double w = 0.0;  // advance width in drawing units
        std::vector<ShapeSeg> segs;
    };
    std::vector<Glyph> glyphs;
    glyphs.reserve(ink.text.size());
    double total = 0.0;
    std::size_t hit = 0;
    for (unsigned char ch : ink.text) {
        Glyph g;
        const auto it = sf->find((int)ch);
        if (it != sf->end() && !it->second.segs.empty()) {
            double maxx = 0.0;
            for (const ShapeSeg& s : it->second.segs) {
                maxx = std::max(maxx, std::max(s.x0, s.x1));
            }
            g.w = maxx * k;
            g.segs = it->second.segs;
            ++hit;
        }
        if (g.w < 1e-9) {
            g.w = 0.6 * h;  // missing glyph / space: placeholder advance
        }
        total += g.w;
        glyphs.push_back(std::move(g));
    }
    if (hit == 0) {
        return false;  // nothing found: keep the placeholder box
    }
    // Vertical centring: shift the glyphs so their outline range sits on the
    // insertion point (fonts carry the baseline below mid-height).
    double yMin = 1e300;
    double yMax = -1e300;
    for (const Glyph& g : glyphs) {
        for (const ShapeSeg& s : g.segs) {
            yMin = std::min(yMin, std::min(s.y0, s.y1));
            yMax = std::max(yMax, std::max(s.y0, s.y1));
        }
    }
    const double yMid = (yMin + yMax) * 0.5 * k;
    auto addEdge = [&out](const gp_Pnt& a, const gp_Pnt& b) {
        BRepBuilderAPI_MakeEdge me(a, b);
        if (me.IsDone()) {
            out.push_back(me.Edge());
        }
    };
    double penX = -0.5 * total;  // centre the whole run on the insertion point
    for (const Glyph& g : glyphs) {
        for (const ShapeSeg& s : g.segs) {
            const gp_Pnt a = c.Translated(gp_Vec(base) * (penX + s.x0 * k))
                             .Translated(norm * (s.y0 * k - yMid));
            const gp_Pnt b = c.Translated(gp_Vec(base) * (penX + s.x1 * k))
                             .Translated(norm * (s.y1 * k - yMid));
            addEdge(a, b);
        }
        penX += g.w;
    }
    return true;
}

//! Append the geometry of one complex symbol (embedded text / shape marker)
//! centred on origin with the tangent direction.
//!
//! Base direction = edge tangent + the dash rotation (counter-clockwise about
//! Z); offsets shift the insertion point along the base and its
//! counter-clockwise normal (AutoCAD complex-linetype semantics).
//!
//! Text elements are rendered from the STYLE font's .shx glyphs (shape
//! number = ASCII code, height S/8 units per shape unit).  Shape elements are
//! restored from their .shx body (18.10.6): the outline is a polyline of short
//! chords (arcs sampled), scaled by S and drawn along the tangent.
//!
//! When the font / shape file cannot be resolved, nothing is added: a
//! placeholder of size S is not what AutoCAD draws (its marks come from the
//! shape and are far smaller), and stamping one at every pattern element turns
//! a patterned outline into a heap of markers.
void addSymbolGeometry(const InkSeg& ink,
                       const gp_Pnt& origin,
                       const gp_Vec& tangent,
                       std::vector<TopoDS_Edge>& out)
{
    if (tangent.Magnitude() < 1e-12) {
        return;  // degenerate point: nothing to anchor a symbol on
    }
    // The linetype frame: tangent and its counter-clockwise normal.
    // AutoCAD offsets (x_offset, y_offset) are given in this unrotated
    // line frame, so the insertion point must be computed before applying
    // the symbol's own rotation.  Otherwise mirrored/rotated symbols in
    // patterns like BATTING (two BAT shapes, the second rotated 180 deg)
    // collapse to the same side of the line instead of complementing each
    // other.
    gp_Dir base0(tangent);
    gp_Vec nv0(0.0, 0.0, 1.0);
    nv0.Cross(gp_Vec(base0));
    if (nv0.Magnitude() < 1e-12) {
        nv0.SetCoord(1.0, 0.0, 0.0);
        nv0.Cross(gp_Vec(base0));  // base along Z: pick any perpendicular
    }
    const gp_Vec norm0 = nv0.Normalized();
    const gp_Pnt c = origin.Translated(gp_Vec(base0) * ink.xOffset)
                           .Translated(norm0 * ink.yOffset);

    // Apply the symbol's own rotation for drawing.
    gp_Dir base = base0;
    if (std::fabs(ink.symRotation) > 1e-12) {
        const double cr = std::cos(ink.symRotation);
        const double sr = std::sin(ink.symRotation);
        base = gp_Dir(cr * base.X() - sr * base.Y(),
                      sr * base.X() + cr * base.Y(), base.Z());
    }
    gp_Vec nv(0.0, 0.0, 1.0);
    nv.Cross(gp_Vec(base));
    if (nv.Magnitude() < 1e-12) {
        nv.SetCoord(1.0, 0.0, 0.0);
        nv.Cross(gp_Vec(base));  // base along Z: pick any perpendicular
    }
    const gp_Vec norm = nv.Normalized();

    const double h = ink.symScale > 1e-9 ? ink.symScale : 1.0;
    auto addEdge = [&out](const gp_Pnt& a, const gp_Pnt& b) {
        BRepBuilderAPI_MakeEdge me(a, b);
        if (me.IsDone()) {
            out.push_back(me.Edge());
        }
    };
    if (ink.shapeFlag == 4) {
        // Embedded text: render the real glyphs from the STYLE font's .shx
        // (shape number = ASCII code, 18.10.6).  When the font is missing or
        // no character has a glyph the run degrades to the placeholder box.
        //
        // AutoCAD's complex-linetype Y offset positions the text BASELINE
        // (not the geometric centre).  FreeCAD users expect the glyphs to
        // straddle the line - drop the Y offset so the text centre sits on
        // the line, matching the visible AutoCAD behaviour for patterns like
        // GAS_LINE (y=-1.27) and HOT_WATER_SUPPLY (y=-1.27).
        const gp_Pnt textC = c.Translated(-norm * ink.yOffset);
        // Nothing is drawn when the font is unavailable.  The old fallback put
        // a rectangle of size S (0.6 x S per character) with both diagonals at
        // every pattern element; on the bridge plan that pattern is DASHED4,
        // whose text element repeats along every abutment outline, so the
        // fallback covered the drawing with hundreds of boxes - the reported
        // "the abutment is surrounded by a heap of little squares".  AutoCAD
        // has the font and stamps the real glyph; inventing geometry of a
        // different size is worse than leaving the run out.
        addTextGlyphs(ink, textC, base, norm, h, out);
    }
    else if (ink.shapeFlag == 2) {
        // Embedded shape: restore the body from its .shx file when available.
        if (!ink.shapefile.empty()) {
            if (const auto sf = loadShapeFile(ink.shapefile)) {
                const auto it = sf->find(ink.complexShapecode);
                if (it != sf->end() && !it->second.segs.empty()) {
                    // Shape units x S, laid out along (base, norm) exactly
                    // like the text box.  Stamp the symbol at `c` (sCenter
                    // plus the linetype X/Y offsets) - the same point the
                    // linetype definition places it at.  Do NOT shift the
                    // symbol along the line: for BATTING the BAT#1 foot
                    // lands 8.71 world short of the BAT#2 foot, and that
                    // crossing is what AutoCAD draws; collapsing the two
                    // onto the same point is what produced the green bar
                    // between them.
                    const ShapeDef& shp = it->second;
                    const auto& segs = shp.segs;

                    // Straight strokes as real edges; arc chords are skipped
                    // and replaced by exact circular edges from shp.arcs so
                    // round symbols (dots, wheels, ...) stay round.
                    for (const ShapeSeg& seg : segs) {
                        if (seg.arcChord) {
                            continue;  // represented by a ShapeArc below
                        }
                        const gp_Pnt a =
                            c.Translated(gp_Vec(base) * (seg.x0 * h))
                                  .Translated(norm * (seg.y0 * h));
                        const gp_Pnt b =
                            c.Translated(gp_Vec(base) * (seg.x1 * h))
                                  .Translated(norm * (seg.y1 * h));
                        addEdge(a, b);
                    }
                    // Exact circular arcs in the symbol frame: X = base,
                    // Y = norm, Z = base x norm (right-handed, so increasing
                    // parameter runs base -> norm like the shape grid).
                    for (const ShapeArc& a : shp.arcs) {
                        const double rr = a.r * h;
                        if (rr < 1e-9) {
                            continue;
                        }
                        const gp_Pnt centre =
                            c.Translated(gp_Vec(base) * (a.cx * h))
                                  .Translated(norm * (a.cy * h));
                        // base x norm (unit, perpendicular by construction).
                        const double zx = base.Y() * norm.Z() - base.Z() * norm.Y();
                        const double zy = base.Z() * norm.X() - base.X() * norm.Z();
                        const double zz = base.X() * norm.Y() - base.Y() * norm.X();
                        if (zx * zx + zy * zy + zz * zz < 1e-24) {
                            continue;
                        }
                        const gp_Ax2 ax(centre, gp_Dir(zx, zy, zz), gp_Dir(base));
                        const gp_Circ circ(ax, rr);
                        const double kPi = 3.14159265358979323846;
                        BRepBuilderAPI_MakeEdge me =
                            (a.sweepDeg >= 359.999)
                                ? BRepBuilderAPI_MakeEdge(circ)
                                : BRepBuilderAPI_MakeEdge(
                                      circ, a.startDeg * kPi / 180.0,
                                      (a.startDeg + a.sweepDeg) * kPi / 180.0);
                        if (me.IsDone()) {
                            out.push_back(me.Edge());
                        }
                    }
                    // No implicit base line either.  The ltypeshp.shx
                    // BAT bytes (25 0A 02 C4 2B 00) define no closing
                    // base segment; AutoCAD draws it open and lets the
                    // two BATs interlock naturally.
                    return;
                }
            }
        }
        // Nothing is drawn when the shape file or the shape itself is missing.
        // The old fallback stamped a diamond of size S at every pattern
        // element, which invents geometry AutoCAD does not have: the marks are
        // far smaller there (they come from the shape), so a patterned outline
        // turned into a heap of markers.
    }
}

//! Parameter of the point located at arc length s (absolute from edge start),
//! searched from (uStart, sStart) forward.
double paramAtAbscissa(const BRepAdaptor_Curve& curve,
                       double s,
                       double uStart,
                       double sStart)
{
    if (s <= sStart + 1e-12) {
        return uStart;
    }
    GCPnts_AbscissaPoint ap(curve, s - sStart, uStart);
    return ap.IsDone() ? ap.Parameter() : uStart;
}

//! Cut one edge into ink sub-edges.  Returns the original edge on failure or
//! when the whole edge is ink.
std::vector<TopoDS_Edge> cutEdge(const TopoDS_Edge& edge,
                                 const std::vector<LineTypeDash>& dashes,
                                 double scale,
                                 double patternLen)
{
    std::vector<TopoDS_Edge> out;
    try {
        BRepAdaptor_Curve curve(edge);
        const double L = GCPnts_AbscissaPoint::Length(curve);
        if (L <= 1e-9) {
            out.push_back(edge);
            return out;
        }
        std::vector<InkSeg> inks = computeInkSegments(L, dashes, scale,
                                                      patternLen);
        if (inks.size() == 1 && !inks[0].dot && !inks[0].complex
            && inks[0].s0 <= 1e-9 && inks[0].s1 >= L - 1e-9) {
            out.push_back(edge);  // fully ink
            return out;
        }
        double u = curve.FirstParameter();
        double sAcc = 0.0;
        for (const InkSeg& ink : inks) {
            if (ink.complex) {
                // Complex element.  A positive-length complex dash (e.g.
                // TRACKS "3.81,3.81[TRACK1,...]") draws its ink run first and
                // then stamps the symbol at the run end (the linetype
                // definition vertex, plus X/Y offsets).  Negative-length
                // complex dashes (BATTING BAT) are shape-only markers with
                // s1 == s0, so no ink is produced here.
                if (ink.s1 - ink.s0 > 1e-9) {
                    const double ua = paramAtAbscissa(curve, ink.s0, u, sAcc);
                    const double ub = paramAtAbscissa(curve, ink.s1, ua,
                                                      ink.s0);
                    BRepBuilderAPI_MakeEdge me(curve.Curve().Curve(), ua, ub);
                    if (me.IsDone()) {
                        out.push_back(TopoDS::Edge(me.Edge().Moved(edge.Location())));
                    }
                    u = ub;
                    sAcc = ink.s1;
                }
                if (ink.sCenter >= -1e-9 && ink.sCenter <= L + 1e-9) {
                    const double uc = paramAtAbscissa(curve, ink.sCenter, u,
                                                      sAcc);
                    gp_Pnt p;
                    gp_Vec tv;
                    curve.D1(uc, p, tv);
                    addSymbolGeometry(ink, p, tv, out);
                }
                continue;
            }
            if (ink.dot) {
                // Dot: a zero-length pattern element.  AutoCAD stamps it as a
                // tiny round mark (roughly pen width), so we cut a small ink
                // stub centred on the dot phase.  The stub must stay small
                // (0.15 x scale, min 0.1 mm) or dot-heavy patterns distort:
                // e.g. ISO06's double dots are only 0.5 mm apart at scale 1
                // and the old fixed 0.5 mm stub welded them into one solid
                // run (visible length / spacing then disagrees with
                // AutoCAD).  Centring keeps the mark on its phase instead of
                // offsetting it down the line.
                if (ink.s0 >= L - 1e-9) {
                    continue;
                }
                double ds = std::max(0.15 * scale, 0.1);
                double lo = ink.s0 - 0.5 * ds;
                double hi = ink.s0 + 0.5 * ds;
                if (lo < sAcc + 1e-9) {
                    lo = std::max(sAcc, 0.0);  // never overlap previous ink
                }
                if (hi > L) {
                    hi = L;
                }
                if (hi - lo < 1e-9) {
                    continue;
                }
                const double ua = paramAtAbscissa(curve, lo, u, sAcc);
                const double ub = paramAtAbscissa(curve, hi, ua, lo);
                // curve.Curve().Curve() is the bare Geom_Curve of the edge: it
                // carries no location.  The cut sub-edge must keep the one the
                // original edge has, or every dash lands at the edge's
                // *untransformed* coordinates - measured on the bridge drawing,
                // the cut geometry of the block content appeared next to the
                // origin (x 1136.1 / y -205.0, i.e. exactly the coordinates used
                // before the insert transform was applied).
                BRepBuilderAPI_MakeEdge me(curve.Curve().Curve(), ua, ub);
                if (me.IsDone()) {
                    out.push_back(TopoDS::Edge(me.Edge().Moved(edge.Location())));
                }
                continue;  // do not advance the phase (dot has no length)
            }
            if (ink.s1 - ink.s0 < 1e-9) {
                continue;
            }
            const double ua = paramAtAbscissa(curve, ink.s0, u, sAcc);
            const double ub = paramAtAbscissa(curve, ink.s1, ua, ink.s0);
            BRepBuilderAPI_MakeEdge me(curve.Curve().Curve(), ua, ub);
            if (me.IsDone()) {
                out.push_back(TopoDS::Edge(me.Edge().Moved(edge.Location())));
            }
            u = ub;
            sAcc = ink.s1;
        }
    }
    catch (const Standard_Failure&) {
        out.clear();
        out.push_back(edge);
    }
    if (out.empty()) {
        out.push_back(edge);
    }
    return out;
}

}  // namespace

namespace SWDWG
{

//! Cut every free edge of shape with the linetype pattern.  Faces (SOLID /
//! FACE3D / HATCH fill) keep their boundaries untouched; vertices pass
//! through.  Returns a new compound, or the input unchanged on failure.
//! (Declared in DWGLineGroup.h with a default patternLen; must live in the
//! SWDWG namespace, not the anonymous one, to match that declaration.)
TopoDS_Shape expandShapeByPattern(const TopoDS_Shape& shape,
                                  const std::vector<LineTypeDash>& dashes,
                                  double scale,
                                  double patternLen)
{
    BRep_Builder B;
    TopoDS_Compound out;
    B.MakeCompound(out);
    bool any = false;

    // Faces and the edges/vertices they use.
    TopTools_IndexedMapOfShape faceEdges;
    TopTools_IndexedMapOfShape faceVerts;
    {
        TopExp_Explorer fexp(shape, TopAbs_FACE);
        for (; fexp.More(); fexp.Next()) {
            const TopoDS_Face& f = TopoDS::Face(fexp.Current());
            B.Add(out, f);
            any = true;
            TopExp::MapShapes(f, TopAbs_EDGE, faceEdges);
            TopExp::MapShapes(f, TopAbs_VERTEX, faceVerts);
        }
    }
    // Free edges are cut.
    TopExp_Explorer eexp(shape, TopAbs_EDGE);
    for (; eexp.More(); eexp.Next()) {
        const TopoDS_Edge& e = TopoDS::Edge(eexp.Current());
        if (faceEdges.Contains(e)) {
            continue;
        }
        std::vector<TopoDS_Edge> subs = cutEdge(e, dashes, scale, patternLen);
        for (const TopoDS_Edge& se : subs) {
            B.Add(out, se);
            any = true;
        }
    }
    // Free vertices (POINT entities).
    TopExp_Explorer vexp(shape, TopAbs_VERTEX);
    for (; vexp.More(); vexp.Next()) {
        const TopoDS_Vertex& v = TopoDS::Vertex(vexp.Current());
        if (faceVerts.Contains(v)) {
            continue;
        }
        B.Add(out, v);
        any = true;
    }
    if (!any) {
        return shape;
    }
    return out;
}

DWGLineGroup::DWGLineGroup()
{
    ADD_PROPERTY_TYPE(DWGSourceShape,
                      (TopoDS_Shape()),
                      "DWG",
                      App::Prop_None,
                      "Raw geometry before L2 linetype cutting");
    ADD_PROPERTY_TYPE(Layer,
                      ("0"),
                      "DWG",
                      App::Prop_None,
                      "DWG layer name");
    ADD_PROPERTY_TYPE(EntityKind,
                      ("LINE"),
                      "DWG",
                      App::Prop_ReadOnly,
                      "Imported DWG entity kind (tree grouping key)");
    ADD_PROPERTY_TYPE(DWGColor,
                      (1.0f, 1.0f, 1.0f),
                      "DWG",
                      App::Prop_None,
                      "Resolved DWG colour");
    ADD_PROPERTY_TYPE(DWGLineWidth,
                      (0.25),
                      "DWG",
                      App::Prop_None,
                      "Line weight in mm");
    ADD_PROPERTY_TYPE(DWGLineType,
                      (""),
                      "DWG",
                      App::Prop_None,
                      "Resolved linetype name");
    ADD_PROPERTY_TYPE(DWGLinePattern,
                      (0xffff),
                      "DWG",
                      App::Prop_None,
                      "L1 quantized 16-bit pattern");
    ADD_PROPERTY_TYPE(DWGLineScale,
                      (1.0),
                      "DWG",
                      App::Prop_None,
                      "Effective linetype scale (LTSCALE x entity)");
    ADD_PROPERTY_TYPE(DWGLineTypeRef,
                      (nullptr),
                      "DWG",
                      App::Prop_None,
                      "Shared linetype definition (L2)");
    // The link is Global, not Local: the linetype definitions live in their
    // own "定义" App::Part while the referencing line groups live in per-layer
    // kind Parts.  A Local-scoped link would make
    // GeoFeatureGroupExtension::addObjects() pull the whole connected
    // component of Local links (every line group of every layer that shares
    // the linetype) into the same Part, which then showed up as "duplicate
    // child item" in the tree and cross-layer crosstalk between containers.
    // App::PropertyLink defaults to LinkScope::Local, so this must be
    // explicit.
    DWGLineTypeRef.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(DashesMaterialized,
                      (false),
                      "DWG",
                      App::Prop_None,
                      "When true the linetype pattern is really cut into "
                      "Shape (large geometry; enable on demand for export / "
                      "measurement).  False keeps the light raw shape.");
}

DWGLineGroup::~DWGLineGroup() = default;

void DWGLineGroup::beforeSave() const
{
    // Shape is a derived mirror of DWGSourceShape, so writing it would store
    // the very same BRep twice (double file size, save time and peak memory).
    // It is marked transient here, right before the property list is written,
    // rather than in the constructor:
    // PropertyContainer::Restore() skips a property whose *current* transient
    // bit is set when the file carries no status for it - which is exactly the
    // case for documents written before the mirror existed, so a constructor
    // default of "transient" silently threw their geometry away.
    auto* self = const_cast<DWGLineGroup*>(this);
    self->Shape.setStatus(App::Property::Status::Transient, true);
    Part::Feature::beforeSave();
}

void DWGLineGroup::onDocumentRestored()
{
    // Shape is transient and comes back empty, so refill it from the persisted
    // DWGSourceShape.  Documents written before this layout may be the other
    // way round (geometry only in Shape), and switching DWGSourceShape to the
    // binary property type means older files' DWGSourceShape is skipped by
    // PropertyContainer::Restore (it compares type names strictly), so refill
    // in whichever direction is missing data.
    if (!Shape.testStatus(App::Property::Status::Transient)) {
        // Restored from a file that predates the transient mirror: keep the
        // mirror out of the file from now on.
        Shape.setStatus(App::Property::Status::Transient, true);
    }
    if (DWGSourceShape.getValue().IsNull() && !Shape.getValue().IsNull()) {
        DWGSourceShape.setValue(Shape.getValue());
        DWGSourceShape.purgeTouched();
    }
    if (Shape.getValue().IsNull() && !DWGSourceShape.getValue().IsNull()) {
        Shape.setValue(DWGSourceShape.getValue());
        Shape.purgeTouched();
    }
    Part::Feature::onDocumentRestored();
}

short DWGLineGroup::mustExecute() const
{
    // DWGLinePattern is in the list because editing a shared DWGLineType
    // definition (Pattern or ScaleOverride) touches it on every linked group -
    // that touch is what has to bring this object through a recompute so the
    // GUI re-applies the updated view-provider styles.
    if (DWGSourceShape.isTouched() || Shape.isTouched()
        || DWGLineTypeRef.isTouched() || DWGLineScale.isTouched()
        || DWGLinePattern.isTouched()) {
        return 1;
    }
    return Part::Feature::mustExecute();
}

App::DocumentObjectExecReturn* DWGLineGroup::execute()
{
    const auto exT0 = std::chrono::steady_clock::now();
    const auto exMs = [&exT0]() {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - exT0)
            .count();
    };
    // P0 / P3 (direction-2, default ON): unless a group explicitly requests
    // materialisation (DashesMaterialized = true) or the global preference
    // "Dir2RawShape" is set to false, the group keeps ONLY the raw source
    // geometry in Shape (no linetype cut), so import / file / open stay light.
    // The dash pattern stays available through DWGLineTypeRef / DWGLineScale:
    // the display view provider (ViewProviderDWGLineGroup) strokes the dashes
    // from the raw edges, and setting DashesMaterialized reuses
    // expandShapeByPattern() to cut the real geometry on demand (P3).
    static const bool dir2 = []() {
        const Base::Reference<ParameterGrp> hGrp =
            App::GetApplication().GetParameterGroupByPath(
                "User parameter:BaseApp/Preferences/Mod/SWDWG");
        return hGrp ? hGrp->GetBool("Dir2RawShape", true) : true;
    }();
    // Raw display path.  DWGSourceShape is authoritative whenever it holds
    // something (it is the persisted geometry, and also what a group switched
    // back from materialised keeps); if only Shape has geometry (documents
    // written by older builds) the source is filled from it.
    // NOTE: the display must not depend on this method writing Shape - the
    // importer touches Shape explicitly once the Importing lock is released.
    const TopoDS_Shape raw = DWGSourceShape.getValue();
    if (dir2 && !DashesMaterialized.getValue()) {
        const TopoDS_Shape cur = Shape.getValue();
        if (!raw.IsNull()) {
            if (cur.IsNull() || !cur.IsSame(raw)) {
                Shape.setValue(raw);
            }
        }
        else if (!cur.IsNull()) {
            DWGSourceShape.setValue(cur);
            // The write above must not leave the group permanently dirty,
            // otherwise every recompute would re-enter execute().
            DWGSourceShape.purgeTouched();
        }
        return App::DocumentObject::StdReturn;
    }
    // Materialise (L2 cut): the cut replaces Shape, never the source.
    TopoDS_Shape src = raw;
    if (src.IsNull()) {
        src = Shape.getValue();
        if (!src.IsNull()) {
            DWGSourceShape.setValue(src);
            DWGSourceShape.purgeTouched();
        }
    }
    if (src.IsNull()) {
        Shape.setValue(src);
        return App::DocumentObject::StdReturn;
    }
    std::vector<LineTypeDash> dashes;
    const double groupScale = DWGLineScale.getValue();
    double scale = effectiveLineScale(groupScale, 1.0);
    double patternLen = 0.0;
    App::DocumentObject* ref = DWGLineTypeRef.getValue();
    SWDWG_LOG(
        "[SWDWG EXEC] group '%s' ref=%p scale=%.6g srcNull=%d\n",
        getNameInDocument(), static_cast<void*>(ref), scale, src.IsNull());
    if (ref) {
        // Note: DWGLineType is both a member (PropertyString) and a class name;
        // use the fully qualified name for the cast.
        if (auto* lt = dynamic_cast<SWDWG::DWGLineType*>(ref)) {
            dashes = lt->dashes();
            patternLen = lt->patternLen();
            // The pattern belongs to the shared definition, the scale does not:
            // every line keeps its own import-time scale and the definition
            // only contributes a shared multiplier (see effectiveLineScale).
            // Overwriting the scale with the definition's made all but one of
            // the linked groups cut at a foreign scale.
            scale = effectiveLineScale(groupScale, lt->ScaleOverride.getValue());
            SWDWG_LOG(
                "[SWDWG EXEC] group '%s' cast OK dashes=%zu ownScale=%.6g "
                "override=%.6g used=%.6g patLen=%.6g\n",
                getNameInDocument(), dashes.size(), groupScale,
                lt->ScaleOverride.getValue(), scale, patternLen);
        }
        else {
            SWDWG_LOG(
                "[SWDWG EXEC] group '%s' cast FAILED ref type=%s\n",
                getNameInDocument(), ref->getTypeId().getName());
        }
    }
    if (dashes.empty()) {
        // CONTINUOUS / unknown linetype: keep the raw geometry.
        SWDWG_LOG(
            "[SWDWG EXEC] group '%s' dashes empty -> keep raw geometry "
            "(%.1f ms)\n",
            getNameInDocument(), exMs());
        Shape.setValue(src);
        return App::DocumentObject::StdReturn;
    }
    // AutoCAD 'A' alignment parameters (diagnostics).
    {
        double P = 0.0;
        for (const auto& d : dashes) {
            P += std::fabs(d.length) * scale;
        }
        const double e1 = dashes.empty() ? 0.0 : std::fabs(dashes[0].length) * scale;
        SWDWG_LOG("[SWDWG EXEC] group '%s' P=%.6g e1=%.6g\n",
                                getNameInDocument(), P, e1);
    }
    TopoDS_Shape out;
    try {
        out = expandShapeByPattern(src, dashes, scale, patternLen);
    }
    catch (const Standard_Failure&) {
        out.Nullify();
    }
    if (!out.IsNull()) {
        int srcEdges = 0;
        {
            TopExp_Explorer ex(src, TopAbs_EDGE);
            for (; ex.More(); ex.Next()) {
                ++srcEdges;
            }
        }
        int cutEdges = 0;
        {
            TopExp_Explorer ex(out, TopAbs_EDGE);
            for (; ex.More(); ex.Next()) {
                ++cutEdges;
            }
        }
        SWDWG_LOG(
            "[SWDWG EXEC] group '%s' srcEdges=%d cutEdges=%d dashes=%zu "
            "P=%.6g scale=%.6g (%.1f ms)\n",
            getNameInDocument(), srcEdges, cutEdges, dashes.size(),
            [&dashes, scale]() {
                double p = 0.0;
                for (const auto& d : dashes) {
                    p += std::fabs(d.length) * scale;
                }
                return p;
            }(),
            scale, exMs());
    }
    Shape.setValue(out.IsNull() ? src : out);
    return App::DocumentObject::StdReturn;
}

}  // namespace SWDWG
