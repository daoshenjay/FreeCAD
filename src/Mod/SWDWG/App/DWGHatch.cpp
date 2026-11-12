//! HATCH pattern generation: boundary clipping and dashed-line rendering.
//!
//! All geometry is 2D (the HATCH plane) and this file intentionally has no
//! FreeCAD / OCC / LibreDWG dependencies, so the pattern math can be
//! developed and debugged in isolation.

#include "DWGTypes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>
#include <vector>

namespace SWDWG
{

//! Intersection of two 2D segments (a-b and c-d).  On success stores the
//! parameters t1 (on a-b) and t2 (on c-d) and returns true.
static bool segSegIntersect2D(const Vec3& a, const Vec3& b, const Vec3& c,
                              const Vec3& d, double& t1, double& t2)
{
    double rx = b.x - a.x;
    double ry = b.y - a.y;
    double sx = d.x - c.x;
    double sy = d.y - c.y;
    double denom = rx * sy - ry * sx;
    if (std::fabs(denom) < 1e-12) {
        return false;  // parallel or collinear
    }
    double qx = c.x - a.x;
    double qy = c.y - a.y;
    t1 = (qx * sy - qy * sx) / denom;
    t2 = (qx * ry - qy * rx) / denom;
    if (t1 < -1e-9 || t1 > 1.0 + 1e-9 || t2 < -1e-9 || t2 > 1.0 + 1e-9) {
        return false;
    }
    return true;
}

//! Ray-casting point-in-polygon test (poly is a 2D vertex list, z ignored).
static bool pointInPoly2D(const Vec3& p, const std::vector<Vec3>& poly)
{
    bool inside = false;
    size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec3& a = poly[i];
        const Vec3& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            double xint = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < xint) {
                inside = !inside;
            }
        }
    }
    return inside;
}

//! Axis-aligned bounding box of one boundary polygon.
struct PolyBox
{
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    bool valid = false;

