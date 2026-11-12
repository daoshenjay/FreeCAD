//! Entity -> OCC shape building.
//!
//! One case per entity type in buildShape().  This is the place to look when
//! the geometry of a specific entity type (LINE, LWPOLYLINE, SPLINE, HATCH,
//! ...) renders wrong: the transform M is applied here and the OCC topology
//! is constructed here.

#include "DWGInternal.h"

#include <Base/Console.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_BSplineCurve.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Elips.hxx>
#include <gp_Pnt.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

namespace SWDWG
{

// Spline-fit polyline rendering diagnostics (imports are single-threaded).
int s_fitPolyTried = 0;
int s_fitPolyQuadOk = 0;
int s_fitPolyCubicOk = 0;
int s_fitPolyNotDone = 0;
int s_fitPolyCurveNull = 0;
int s_fitPolyEdgeNull = 0;
int s_fitPolyException = 0;

TopoDS_Edge makeSegEdge(const Vec3& p1, const Vec3& p2, double b, const Mat4& M)
{
    if (std::fabs(b) < 1e-9) {
        Vec3 a = xform(p1, M);
        Vec3 c = xform(p2, M);
        BRepBuilderAPI_MakeEdge mk(gp_Pnt(a.x, a.y, a.z),
                                   gp_Pnt(c.x, c.y, c.z));
        return mk.IsDone() ? mk.Edge() : TopoDS_Edge();
    }
    // Arc segment: bulge b = tan(theta/4).  Per the DWG reference, the
    // arc is CCW from p1 to p2 when b > 0 (CW when b < 0).  For a chord
    // (0,0)->(1,0) the CCW short arc dips BELOW the chord (apex on the
    // RIGHT side of the start-to-end vector), so the sagitta must be added
    // along the RIGHT perpendicular.  The earlier LEFT-perpendicular code
    // produced CW arcs for positive bulge - - i.e. rectangle chamfers were
    // drawn concave instead of convex.
    Vec3 v = {p2.x - p1.x, p2.y - p1.y, 0.0};
    double d = std::hypot(v.x, v.y);
    if (d < 1e-12) {
        return TopoDS_Edge();
    }
    double h = 0.5 * d * b;  // signed sagitta
    Vec3 mid = {(p1.x + p2.x) / 2.0, (p1.y + p2.y) / 2.0, (p1.z + p2.z) / 2.0};
    Vec3 perp = {v.y / d, -v.x / d, 0.0};
    Vec3 apex = {mid.x + perp.x * h, mid.y + perp.y * h, mid.z};

    Vec3 a = xform(p1, M);
    Vec3 q = xform(apex, M);
    Vec3 c = xform(p2, M);
    GC_MakeArcOfCircle mk(gp_Pnt(a.x, a.y, a.z), gp_Pnt(q.x, q.y, q.z),
                          gp_Pnt(c.x, c.y, c.z));
    if (mk.IsDone()) {
        return BRepBuilderAPI_MakeEdge(mk.Value());
    }
    // Degenerate arc: fall back to a straight segment.
    BRepBuilderAPI_MakeEdge mk2(gp_Pnt(a.x, a.y, a.z), gp_Pnt(c.x, c.y, c.z));
    return mk2.IsDone() ? mk2.Edge() : TopoDS_Edge();
}

TopoDS_Shape makeCircleLike(const Vec3& c, const Vec3& vx, const Vec3& vy)
{
    double lx = norm(vx);
    double ly = norm(vy);
    if (lx < 1e-9 || ly < 1e-9) {
        return TopoDS_Shape();
    }
    Vec3 n = cross(vx, vy);
    double nn = norm(n);
    if (nn < 1e-12 * lx * ly) {
        return TopoDS_Shape();
    }
    Vec3 un = {n.x / nn, n.y / nn, n.z / nn};
    gp_Pnt pc(c.x, c.y, c.z);
    gp_Dir dn(un.x, un.y, un.z);
    if (std::fabs(lx - ly) < 1e-6 * std::max(lx, ly)) {
        double r = 0.5 * (lx + ly);
        return BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(pc, dn), r));
    }
    // Non-uniform transform: represent as an ellipse.  The major axis must
    // be the longer one or gp_Elips will fail.
    if (ly > lx) {
        Vec3 ey = {vy.x / ly, vy.y / ly, vy.z / ly};
        return BRepBuilderAPI_MakeEdge(
            gp_Elips(gp_Ax2(pc, dn, gp_Dir(ey.x, ey.y, ey.z)), ly, lx));
    }
    Vec3 ex = {vx.x / lx, vx.y / lx, vx.z / lx};
    return BRepBuilderAPI_MakeEdge(
        gp_Elips(gp_Ax2(pc, dn, gp_Dir(ex.x, ex.y, ex.z)), lx, ly));
}