    bool contains(double x, double y) const
    {
        return valid && x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
};

static PolyBox polyBox(const std::vector<Vec3>& poly)
{
    PolyBox b;
    for (const Vec3& p : poly) {
        if (!b.valid) {
            b.x0 = b.x1 = p.x;
            b.y0 = b.y1 = p.y;
            b.valid = true;
        }
        else {
            b.x0 = std::min(b.x0, p.x);
            b.x1 = std::max(b.x1, p.x);
            b.y0 = std::min(b.y0, p.y);
            b.y1 = std::max(b.y1, p.y);
        }
    }
    return b;
}

//! Clip an infinite hatch pattern line (segment from p1 to p2 that already
//! spans the whole boundary bounding box) against the boundary polygons.
//! Segments inside an outer polygon but inside an inner (hole) polygon are
//! discarded.
static std::vector<std::pair<Vec3, Vec3>> clipLineToPolys(
    const Vec3& p1, const Vec3& p2,
    const std::vector<std::vector<Vec3>>& outerPolys,
    const std::vector<std::vector<Vec3>>& innerPolys,
    const std::vector<PolyBox>& outerBoxes,
    const std::vector<PolyBox>& innerBoxes, double z)
{
    std::vector<std::pair<Vec3, Vec3>> out;
    // Reserve generously: a sweep line can cross several boundaries, so a
    // fixed 8-entry buffer would force many reallocs per line.
    std::size_t estEdges = 0;
    for (const auto& poly : outerPolys) {
        estEdges += poly.size();
    }
    for (const auto& poly : innerPolys) {
        estEdges += poly.size();
    }
    std::vector<double> ts;
    ts.reserve(estEdges + 2);
    ts.push_back(0.0);
    ts.push_back(1.0);
    auto collect = [&](const std::vector<std::vector<Vec3>>& polys) {
        for (const auto& poly : polys) {
            size_t n = poly.size();
            if (n < 3) {
                continue;
            }
            for (size_t i = 0; i < n; ++i) {
                Vec3 c = poly[i];
                Vec3 d = poly[(i + 1) % n];
                double t1 = 0.0, t2 = 0.0;
                if (segSegIntersect2D(p1, p2, c, d, t1, t2)) {
                    ts.push_back(t1);
                }
            }
        }
    };
    collect(outerPolys);
    collect(innerPolys);
    std::sort(ts.begin(), ts.end());
    for (size_t i = 0; i + 1 < ts.size(); ++i) {
        double t0 = ts[i];
        double t1 = ts[i + 1];
        if (t1 - t0 < 1e-9) {
            continue;
        }
        double tm = 0.5 * (t0 + t1);
        double mx = p1.x + (p2.x - p1.x) * tm;
        double my = p1.y + (p2.y - p1.y) * tm;
        Vec3 mid{mx, my, z};
        // Fast reject: only polygons whose bbox contains the midpoint can
        // claim it.  Skips the O(n) ray cast for every far-away boundary of
        // a drawing with many separate hatch regions / islands.
        bool inside = false;
        for (size_t oi = 0; oi < outerPolys.size(); ++oi) {
            if (oi < outerBoxes.size() && !outerBoxes[oi].contains(mx, my)) {
                continue;
            }
            if (pointInPoly2D(mid, outerPolys[oi])) {
                inside = true;
                break;
            }
        }
        if (inside) {
            for (size_t ii = 0; ii < innerPolys.size(); ++ii) {
                if (ii < innerBoxes.size()
                    && !innerBoxes[ii].contains(mx, my)) {
                    continue;
                }
                if (pointInPoly2D(mid, innerPolys[ii])) {
                    inside = false;
                    break;
                }
            }
        }
        if (!inside) {
            continue;
        }
        out.emplace_back(
            Vec3{p1.x + (p2.x - p1.x) * t0, p1.y + (p2.y - p1.y) * t0, z},
            Vec3{p1.x + (p2.x - p1.x) * t1, p1.y + (p2.y - p1.y) * t1, z});
    }
    return out;
}

//! Split a clipped segment into pen-up/pen-down pieces according to the
//! pattern definition.  The dash lengths are signed: positive = pen-down,
//! negative = pen-up.  The sign of each entry determines the pen state for
//! that segment directly; they do not alternate implicitly.  "phase" is the
//! distance from the pattern origin of the current family line to the segment
//! start measured along the line direction, so neighbouring segments share a
//! coherent dash pattern.
static std::vector<std::pair<Vec3, Vec3>> applyDashPattern(
    const Vec3& a, const Vec3& b, const std::vector<double>& dashes,
    double phase)
{
    std::vector<std::pair<Vec3, Vec3>> out;
    if (dashes.empty()) {
        out.emplace_back(a, b);
        return out;
    }
    double cycle = 0.0;
    for (double d : dashes) {
        cycle += std::fabs(d);
    }
    if (cycle <= 1e-12) {
        out.emplace_back(a, b);
        return out;
    }
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double L = std::hypot(dx, dy);
    if (L <= 1e-12) {
        return out;
    }
    Vec3 d{dx / L, dy / L, 0.0};
    // Absolute pattern coordinate of the segment start, normalised to
    // [0, cycle).  Walk the pattern to find the starting segment.
    double s = std::fmod(std::fmod(phase, cycle) + cycle, cycle);
    size_t di = 0;
    double consumed = 0.0;
    while (consumed + std::fabs(dashes[di]) <= s + 1e-12) {
        consumed += std::fabs(dashes[di]);
        di = (di + 1) % dashes.size();
    }
    double pos = s - consumed;  // position inside the current dash segment
    double t = 0.0;
    while (t < L - 1e-12) {
        double remain = std::fabs(dashes[di]) - pos;  // left in this segment
        double e = std::min(t + remain, L);          // end of this piece
        if (dashes[di] >= 0.0) {                      // pen-down for positives
            out.emplace_back(Vec3{a.x + d.x * t, a.y + d.y * t, a.z},
                             Vec3{a.x + d.x * e, a.y + d.y * e, a.z});
        }
        t = e;
        if (t >= L - 1e-12) {
            break;
        }
        pos = 0.0;
        di = (di + 1) % dashes.size();
    }
    return out;
}

//! Return a built-in hatch pattern definition for common AutoCAD pattern
//! names.  DWG files often reference predefined patterns (e.g. ANSI31,
//! AR-B816) by name only; libredwg does not embed the .pat definition, so we
//! supply a small set of standard definitions ourselves.
std::vector<HatchDefLine> builtinHatchPattern(const std::string& name)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    std::vector<HatchDefLine> out;
    if (n == "ANSI31") {
        // 45-degree parallel lines, spacing 0.125.
        out.push_back(
            {PI / 4.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 0.125, 0.0}, {}});
    }
    else if (n == "ANSI32") {
        // 45-degree parallel lines, spacing 0.25 with dash-gap.
        out.push_back({PI / 4.0,
                       Vec3{0.0, 0.0, 0.0},
                       Vec3{0.0, 0.25, 0.0},
                       {0.125, -0.0625, 0.0, -0.0625}});
    }
    else if (n == "ANSI33") {
        // -45-degree parallel lines, spacing 0.25 with dash-gap.
        out.push_back({-PI / 4.0,
                       Vec3{0.0, 0.0, 0.0},
                       Vec3{0.0, 0.25, 0.0},
                       {0.125, -0.0625, 0.0, -0.0625}});
    }
    else if (n == "ANSI34") {
        // Horizontal parallel lines, spacing 0.125.
        out.push_back({0.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 0.125, 0.0}, {}});
    }
    else if (n == "ANSI35") {
        // Vertical parallel lines, spacing 0.125.
        out.push_back(
            {PI / 2.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.125, 0.0, 0.0}, {}});
    }
    else if (n == "ANSI36") {
        // Horizontal/vertical grid, spacing 0.25.
        out.push_back({0.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 0.25, 0.0}, {}});
        out.push_back(
            {PI / 2.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.25, 0.0, 0.0}, {}});
    }
    else if (n == "ANSI37") {
        // 45-degree crosshatch.
        out.push_back(
            {PI / 4.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 0.25, 0.0}, {}});
        out.push_back(
            {-PI / 4.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 0.25, 0.0}, {}});
    }
    else if (n == "ANSI38") {
        // Horizontal lines with dash-gap.
        out.push_back({0.0,
                       Vec3{0.0, 0.0, 0.0},
                       Vec3{0.0, 0.25, 0.0},
                       {0.125, -0.125}});
    }
    else if (n == "AR-B816" || n == "AR-B816C") {
        // 8x16 running-bond brick (AutoCAD standard).
        out.push_back({0.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 8.0, 0.0}, {}});
        out.push_back({PI / 2.0,
                       Vec3{0.0, 0.0, 0.0},
                       Vec3{8.0, 8.0, 0.0},
                       {8.0, -8.0}});
    }
    else if (n == "AR-B88") {
        // 8x8 brick.
        out.push_back({0.0, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 8.0, 0.0}, {}});
        out.push_back({PI / 2.0,
                       Vec3{0.0, 0.0, 0.0},
                       Vec3{8.0, 8.0, 0.0},
                       {8.0, -8.0}});
    }
    return out;
}

//! Generate hatch pattern lines: for each pattern definition line family,
//! sweep parallel lines across the boundary bounding box and clip them to
//! the boundary polygons.  Returns model-space (pre-transform) 2D segments.
std::vector<std::pair<Vec3, Vec3>> hatchPatternLines(
    const std::vector<std::vector<Vec3>>& outerPolys,
    const std::vector<std::vector<Vec3>>& innerPolys,
    const std::vector<HatchDefLine>& deflines, double scale, double rot,
    double z)
{
    std::vector<std::pair<Vec3, Vec3>> out;
    if (outerPolys.empty() || deflines.empty()) {
        return out;
    }
    // Per-polygon bboxes, computed once, reused by every sweep line's clip
    // for fast rejection of far-away boundaries.
    std::vector<PolyBox> outerBoxes;
    outerBoxes.reserve(outerPolys.size());
    for (const auto& poly : outerPolys) {
        outerBoxes.push_back(polyBox(poly));
    }
    std::vector<PolyBox> innerBoxes;
    innerBoxes.reserve(innerPolys.size());
    for (const auto& poly : innerPolys) {
        innerBoxes.push_back(polyBox(poly));
    }
    // Bounding box over all boundary polygons.
    double xmin = 0.0, ymin = 0.0, xmax = 0.0, ymax = 0.0;
    bool have = false;
    auto expandBBox = [&](const std::vector<std::vector<Vec3>>& polys) {
        for (const auto& poly : polys) {
            for (const Vec3& p : poly) {
                if (!have) {
                    xmin = xmax = p.x;
                    ymin = ymax = p.y;
                    have = true;
                }
                else {
                    xmin = std::min(xmin, p.x);
                    xmax = std::max(xmax, p.x);
                    ymin = std::min(ymin, p.y);
                    ymax = std::max(ymax, p.y);
                }
            }
        }
    };
    expandBBox(outerPolys);
    expandBBox(innerPolys);
    if (!have || xmax - xmin < 1e-12 || ymax - ymin < 1e-12) {
        return out;
    }
    Vec3 corners[4] = {{xmin, ymin, 0.0},
                       {xmax, ymin, 0.0},
                       {xmax, ymax, 0.0},
                       {xmin, ymax, 0.0}};

    for (const HatchDefLine& dl : deflines) {
        double a = dl.angle + rot;
        double ca = std::cos(a);
        double sa = std::sin(a);
        Vec3 dir{ca, sa, 0.0};
        // The pattern scale applies to the definition; the offset defines the
        // family pitch.  Skip families with no pitch (would be a single line).
        Vec3 off{dl.offset.x * scale, dl.offset.y * scale, 0.0};
        Vec3 p0{dl.pt0.x * scale, dl.pt0.y * scale, 0.0};
        // The family spacing is the component of the offset vector that is
        // perpendicular to the line direction.  k advances by one family
        // member each time the line is shifted by this perpendicular distance.
        double perpX = -sa;
        double perpY = ca;
        double perpSpacing = off.x * perpX + off.y * perpY;
        if (std::abs(perpSpacing) < 1e-12) {
            continue;  // offset parallel to line direction -> degenerate family
        }
        double perpSpacingAbs = std::abs(perpSpacing);
        double p0Perp = p0.x * perpX + p0.y * perpY;
        // Sweep range of the family index k: project the bbox corners onto the
        // perpendicular direction, measured in units of perpSpacingAbs.
        double pmin = 0.0, pmax = 0.0;
        bool first = true;
        for (const Vec3& c : corners) {
            double pr = (c.x * perpX + c.y * perpY) - p0Perp;
            if (first) {
                pmin = pmax = pr;
                first = false;
            }
            else {
                pmin = std::min(pmin, pr);
                pmax = std::max(pmax, pr);
            }
        }
        // Note: perpSpacing is SIGNED.  A negative perpSpacing flips the
        // order of the quotients, so take min/max of the two ratios.
        double q1 = pmin / perpSpacing;
        double q2 = pmax / perpSpacing;
        long kmin = (long)std::floor(std::min(q1, q2)) - 1;
        long kmax = (long)std::ceil(std::max(q1, q2)) + 1;
        // Along-line extent of the bbox, plus a margin for clipping safety.
        double dmin = 0.0, dmax = 0.0;
        first = true;
        for (const Vec3& c : corners) {
            double pr = c.x * ca + c.y * sa;
            if (first) {
                dmin = dmax = pr;
                first = false;
            }
            else {
                dmin = std::min(dmin, pr);
                dmax = std::max(dmax, pr);
            }
        }
        double margin = perpSpacingAbs;
        double t0 = dmin - margin;
        double t1 = dmax + margin;
        // Dash lengths are in pattern units and are scaled like the rest of
        // the definition.
        std::vector<double> dashes;
        dashes.reserve(dl.dashes.size());
        for (double dd : dl.dashes) {
            dashes.push_back(dd * scale);
        }
        for (long k = kmin; k <= kmax; ++k) {
            Vec3 ref{p0.x + off.x * k, p0.y + off.y * k, 0.0};
            // Shift the along-line span so that this particular family member
            // passes through the reference point; t0/t1 are expressed relative
            // to the line-direction origin, not relative to the reference point.
            double refAlong = ref.x * ca + ref.y * sa;
            Vec3 p1{ref.x + dir.x * (t0 - refAlong),
                    ref.y + dir.y * (t0 - refAlong), z};
            Vec3 p2{ref.x + dir.x * (t1 - refAlong),
                    ref.y + dir.y * (t1 - refAlong), z};
            // Dash pattern origin is the reference point projected onto the line.
            double orgDot = refAlong;
            std::vector<std::pair<Vec3, Vec3>> segs =
                clipLineToPolys(p1, p2, outerPolys, innerPolys, outerBoxes,
                                innerBoxes, z);
            for (const auto& sg : segs) {
                // Phase is the signed distance from the family-line origin to
                // the segment start, so all lines in this family share the
                // same dash origin.
                double phase = (sg.first.x * ca + sg.first.y * sa) - orgDot;
                std::vector<std::pair<Vec3, Vec3>> pieces =
                    applyDashPattern(sg.first, sg.second, dashes, phase);
                out.insert(out.end(), pieces.begin(), pieces.end());
            }
        }
    }
    return out;
}

}  // namespace SWDWG