TopoDS_Shape buildShape(const Entity& e, Mat4 M)
{
    // Every 2D entity stores its coordinates in the plane defined by its
    // extrusion (DXF 210-230), so for a flipped normal - (0,0,-1), which any
    // AutoCAD MIRROR leaves behind - the stored numbers are mirrored relative
    // to world space.  Folding the OCS basis into the placement matrix here
    // fixes all of them at once (arcs, circles, ellipses, polyline vertices
    // *and* their bulges, solids, hatch boundaries) instead of reworking each
    // builder's angles, and it composes correctly with a tilted plane too.
    //
    // Row-vector convention: the OCS rotation applies first (the local
    // coordinates are OCS), the entity's placement second.
    if (!(e.extrusion.z == 1.0 && e.extrusion.x == 0.0
          && e.extrusion.y == 0.0)) {
        M = matMul(ocsToWorld(e.extrusion), M);
    }

    switch (e.type) {
        case EntType::LINE: {
            if (e.pts.size() < 2) {
                return TopoDS_Shape();
            }
            Vec3 a = xform(e.pts[0], M);
            Vec3 b = xform(e.pts[1], M);
            return BRepBuilderAPI_MakeEdge(gp_Pnt(a.x, a.y, a.z),
                                           gp_Pnt(b.x, b.y, b.z));
        }
        case EntType::LWPOLYLINE: {
            if (e.pts.size() < 2) {
                return TopoDS_Shape();
            }
            // Curve/spline-fit POLYLINEs are drawn as smooth quadratic
            // B-splines.  AutoCAD stores the original vertices as the B-spline
            // frame control points (poles); the curve is pulled by them but
            // only passes through the first and last ones.
            if (e.fitSpline && e.pts.size() >= 3) {
                ++s_fitPolyTried;
                // Returns a null shape when smooth rendering is not possible;
                // the caller then falls through to the faceted polyline.
                auto trySmooth = [&]() -> TopoDS_Shape {
                    // Pre-process: drop non-finite points and consecutive
                    // duplicates (OCCT interpolation is singular on them),
                    // and skip curves whose vertices are all collinear.
                    std::vector<Vec3> clean;
                    clean.reserve(e.pts.size());
                    for (const Vec3& p : e.pts) {
                        if (!std::isfinite(p.x) || !std::isfinite(p.y)
                            || !std::isfinite(p.z)) {
                            continue;
                        }
                        if (!clean.empty()) {
                            const Vec3& q = clean.back();
                            double dx = p.x - q.x;
                            double dy = p.y - q.y;
                            double dz = p.z - q.z;
                            if (dx * dx + dy * dy + dz * dz < 1e-18) {
                                continue;  // duplicate vertex
                            }
                        }
                        clean.push_back(p);
                    }
                    bool degenerate = clean.size() < 3;
                    if (!degenerate) {
                        const Vec3& f = clean.front();
                        const Vec3& l = clean.back();
                        Vec3 dir = sub(l, f);
                        double len2 = dot(dir, dir);
                        if (len2 < 1e-18) {
                            degenerate = true;  // all vertices coincide
                        }
                        else {
                            double maxD2 = 0.0;
                            for (size_t k = 1; k + 1 < clean.size(); ++k) {
                                Vec3 cr = cross(dir, sub(clean[k], f));
                                double d2 = dot(cr, cr) / len2;
                                if (d2 > maxD2) {
                                    maxD2 = d2;
                                }
                            }
                            if (maxD2 < 1e-12) {
                                degenerate = true;  // collinear
                            }
                        }
                    }
                    if (degenerate) {
                        return TopoDS_Shape();
                    }
                    try {
                        const Standard_Integer n =
                            static_cast<Standard_Integer>(clean.size());
                        // AutoCAD's spline-fit polylines are quadratic B-splines
                        // whose poles are the frame control points.  The curve is
                        // pulled by the control points but only passes through
                        // the first and last ones, which gives the smoother
                        // look seen in AutoCAD.  Build the curve directly as a
                        // clamped quadratic B-spline.
                        TColgp_Array1OfPnt poles(1, n);
                        for (Standard_Integer k = 0; k < n; ++k) {
                            Vec3 p = xform(clean[k], M);
                            poles.SetValue(k + 1, gp_Pnt(p.x, p.y, p.z));
                        }
                        TColStd_Array1OfReal knots(1, n - 1);
                        TColStd_Array1OfInteger mults(1, n - 1);
                        knots.SetValue(1, 0.0);
                        mults.SetValue(1, 3);
                        knots.SetValue(n - 1, static_cast<double>(n - 2));
                        mults.SetValue(n - 1, 3);
                        for (Standard_Integer k = 2; k <= n - 2; ++k) {
                            knots.SetValue(k, static_cast<double>(k - 1));
                            mults.SetValue(k, 1);
                        }
                        TColStd_Array1OfReal weights(1, n);
                        for (Standard_Integer k = 1; k <= n; ++k) {
                            weights.SetValue(k, 1.0);
                        }
                        Handle(Geom_BSplineCurve) curve =
                            new Geom_BSplineCurve(
                                poles, weights, knots, mults, 2);
                        if (!curve.IsNull()) {
                            TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(curve);
                            if (!edge.IsNull()) {
                                ++s_fitPolyQuadOk;
                                return edge;
                            }
                        }
                        // Fallback: cubic interpolation through the points.
                        Handle(TColgp_HArray1OfPnt) hpts =
                            new TColgp_HArray1OfPnt(1, n);
                        for (Standard_Integer k = 0; k < n; ++k) {
                            hpts->SetValue(k + 1, poles.Value(k + 1));
                        }
                        GeomAPI_Interpolate interp(
                            hpts, Standard_False, 1e-3);
                        interp.Perform();
                        if (interp.IsDone()) {
                            curve = interp.Curve();
                            if (!curve.IsNull()) {
                                TopoDS_Edge edge =
                                    BRepBuilderAPI_MakeEdge(curve);
                                if (!edge.IsNull()) {
                                    ++s_fitPolyCubicOk;
                                    return edge;
                                }
                                ++s_fitPolyEdgeNull;
                            }
                            else {
                                ++s_fitPolyCurveNull;
                            }
                        }
                        else {
                            ++s_fitPolyNotDone;
                        }
                    }
                    catch (const Standard_Failure&) {
                        ++s_fitPolyException;
                    }
                    return TopoDS_Shape();
                };
                TopoDS_Shape smooth = trySmooth();
                if (!smooth.IsNull()) {
                    return smooth;
                }
                // Fall through to the faceted polyline below.
            }
            size_t n = e.pts.size();
            BRep_Builder B;
            TopoDS_Compound comp;
            B.MakeCompound(comp);
            for (size_t i = 0; i < n; ++i) {
                size_t j = i + 1;
                if (j >= n) {
                    if (!e.closed) {
                        break;
                    }
                    j = 0;
                }
                double b = (i < e.bulges.size()) ? e.bulges[i] : 0.0;
                TopoDS_Edge ed = makeSegEdge(e.pts[i], e.pts[j], b, M);
                if (!ed.IsNull()) {
                    B.Add(comp, ed);
                }
            }
            return comp;
        }
        case EntType::CIRCLE: {
            Vec3 c = xform(e.center, M);
            Vec3 vx = sub(xform(
                             Vec3{e.center.x + e.radius, e.center.y, e.center.z}, M),
                          c);
            Vec3 vy = sub(xform(
                             Vec3{e.center.x, e.center.y + e.radius, e.center.z}, M),
                          c);
            return makeCircleLike(c, vx, vy);
        }
        case EntType::ARC: {
            double a0 = e.startAngle;
            double a1 = e.endAngle;
            if (a1 <= a0) {
                a1 += 2.0 * PI;
            }
            double sweep = a1 - a0;
            Vec3 c = xform(e.center, M);
            if (sweep >= 2.0 * PI - 1e-6) {
                // Full circle.
                Vec3 vx = sub(xform(Vec3{e.center.x + e.radius, e.center.y,
                                         e.center.z},
                                    M),
                              c);
                Vec3 vy = sub(xform(Vec3{e.center.x, e.center.y + e.radius,
                                         e.center.z},
                                    M),
                              c);
                return makeCircleLike(c, vx, vy);
            }
            // Three-point arc: start, midpoint, end.  Robust under any
            // affine transform (exact circle for uniform transforms).
            double amid = 0.5 * (a0 + a1);
            Vec3 p0 = {e.center.x + e.radius * std::cos(a0),
                       e.center.y + e.radius * std::sin(a0), e.center.z};
            Vec3 pm = {e.center.x + e.radius * std::cos(amid),
                       e.center.y + e.radius * std::sin(amid), e.center.z};
            Vec3 p1 = {e.center.x + e.radius * std::cos(a1),
                       e.center.y + e.radius * std::sin(a1), e.center.z};
            Vec3 a = xform(p0, M);
            Vec3 q = xform(pm, M);
            Vec3 b = xform(p1, M);
            GC_MakeArcOfCircle mk(gp_Pnt(a.x, a.y, a.z), gp_Pnt(q.x, q.y, q.z),
                                  gp_Pnt(b.x, b.y, b.z));
            if (mk.IsDone()) {
                return BRepBuilderAPI_MakeEdge(mk.Value());
            }
            BRepBuilderAPI_MakeEdge mk2(gp_Pnt(a.x, a.y, a.z),
                                        gp_Pnt(b.x, b.y, b.z));
            if (mk2.IsDone()) {
                return mk2.Edge();
            }
            return TopoDS_Shape();
        }
        case EntType::ELLIPSE: {
            Vec3 c = xform(e.center, M);
            Vec3 sm = sub(xform(Vec3{e.center.x + e.smAxis.x,
                                     e.center.y + e.smAxis.y,
                                     e.center.z + e.smAxis.z},
                                M),
                          c);
            double a = norm(sm);
            if (a < 1e-9) {
                return TopoDS_Shape();
            }
            // Local minor axis = sm_axis rotated 90 deg CCW * axis_ratio.
            double mx = -e.smAxis.y * e.axisRatio;
            double my = e.smAxis.x * e.axisRatio;
            Vec3 mn = sub(xform(Vec3{e.center.x + mx, e.center.y + my,
                                     e.center.z},
                                M),
                          c);
            double b = norm(mn);
            if (b < 1e-9) {
                return TopoDS_Shape();
            }
            Vec3 n = cross(sm, mn);
            double nn = norm(n);
            if (nn < 1e-12) {
                return TopoDS_Shape();
            }
            Vec3 un = {n.x / nn, n.y / nn, n.z / nn};
            // Major axis must be the longer one for gp_Elips.
            Vec3 ex = {sm.x / a, sm.y / a, sm.z / a};
            if (b > a) {
                std::swap(a, b);  // a = |mn|, b = |sm|
                ex = {mn.x / a, mn.y / a, mn.z / a};
            }
            gp_Ax2 ax(gp_Pnt(c.x, c.y, c.z), gp_Dir(un.x, un.y, un.z),
                      gp_Dir(ex.x, ex.y, ex.z));
            double s = e.startAngle;
            double e2 = e.endAngle;
            if (e2 <= s) {
                e2 += 2.0 * PI;
            }
            if (e2 - s >= 2.0 * PI - 1e-6) {
                return BRepBuilderAPI_MakeEdge(gp_Elips(ax, a, b));
            }
            return BRepBuilderAPI_MakeEdge(gp_Elips(ax, a, b), s, e2);
        }
        case EntType::POINT: {
            if (e.pts.empty()) {
                return TopoDS_Shape();
            }
            Vec3 p = xform(e.pts[0], M);
            return BRepBuilderAPI_MakeVertex(gp_Pnt(p.x, p.y, p.z));
        }
        case EntType::TEXT:
        case EntType::MTEXT: {
            if (e.pts.empty()) {
                return TopoDS_Shape();
            }
            // The pure C++ importer has no font/glyph engine, so instead of a
            // real string outline we draw a clearly visible bounding rectangle
            // (height = text height, width estimated from the string length)
            // with two diagonals marking the insertion point. The actual text
            // is kept in the object Label.
            double h = (e.textHeight > 1e-9) ? e.textHeight : 2.5;
            double w = 0.6 * h * (double)e.text.size();
            if (w < h) {
                w = h;
            }
            Vec3 o = xform(e.pts[0], M);
            double cr = std::cos(e.rotation);
            double sr = std::sin(e.rotation);
            // Text-local axes (rotation about Z) transformed through M so the
            // box follows INSERT scaling and rotation.
            Vec3 u = sub(xform(Vec3{e.pts[0].x + cr, e.pts[0].y + sr,
                                    e.pts[0].z},
                               M),
                         o);
            Vec3 v = sub(xform(Vec3{e.pts[0].x - sr, e.pts[0].y + cr,
                                    e.pts[0].z},
                               M),
                         o);
            if (norm(u) < 1e-9 || norm(v) < 1e-9) {
                u = Vec3{w, 0.0, 0.0};
                v = Vec3{0.0, h, 0.0};
            }
            Vec3 c0 = o;
            Vec3 c1 = add(o, scale(u, w));
            Vec3 c2 = add(add(o, scale(u, w)), scale(v, h));
            Vec3 c3 = add(o, scale(v, h));
            BRep_Builder B;
            TopoDS_Compound comp;
            B.MakeCompound(comp);
            auto addEdge = [&](const Vec3& a, const Vec3& b) {
                BRepBuilderAPI_MakeEdge me(gp_Pnt(a.x, a.y, a.z),
                                           gp_Pnt(b.x, b.y, b.z));
                if (me.IsDone()) {
                    B.Add(comp, me.Edge());
                }
            };
            addEdge(c0, c1);
            addEdge(c1, c2);
            addEdge(c2, c3);
            addEdge(c3, c0);
            addEdge(c0, c2);  // diagonals mark the anchor point
            addEdge(c1, c3);
            return comp;
        }
        case EntType::LEADER: {
            if (e.pts.size() < 2) {
                return TopoDS_Shape();
            }
            // Diagnostics for the "elements are missing" reports.  LEADER used
            // to be dropped whole (unsupported fixedtype 0x45); the vertices
            // alone do not say which end carries the arrow, so report every
            // field the interpretation rests on.  Capped rather than keyed by
            // signature: all leaders of a drawing share layer and style, and
            // the individual vertex lists are what has to be checked.
            {
                static int s_logged = 0;
                if (s_logged < 12) {
                    ++s_logged;
                    std::ostringstream lg;
                    lg << "[SWDWG] leader layer='" << e.layer << "' dimstyle='"
                       << e.dimStyleName << "' path=" << e.leaderPathType
                       << " arrow=" << (e.leaderArrow ? 1 : 0) << " type="
                       << e.leaderArrowType << " asz=" << e.leaderArrowSize
                       << " hook=" << (e.leaderHook ? 1 : 0) << "/"
                       << (e.leaderHookX ? 1 : 0) << " n=" << e.pts.size()
                       << " box=" << e.leaderBoxWidth << "x"
                       << e.leaderBoxHeight << " insptOff=("
                       << e.leaderInsptOffset.x << ","
                       << e.leaderInsptOffset.y << ") endproj=("
                       << e.leaderEndProj.x << "," << e.leaderEndProj.y
                       << ") pts=";
                    for (std::size_t i = 0; i < e.pts.size() && i < 8; ++i) {
                        lg << "(" << e.pts[i].x << "," << e.pts[i].y << ")";
                    }
                    lg << "\n";
                    Base::Console().message("%s", lg.str().c_str());
                }
            }
            // A LEADER is plain line work: one edge per segment, plus a filled
            // arrow head at the first vertex - the end that touches the
            // annotated geometry, while the annotation itself sits at the other
            // end (that is where AutoCAD also draws the hook line).
            BRep_Builder B;
            TopoDS_Compound comp;
            B.MakeCompound(comp);
            auto addSeg = [&](const Vec3& a, const Vec3& b) {
                Vec3 p = xform(a, M);
                Vec3 q = xform(b, M);
                BRepBuilderAPI_MakeEdge me(gp_Pnt(p.x, p.y, p.z),
                                           gp_Pnt(q.x, q.y, q.z));
                if (me.IsDone()) {
                    B.Add(comp, me.Edge());
                }
            };
            for (std::size_t i = 1; i < e.pts.size(); ++i) {
                addSeg(e.pts[i - 1], e.pts[i]);
            }
            if (e.leaderArrow && e.leaderArrowSize > 1e-9) {
                // Closed-filled arrow, same geometry as the dimension arrow
                // heads: length = DIMASZ (the entity carries its own copy),
                // half-width ratio 0.25 (4:1 arrow, the built-in symbol).
                // Geometry is built in the leader's own 2D plane and then
                // transformed, so a tilted extrusion comes out right.
                constexpr double kHalfWidthRatio = 0.25;
                const double asz = e.leaderArrowSize;
                Vec3 tip = e.pts.front();
                Vec3 d = sub(e.pts[1], tip);
                const double dl = norm(d);
                if (dl > 1e-9) {
                    Vec3 dir = scale(d, 1.0 / dl);  // tip -> base
                    Vec3 nrm{-dir.y, dir.x, 0.0};   // 2D normal in the plane
                    Vec3 base = add(tip, scale(dir, asz));
                    Vec3 half = scale(nrm, asz * kHalfWidthRatio);
                    BRepBuilderAPI_MakePolygon mk;
                    Vec3 tri[3] = {tip, add(base, half), sub(base, half)};
                    bool ok = true;
                    for (const Vec3& t : tri) {
                        Vec3 q = xform(t, M);
                        if (!std::isfinite(q.x) || !std::isfinite(q.y)
                            || !std::isfinite(q.z)) {
                            ok = false;
                            break;
                        }
                        mk.Add(gp_Pnt(q.x, q.y, q.z));
                    }
                    if (ok) {
                        mk.Close();
                        if (mk.IsDone()) {
                            BRepBuilderAPI_MakeFace f(mk.Wire(),
                                                      /*onlyPlane=*/true);
                            if (f.IsDone()) {
                                B.Add(comp, f.Face());
                            }
                        }
                    }
                }
            }
            return comp;
        }
        case EntType::SOLID:
        case EntType::FACE3D: {
            if (e.pts.size() < 3) {
                return TopoDS_Shape();
            }
            BRepBuilderAPI_MakePolygon mk;
            for (const Vec3& p : e.pts) {
                Vec3 q = xform(p, M);
                mk.Add(gp_Pnt(q.x, q.y, q.z));
            }
            mk.Close();
            if (!mk.IsDone()) {
                return TopoDS_Shape();
            }
            TopoDS_Wire w = mk.Wire();
            BRepBuilderAPI_MakeFace f(w, /*onlyPlane=*/true);
            if (f.IsDone()) {
                return f.Face();
            }
            return w;  // non-planar: keep the wire
        }
        case EntType::SPLINE: {
            const std::vector<Vec3>& src = e.fitPts.empty() ? e.pts : e.fitPts;
            if (src.size() < 2) {
                return TopoDS_Shape();
            }
            // Interpolate a NURBS curve only for moderate point counts; on huge
            // drawings thousands of dense splines would otherwise consume
            // excessive memory. Large splines fall back to a polyline, which
            // is always visible and cheap.
            if (src.size() <= 100) {
                try {
                    Handle(TColgp_HArray1OfPnt) arr =
                        new TColgp_HArray1OfPnt(1, (int)src.size());
                    for (size_t k = 0; k < src.size(); ++k) {
                        Vec3 p = xform(src[k], M);
                        arr->SetValue((int)k + 1, gp_Pnt(p.x, p.y, p.z));
                    }
                    GeomAPI_Interpolate interp(arr, e.splineClosed, 1e-7);
                    interp.Perform();
                    return BRepBuilderAPI_MakeEdge(interp.Curve());
                }
                catch (const Standard_Failure&) {
                    // fall back to a polygon through the points
                }
            }
            // 导出功能 BEGIN（导入修复）：多段线的顶点里常有重合点（相邻点完全
            // 相同，或闭合线首尾重复），BRepBuilderAPI_MakePolygon 会用它造出
            // 零长度边，得到的 wire 是**无效拓扑**；导出时遍历它的边会让 OCC
            // 读到坏数据直接崩溃（用户报告的"导出报错 + FreeCAD 退出"）。
            // 这里先去掉重合点，再判断闭合，保证造出来的 wire 有效。
            std::vector<gp_Pnt> poly;
            poly.reserve(src.size());
            for (size_t k = 0; k < src.size(); ++k) {
                const Vec3 p = xform(src[k], M);
                const gp_Pnt q(p.x, p.y, p.z);
                if (!poly.empty() && q.Distance(poly.back()) <= 1e-9) {
                    continue;
                }
                poly.push_back(q);
            }
            if (e.splineClosed && poly.size() > 1
                && poly.front().Distance(poly.back()) <= 1e-9) {
                poly.pop_back();  // 闭合线不重复首点，否则 Close() 又是零长度边
            }
            BRepBuilderAPI_MakePolygon mk;
            for (const gp_Pnt& q : poly) {
                mk.Add(q);
            }
            if (poly.size() < 2) {
                return TopoDS_Shape();
            }
            if (e.splineClosed) {
                mk.Close();
            }
            if (mk.IsDone()) {
                return mk.Wire();
            }
            return TopoDS_Shape();
            // 导出功能 END
        }
        case EntType::HATCH: {
            // Rebuild the hatch: one wire per boundary loop plus (for solid
            // fills) a face per loop, or (for pattern fills) the pattern
            // lines clipped to the boundary loops.
            const double z = e.elevation;
            BRep_Builder B;
            TopoDS_Compound comp;
            B.MakeCompound(comp);
            bool any = false;

            // Boundary loops -> (a) one wire per loop in model space, and
            // (b) local 2D outline polygons, separated into outer loops and
            //     inner holes for pattern clipping.
            std::vector<std::vector<Vec3>> outerPolys;
            std::vector<std::vector<Vec3>> innerPolys;
            std::vector<TopoDS_Shape> pathShapes;
            for (const HatchPath& hp : e.hatchPaths) {
                std::vector<TopoDS_Edge> edges;
                std::vector<Vec3> poly;  // local XY outline (z ignored)
                if (hp.isPolyline) {
                    // Polyline path: vertices + bulges.
                    const size_t n = hp.polyPts.size();
                    for (size_t i = 0; i < n; ++i) {
                        size_t j = i + 1;
                        if (j >= n) {
                            if (!hp.closed) {
                                break;
                            }
                            j = 0;
                        }
                        double b = (i < hp.polyBulges.size())
                                       ? hp.polyBulges[i]
                                       : 0.0;
                        TopoDS_Edge ed =
                            makeSegEdge(hp.polyPts[i], hp.polyPts[j], b, M);
                        if (!ed.IsNull()) {
                            edges.push_back(ed);
                        }
                        if (std::fabs(b) > 1e-9) {
                            // Sample the bulge arc for the clipping outline.
                            // We add pts[i] as the first sample, then N-1
                            // intermediate points along the true arc, and
                            // finally pts[j] - the last edge's last sample
                            // closes the loop back to pts[0].  Sampling
                            // only the apex (old behaviour) left the clip
                            // polygon far from the arc, producing burrs
                            // where hatch lines crossed the true curve.
                            Vec3 v = {hp.polyPts[j].x - hp.polyPts[i].x,
                                      hp.polyPts[j].y - hp.polyPts[i].y, 0.0};
                            double d = std::hypot(v.x, v.y);
                            if (d > 1e-12) {
                                double bAbs = std::fabs(b);
                                double sign = (b > 0.0) ? 1.0 : -1.0;
                                double theta = 4.0 * std::atan(bAbs);
                                double r =
                                    d / (2.0 * std::sin(theta * 0.5));
                                double sag = 0.5 * d * bAbs;
                                Vec3 mid = {(hp.polyPts[i].x
                                             + hp.polyPts[j].x)
                                                * 0.5,
                                            (hp.polyPts[i].y
                                             + hp.polyPts[j].y)
                                                * 0.5,
                                            0.0};
                                // RIGHT perpendicular = apex direction
                                // for b > 0 (per DWG reference).
                                Vec3 perp = {v.y / d, -v.x / d, 0.0};
                                // Arc center sits on the OPPOSITE side of
                                // the apex from `perp`, at distance (r - sag)
                                // from the chord midpoint along that axis.
                                double dist = r - sag;
                                Vec3 center = {mid.x - perp.x * sign * dist,
                                               mid.y - perp.y * sign * dist,
                                               0.0};
                                double a0 = std::atan2(
                                    hp.polyPts[i].y - center.y,
                                    hp.polyPts[i].x - center.x);
                                double a1 = std::atan2(
                                    hp.polyPts[j].y - center.y,
                                    hp.polyPts[j].x - center.x);
                                if (sign > 0.0) {  // CCW (b > 0)
                                    while (a1 <= a0) {
                                        a1 += 2.0 * M_PI;
                                    }
                                }
                                else {  // CW (b < 0)
                                    while (a1 >= a0) {
                                        a1 -= 2.0 * M_PI;
                                    }
                                }
                                double arcLen = r * std::fabs(a1 - a0);
                                int nSamples = std::max(
                                    2,
                                    static_cast<int>(std::ceil(
                                        arcLen * 8.0 / d)));
                                poly.push_back(hp.polyPts[i]);
                                for (int k = 1; k < nSamples; ++k) {
                                    double t =
                                        static_cast<double>(k) / nSamples;
                                    double alpha = a0 + t * (a1 - a0);
                                    poly.push_back(Vec3{
                                        center.x + r * std::cos(alpha),
                                        center.y + r * std::sin(alpha),
                                        0.0});
                                }
                                poly.push_back(hp.polyPts[j]);
                            }
                            else {
                                poly.push_back(hp.polyPts[i]);
                            }
                        }
                        else {
                            poly.push_back(hp.polyPts[i]);
                        }
                    }
                }
                else {
                    for (const HatchSeg& s : hp.segs) {
                        switch (s.curveType) {
                            case 1: {  // LINE
                                Vec3 a = xform(s.p1, M);
                                Vec3 b = xform(s.p2, M);
                                BRepBuilderAPI_MakeEdge me(
                                    gp_Pnt(a.x, a.y, a.z),
                                    gp_Pnt(b.x, b.y, b.z));
                                if (me.IsDone()) {
                                    edges.push_back(me.Edge());
                                }
                                poly.push_back(Vec3{s.p1.x, s.p1.y, 0.0});
                                break;
                            }
                            case 2: {  // CIRCULAR ARC (three-point rebuild)
                                // The DWG stores the arc's two angles plus a
                                // direction bit.  For a clockwise edge (ccw =
                                // 0) the angles are measured in a mirrored
                                // frame: on the rounded boundary below, the
                                // stored a1 = 1.855634 rad placed the arc at
                                // y = 2884653, while its own line edge ends at
                                // (433747.303, 2884204.777) - 448 units away.
                                // Negating the angles puts that end exactly on
                                // (433747.0, 2884205.0), i.e. on the line.
                                // Without this the boundary arc floats ~450
                                // units off, the boundary polygon spans that
                                // gap and the hatch sweeps tens of thousands of
                                // pattern lines (see docs/29).
                                double a0 = s.ccw ? s.startAngle : -s.startAngle;
                                double a1 = s.ccw ? s.endAngle : -s.endAngle;
                                // A hatch boundary arc is the short way round
                                // between its two angles.  The per-edge
                                // direction flag (`is_ccw`) is NOT usable as
                                // the discriminator: on one rounded rectangle
                                // both arcs come back with the same
                                // (increasing) angle order but opposite flags,
                                // and honouring the flag swaps one of them,
                                // turning a 7.5 degree fillet into a 352
                                // degree near-circle - which is what made the
                                // boundary span ~460 units and the hatch sweep
                                // ~80 000 pattern lines instead of a few
                                // hundred (see docs/29).  Normalising the
                                // difference into (-pi, pi] picks the filament
                                // the drawing actually has, whatever the flag
                                // says.
                                double sweep = a1 - a0;
                                while (sweep > PI) {
                                    sweep -= 2.0 * PI;
                                }
                                while (sweep < -PI) {
                                    sweep += 2.0 * PI;
                                }
                                double amid = a0 + 0.5 * sweep;
                                Vec3 p0 = {s.center.x
                                               + s.radius * std::cos(a0),
                                           s.center.y
                                               + s.radius * std::sin(a0),
                                           z};
                                Vec3 pm = {s.center.x
                                               + s.radius * std::cos(amid),
                                           s.center.y
                                               + s.radius * std::sin(amid),
                                           z};
                                Vec3 p1 = {s.center.x
                                               + s.radius
                                                     * std::cos(a0 + sweep),
                                           s.center.y
                                               + s.radius
                                                     * std::sin(a0 + sweep),
                                           z};
                                Vec3 a = xform(p0, M);
                                Vec3 q = xform(pm, M);
                                Vec3 b = xform(p1, M);
                                GC_MakeArcOfCircle mk(
                                    gp_Pnt(a.x, a.y, a.z),
                                    gp_Pnt(q.x, q.y, q.z),
                                    gp_Pnt(b.x, b.y, b.z));
                                if (mk.IsDone()) {
                                    edges.push_back(
                                        BRepBuilderAPI_MakeEdge(
                                            mk.Value())
                                            .Edge());
                                }
                                else {
                                    BRepBuilderAPI_MakeEdge me(
                                        gp_Pnt(a.x, a.y, a.z),
                                        gp_Pnt(b.x, b.y, b.z));
                                    if (me.IsDone()) {
                                        edges.push_back(me.Edge());
                                    }
                                }
                                // Sample the arc for the clipping outline.
                                const int NS = 16;
                                for (int k = 0; k <= NS; ++k) {
                                    double ang =
                                        a0 + sweep * (double)k / NS;
                                    poly.push_back(Vec3{
                                        s.center.x
                                            + s.radius * std::cos(ang),
                                        s.center.y
                                            + s.radius * std::sin(ang),
                                        0.0});
                                }
                                break;
                            }
                            case 3: {  // ELLIPTICAL ARC -> discretised
                                double maL = std::hypot(s.endpoint.x,
                                                        s.endpoint.y);
                                if (maL < 1e-12) {
                                    break;
                                }
                                double ratio =
                                    std::clamp(s.ratio, 1e-3, 1.0);
                                Vec3 mn{-s.endpoint.y * ratio,
                                         s.endpoint.x * ratio, 0.0};
                                double a0 = s.startAngle;
                                double a1 = s.endAngle;
                                // Same reasoning as the circular arc above:
                                // the per-edge direction flag is not a usable
                                // discriminator, so take the short way round.
                                double sweep = a1 - a0;
                                while (sweep > PI) {
                                    sweep -= 2.0 * PI;
                                }
                                while (sweep < -PI) {
                                    sweep += 2.0 * PI;
                                }
                                if (sweep > 2.0 * PI - 1e-6) {
                                    sweep = 2.0 * PI - 1e-6;
                                }
                                const int NE = 16;
                                Vec3 prev;
                                bool havePrev = false;
                                for (int k = 0; k <= NE; ++k) {
                                    double t = a0 + sweep * (double)k / NE;
                                    Vec3 p = {s.center.x
                                                  + s.endpoint.x
                                                        * std::cos(t)
                                                  + mn.x * std::sin(t),
                                              s.center.y
                                                  + s.endpoint.y
                                                        * std::cos(t)
                                                  + mn.y * std::sin(t),
                                              z};
                                    if (havePrev) {
                                        Vec3 a = xform(prev, M);
                                        Vec3 b = xform(p, M);
                                        BRepBuilderAPI_MakeEdge me(
                                            gp_Pnt(a.x, a.y, a.z),
                                            gp_Pnt(b.x, b.y, b.z));
                                        if (me.IsDone()) {
                                            edges.push_back(me.Edge());
                                        }
                                    }
                                    poly.push_back(
                                        Vec3{p.x, p.y, 0.0});
                                    prev = p;
                                    havePrev = true;
                                }
                                break;
                            }
                            case 4: {  // SPLINE -> discretised polyline
                                const std::vector<Vec3>& src =
                                    s.fitPts.empty() ? s.ctrlPts : s.fitPts;
                                if (src.size() < 2) {
                                    break;
                                }
                                for (size_t k = 0; k + 1 < src.size();
                                     ++k) {
                                    Vec3 a = xform(src[k], M);
                                    Vec3 b = xform(src[k + 1], M);
                                    BRepBuilderAPI_MakeEdge me(
                                        gp_Pnt(a.x, a.y, a.z),
                                        gp_Pnt(b.x, b.y, b.z));
                                    if (me.IsDone()) {
                                        edges.push_back(me.Edge());
                                    }
                                    poly.push_back(
                                        Vec3{src[k].x, src[k].y, 0.0});
                                }
                                poly.push_back(Vec3{src.back().x,
                                                    src.back().y, 0.0});
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
                if (!edges.empty()) {
                    BRepBuilderAPI_MakeWire mw;
                    for (const TopoDS_Edge& ed : edges) {
                        mw.Add(ed);
                    }
                    if (mw.IsDone()) {
                        pathShapes.push_back(mw.Wire());
                    }
                    else {
                        TopoDS_Compound c;
                        B.MakeCompound(c);
                        for (const TopoDS_Edge& ed : edges) {
                            B.Add(c, ed);
                        }
                        pathShapes.push_back(c);
                    }
                }
                if (!poly.empty()) {
                    if (hp.closed
                        && (poly.front().x != poly.back().x
                            || poly.front().y != poly.back().y)) {
                        poly.push_back(poly.front());  // close the loop
                    }
                    if (hp.isOuter) {
                        outerPolys.push_back(std::move(poly));
                    }
                    else {
                        innerPolys.push_back(std::move(poly));
                    }
                }
            }

            // The pattern name is the text before the first comma - that is how
            // a .pat header reads ("*NAME, description") and it is what AutoCAD
            // resolves against its pattern list.  Files in the wild keep the
            // comma and the description inside the same string: measured
            // "SOLID,_O" on 88 hatches, which matched no built-in name and used
            // to fall through to the boundary-only fallback.
            std::string pn = e.hatchPattern;
            const std::size_t comma = pn.find(',');
            if (comma != std::string::npos) {
                pn.erase(comma);
            }
            const std::size_t first = pn.find_first_not_of(" \t");
            if (first == std::string::npos) {
                pn.clear();
            }
            else {
                pn.erase(0, first);
                while (!pn.empty()
                       && std::isspace(static_cast<unsigned char>(pn.back()))) {
                    pn.pop_back();
                }
            }
            std::transform(pn.begin(), pn.end(), pn.begin(),
                           [](unsigned char c) {
                               return (char)std::toupper(c);
                           });
            // Solid fill = either the solid flag or the "SOLID" pattern.  The
            // name half only works once the name has been cut at the comma:
            // these files store "SOLID,_O", which compared unequal to "SOLID"
            // and left the whole fill out - 88 hatches on one test drawing.
            // Their is_solid_fill is 1, so the flag half was never the problem
            // (verified by A/B: with the name cut and this expression left
            // untouched, all 88 come out solid); the name half simply never
            // matched.
            bool solid = e.hatchSolid && (pn.empty() || pn == "SOLID");
            std::vector<HatchDefLine> deflines = e.hatchDefLines;
            bool deflinesFromDwg = !deflines.empty();
            if (!solid && deflines.empty() && !pn.empty() && pn != "SOLID") {
                deflines = builtinHatchPattern(pn);
            }
            // DWG stores defline values already scaled to model space;
            // built-in fallback definitions are still in raw pattern units.
            if (!deflinesFromDwg && !deflines.empty()) {
                for (HatchDefLine& dl : deflines) {
                    dl.pt0.x *= e.hatchScale;
                    dl.pt0.y *= e.hatchScale;
                    dl.offset.x *= e.hatchScale;
                    dl.offset.y *= e.hatchScale;
                    for (double& d : dl.dashes) {
                        d *= e.hatchScale;
                    }
                }
            }

            // Diagnostics for "the fill is missing" reports.  A non-solid hatch
            // that ends up with no pattern definition at all can only be drawn
            // as its boundary outline, which looks like a dropped element (the
            // boundary is usually already there as ordinary line work).
            // Console warnings are not gated by sw_debug, and the report is
            // rate limited to one line per distinct signature so a drawing with
            // hundreds of identical hatches stays readable.
            {
                static std::set<std::string> s_seen;
                static int s_dropped = 0;
                const bool fillMissing =
                    (!solid && deflines.empty() && !e.hatchPattern.empty());
                // Boundary extent: a boundary that came out far larger than the
                // area the hatch is meant to cover is the signature of a
                // mis-built boundary edge, and it shows up as a pattern sweep
                // thousands of lines wide (see docs/29 and docs/32).
                double bx0 = 1e30, bx1 = -1e30, by0 = 1e30, by1 = -1e30;
                auto grow = [&](const Vec3& v) {
                    bx0 = std::min(bx0, v.x);
                    bx1 = std::max(bx1, v.x);
                    by0 = std::min(by0, v.y);
                    by1 = std::max(by1, v.y);
                };
                // Grow a box from one boundary segment using only the fields
                // that segment type actually stores.  A circular-arc segment
                // (type 2) carries centre/radius/angles and leaves `endpoint`
                // zero - that field is the ELLIPSE's major-axis end - so
                // reading it for an arc drags the box to the drawing origin.
                // Measured on the 辅道 road plan: every ARC segment of an
                // ordinary road-area hatch came back with endpoint (0,0) and
                // stretched its extent to 434049 x 2884665 units, and the
                // pattern sweep then covered that whole box.  One helper so the
                // production box and the diagnostic below cannot drift apart.
                auto growSeg = [](double& x0, double& x1, double& y0, double& y1,
                                  const HatchSeg& s) {
                    auto g = [&](const Vec3& v) {
                        x0 = std::min(x0, v.x);
                        x1 = std::max(x1, v.x);
                        y0 = std::min(y0, v.y);
                        y1 = std::max(y1, v.y);
                    };
                    switch (s.curveType) {
                        case 1:  // LINE: both endpoints are real
                            g(s.p1);
                            g(s.p2);
                            break;
                        case 2:  // CIRCULAR ARC: radius is a length
                            g(Vec3{s.center.x - s.radius,
                                   s.center.y - s.radius, 0.0});
                            g(Vec3{s.center.x + s.radius,
                                   s.center.y + s.radius, 0.0});
                            break;
                        case 3:  // ELLIPSE: radius is the minor/major RATIO
                            g(s.endpoint);
                            g(Vec3{2.0 * s.center.x - s.endpoint.x,
                                   2.0 * s.center.y - s.endpoint.y, 0.0});
                            g(Vec3{s.center.x - std::fabs(s.endpoint.x
                                                          - s.center.x),
                                   s.center.y - std::fabs(s.endpoint.y
                                                          - s.center.y), 0.0});
                            g(Vec3{s.center.x + std::fabs(s.endpoint.x
                                                          - s.center.x),
                                   s.center.y + std::fabs(s.endpoint.y
                                                          - s.center.y), 0.0});
                            break;
                        case 4:  // SPLINE control / fit points
                            for (const Vec3& v : s.ctrlPts) {
                                g(v);
                            }
                            for (const Vec3& v : s.fitPts) {
                                g(v);
                            }
                            break;
                        default:
                            break;
                    }
                };
                bool allPolyline = !e.hatchPaths.empty();
                for (const HatchPath& hp : e.hatchPaths) {
                    if (!hp.isPolyline) {
                        allPolyline = false;
                    }
                }
                for (const HatchPath& hp : e.hatchPaths) {
                    for (const Vec3& v : hp.polyPts) {
                        grow(v);
                    }
                    for (const HatchSeg& s : hp.segs) {
                        growSeg(bx0, bx1, by0, by1, s);
                    }
                }
                // Grouped by *kind*, not by location: the boundary box is a
                // per-entity property, and putting it in the key turned every
                // hatch into its own signature and burned the report budget on
                // the first 15 of them.
                std::ostringstream sig;
                sig << e.hatchPattern << '|' << e.hatchPatternType << '|'
                    << e.hatchDefLines.size() << '|' << e.hatchPaths.size()
                    << '|' << e.hatchScale << '|' << e.hatchAngle << '|'
                    << (solid ? 1 : 0) << '|' << (e.hatchGradient ? 1 : 0)
                    << '|' << e.hatchGradientColors;
                if (s_seen.size() < 15 && s_seen.insert(sig.str()).second) {
                    // The name goes out hex-escaped as well: a pattern name
                    // that looks like two names stuck together ("SOLID,_O") is
                    // the signature of a decoding problem, not of a drawing
                    // that really contains such a pattern.
                    std::string hex;
                    for (unsigned char ch : e.hatchPattern) {
                        char b[4];
                        std::snprintf(b, sizeof(b), "%02X ", ch);
                        hex += b;
                    }
                    Base::Console().message(
                        "[SWDWG] hatch '%s' [%s] layer='%s' type=%d deflines=%zu "
                        "paths=%zu scale=%.6g angle=%.6g solid=%d grad=%d/%d "
                        "'%s' aci=%d rgb=0x%08lX "
                        "boundary x=[%.0f,%.0f] y=[%.0f,%.0f] (%s) -> "
                        "pattern lines: %s\n",
                        e.hatchPattern.c_str(), hex.c_str(), e.layer.c_str(),
                        e.hatchPatternType, e.hatchDefLines.size(),
                        e.hatchPaths.size(), e.hatchScale, e.hatchAngle,
                        solid ? 1 : 0, e.hatchGradient ? 1 : 0,
                        e.hatchGradientColors, e.hatchGradientName.c_str(),
                        e.hatchGradientAci, e.hatchGradientRgb, bx0, bx1, by0,
                        by1, allPolyline ? "all polyline paths" : "edge paths",
                        fillMissing ? "NONE (boundary outline only)"
                                    : (solid ? "solid fill" : "yes"));
                }
                // A pattern hatch whose boundary came out far larger than the
                // area it should cover sweeps its line families across that
                // whole box, which lands thousands of dashes somewhere else in
                // the drawing.  docs/29 fixed the boundary arc that caused one
                // of these; this reports the next one by name and size.
                const double spanX = bx1 - bx0;
                const double spanY = by1 - by0;
                static int s_wide = 0;
                // Applies to solid fills too: a face spanning tens of thousands
                // of units in a drawing measured in hundreds is a mis-built
                // boundary path (one was measured at 63695 x 44991 because a
                // single path landed at the origin while its siblings sat at
                // the block's absolute coordinates).
                if (std::max(spanX, spanY) > 100000.0 && ++s_wide <= 20) {
                    Base::Console().warning(
                        "[SWDWG] hatch '%s' layer='%s': boundary spans "
                        "%.0f x %.0f units (paths=%zu), pattern sweep will "
                        "cover that whole box\n",
                        e.hatchPattern.c_str(), e.layer.c_str(), spanX, spanY,
                        e.hatchPaths.size());
                    // Per-path extents and, for small polyline paths, the raw
                    // points: one path sitting at the origin while its siblings
                    // carry the (block-)absolute coordinates is what makes the
                    // box explode, and this says which path does it.  Both the
                    // report and this dump use the same 100000-unit gate: a
                    // 5000-unit reinforcement hatch or a 5100-unit strip is
                    // genuine line work (the 辅道 plan has both), while the
                    // origin-drag cases measured 430000 x 2884000.
                    const bool dumpPaths =
                        std::max(spanX, spanY) >= 100000.0;
                    for (std::size_t pi = 0;
                         dumpPaths && pi < e.hatchPaths.size(); ++pi) {
                        const HatchPath& pth = e.hatchPaths[pi];
                        double px0 = 1e300, py0 = 1e300;
                        double px1 = -1e300, py1 = -1e300;
                        std::size_t n = 0;
                        auto acc = [&](const Vec3& v) {
                            px0 = std::min(px0, v.x);
                            px1 = std::max(px1, v.x);
                            py0 = std::min(py0, v.y);
                            py1 = std::max(py1, v.y);
                            ++n;
                        };
                        // Same helper the production box uses, so this
                        // diagnostic cannot invent a zero of its own.
                        for (const Vec3& v : pth.polyPts) {
                            acc(v);
                        }
                        for (const HatchSeg& s : pth.segs) {
                            growSeg(px0, px1, py0, py1, s);
                            if (s.curveType == 1) {
                                ++n;
                            }
                            else if (s.curveType == 2 || s.curveType == 3) {
                                n += 2;
                            }
                            else {
                                n += s.ctrlPts.size() + s.fitPts.size();
                            }
                        }
                        if (n == 0) {
                            continue;
                        }
                        Base::Console().warning(
                            "[SWDWG]   path[%zu] %s pts=%zu bbox=[%.2f,%.2f]-"
                            "[%.2f,%.2f] r=%.0f\n",
                            pi, pth.isPolyline ? "polyline" : "edges", n,
                            px0, py0, px1, py1,
                            std::hypot(px1 - px0, py1 - py0));
                        // Raw fields of the path that reaches the origin: which
                        // segment type carries the zero tells us whether it is
                        // a union default or real file data.
                        if (std::min(std::fabs(px0), std::fabs(py0)) < 1.0) {
                            if (pth.isPolyline) {
                                std::string pts;
                                for (std::size_t k = 0;
                                     k < pth.polyPts.size() && k < 10; ++k) {
                                    char buf[48];
                                    std::snprintf(buf, sizeof(buf),
                                                  "(%.2f,%.2f) ", pth.polyPts[k].x,
                                                  pth.polyPts[k].y);
                                    pts += buf;
                                }
                                Base::Console().warning(
                                    "[SWDWG]     polyPts: %s\n", pts.c_str());
                            }
                            for (std::size_t k = 0;
                                 k < pth.segs.size() && k < 10; ++k) {
                                const HatchSeg& s = pth.segs[k];
                                Base::Console().warning(
                                    "[SWDWG]     seg[%zu] type=%d p1=(%.2f,"
                                    "%.2f) p2=(%.2f,%.2f) c=(%.2f,%.2f) r=%.2f "
                                    "end=(%.2f,%.2f) ctrl=%zu fit=%zu\n",
                                    k, s.curveType, s.p1.x, s.p1.y, s.p2.x,
                                    s.p2.y, s.center.x, s.center.y, s.radius,
                                    s.endpoint.x, s.endpoint.y,
                                    s.ctrlPts.size(), s.fitPts.size());
                            }
                        }
                    }
                }
                if (fillMissing) {
                    ++s_dropped;
                    if (s_dropped <= 10) {
                        Base::Console().warning(
                            "[SWDWG] dropped hatch #%d layer='%s' pattern='%s' "
                            "(type=%d)\n",
                            s_dropped, e.layer.c_str(), e.hatchPattern.c_str(),
                            e.hatchPatternType);
                    }
                    else if (s_dropped == 11) {
                        Base::Console().warning(
                            "[SWDWG] ...%d dropped hatches so far, further "
                            "reports suppressed\n",
                            s_dropped);
                    }
                }
            }

            if (solid) {
                // Separate outer and inner boundary wires; build faces with
                // holes so island/void loops are not filled.
                std::vector<TopoDS_Wire> outerWires;
                std::vector<TopoDS_Wire> innerWires;
                for (size_t idx = 0; idx < pathShapes.size(); ++idx) {
                    const TopoDS_Shape& ps = pathShapes[idx];
                    if (ps.ShapeType() != TopAbs_WIRE) {
                        B.Add(comp, ps);
                        any = true;
                        continue;
                    }
                    const TopoDS_Wire& w = static_cast<const TopoDS_Wire&>(ps);
                    bool isOuter = (idx < e.hatchPaths.size())
                                       ? e.hatchPaths[idx].isOuter
                                       : true;
                    if (isOuter) {
                        outerWires.push_back(w);
                    }
                    else {
                        innerWires.push_back(w);
                    }
                }
                for (const TopoDS_Wire& ow : outerWires) {
                    try {
                        BRepBuilderAPI_MakeFace f(ow, Standard_True);
                        for (const TopoDS_Wire& iw : innerWires) {
                            f.Add(iw);
                        }
                        if (f.IsDone()) {
                            B.Add(comp, f.Face());
                            any = true;
                        }
                        else {
                            B.Add(comp, ow);
                            any = true;
                        }
                    }
                    catch (const Standard_Failure&) {
                        B.Add(comp, ow);
                        any = true;
                    }
                }
            }
            else {
                // Pattern fill: boundary wires + pattern lines clipped to
                // the boundary loops.
                for (const TopoDS_Shape& ps : pathShapes) {
                    B.Add(comp, ps);
                    any = true;
                }
                if (!outerPolys.empty() && !deflines.empty()) {
                    // NOTE: the pitch of `deflines` is the raw pattern
                    // definition, so the family spacing used below is
                    // definition-units x 1.0.  On this metre drawing that makes
                    // one ANSI31 hatch sweep ~80 000 lines across a 7113 x 5019
                    // boundary (measured) instead of the few thousand AutoCAD
                    // draws, which is the "layer full of oversized elements"
                    // report.  The hatch's own pattern scale is NOT
                    // HATCH.scale_spacing: using that field multiplies the line
                    // count by ~5 instead of dividing it (measured, 80 742 ->
                    // 400 058), so the correct source still has to be
                    // identified - see docs/29.  Left at 1.0 deliberately so
                    // the behaviour does not get worse than the reported state.
                    std::vector<std::pair<Vec3, Vec3>> lines =
                        hatchPatternLines(outerPolys, innerPolys, deflines,
                                          1.0, e.hatchAngle, z);
                    // (the temporary HATCHLINES / HATCHPATH diagnostics that
                    // located the ccw mirror bug have been removed - see
                    // docs/29)
                    for (const auto& sg : lines) {
                        Vec3 a = xform(sg.first, M);
                        Vec3 b = xform(sg.second, M);
                        BRepBuilderAPI_MakeEdge me(
                            gp_Pnt(a.x, a.y, a.z), gp_Pnt(b.x, b.y, b.z));
                        if (me.IsDone()) {
                            B.Add(comp, me.Edge());
                            any = true;
                        }
                    }
                }
            }
            if (!any) {
                return TopoDS_Shape();
            }
            return comp;
        }
        default:
            return TopoDS_Shape();
    }
}

}  // namespace SWDWG
