//! DIMENSION entity extraction, geometry computation and document-object
//! creation.
//!
//! The LibreDWG objects are mapped to the plain DimensionData struct here
//! (extractDimension), then expanded to world-space display geometry
//! (buildDimensionGeometry).  DWGImporter.cpp only calls createDimensionObject
//! - none of the dimension logic lives in the importer itself.

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX  // keep std::min/std::max from Windows macros
#  endif
#  include <Windows.h>
#endif

#include "DWGDimension.h"
#include "DWGDimstyle.h"
#include "DWGInternal.h"

#include <App/Document.h>
#include <Base/Color.h>
#include <Base/Console.h>
#include <Base/Vector3D.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace SWDWG;

namespace
{

//! Depth of the "bulk dimension creation" windows opened by the importer.
//! While non-zero, ViewProviderDWGDimension defers geometry / glyph rebuilds.
int& dimBulkDepth()
{
    static int depth = 0;
    return depth;
}

//! Rounded / centre point helpers in the dimension plane (z constant).
double planeZ(const DimensionData& dim)
{
    return dim.defPt.z != 0.0 ? dim.defPt.z : dim.elevation;
}

Vec3 p2d(const Vec3& p, double z)
{
    return Vec3{p.x, p.y, z};
}

//! Dimension line / arc / leader segment (DIMCLRD colour group).
void addDimLine(DimGeom& g, const Vec3& a, const Vec3& b)
{
    g.dimLines.push_back(a);
    g.dimLines.push_back(b);
}

//! Extension line segment (DIMCLRE colour group).
void addExtLine(DimGeom& g, const Vec3& a, const Vec3& b)
{
    g.extLines.push_back(a);
    g.extLines.push_back(b);
}

Vec3 perp2d(const Vec3& u)
{
    return Vec3{-u.y, u.x, 0.0};
}

//! Closed-filled arrow geometry.  The length (tip to base) is the DIMASZ
//! value of the DWG dimension style (scaled by DIMSCALE); the half-width
//! ratio 0.25 is the fixed geometry of AutoCAD's built-in arrow symbol
//! (4:1 arrow, ~28° total angle) and is not a dimension-style variable.
constexpr double kArrowHalfWidthRatio = 0.25;

//! Append one filled arrow-head triangle.  \a tip is the arrow point, \a dir
//! the direction from the tip to the base, \a n a unit vector perpendicular
//! to \a dir in the plane.
void addArrow(DimGeom& g, const Vec3& tip, const Vec3& dir, double asz,
              const Vec3& n)
{
    if (asz <= 0.0) {
        return;
    }
    Vec3 base = add(tip, scale(dir, asz));
    Vec3 half = scale(n, asz * kArrowHalfWidthRatio);
    g.arrows.push_back(tip);
    g.arrows.push_back(add(base, half));
    g.arrows.push_back(sub(base, half));
}

//! Draw one dimension-line end marker.  The marker geometry is driven by the
//! DWG dimension style: a non-zero \a tickSize (DIMTSZ) draws an
//! architectural tick (a 45° dash of that length centred on \a tip),
//! otherwise a closed-filled arrow of length \a asz (DIMASZ) is drawn.
//! \a tip is the marker point on the dimension line, \a dir the direction
//! from the marker towards the interior of the dimension line, \a n the
//! dimension-line normal (towards the text).
void addDimEnd(DimGeom& g, const Vec3& tip, const Vec3& dir, const Vec3& n,
               double asz, double tickSize)
{
    if (tickSize > 1e-9) {
        Vec3 d = normalize(sub(n, dir));  // 45° between dir and the normal
        addDimLine(g, add(tip, scale(d, -tickSize * 0.5)),
                   add(tip, scale(d, tickSize * 0.5)));
        return;
    }
    addArrow(g, tip, dir, asz, n);
}

//! Angle sweep helper: normalise (a2 - a1) into (-PI, PI].
double angleSweep(double a1, double a2)
{
    double s = a2 - a1;
    while (s > PI) {
        s -= 2.0 * PI;
    }
    while (s <= -PI) {
        s += 2.0 * PI;
    }
    return s;
}

//! Keep dimension text readable: clamp the baseline angle to (-90°, +90°].
//! This mirrors AutoCAD's default DIMTXTDIRECTION=0 behaviour, which flips
//! the text 180° rather than letting it appear upside-down.
double readableAngle(double a)
{
    while (a <= -PI / 2.0) {
        a += PI;
    }
    while (a > PI / 2.0) {
        a -= PI;
    }
    return a;
}

//! "<>" placeholder in a user-supplied dimension text is replaced by the
//! auto measurement; a user text without placeholder wins verbatim.
std::string applyUserText(const std::string& base, const std::string& user)
{
    if (user.empty()) {
        return base;
    }
    size_t ph = user.find("<>");
    if (ph != std::string::npos) {
        return user.substr(0, ph) + base + user.substr(ph + 2);
    }
    return user;
}

//! Build the dimension text string (measurement or angle + auto prefixes).
std::string computeDimText(const DimensionData& dim, const DimStyle& style)
{
    if (dim.type == DimType::ANG3PT || dim.type == DimType::ANG2LN) {
        // DIMAUNIT default = decimal degrees.
        std::string v = formatDimValue(dim.angleValue * 180.0 / PI, style);
        return applyUserText(v + "\xC2\xB0", dim.userText);  // trailing °
    }
    std::string value = formatDimValue(dim.actMeasurement, style);
    if (dim.type == DimType::RADIUS) {
        value = "R" + value;
    }
    else if (dim.type == DimType::DIAMETER) {
        value = "\xC3\x98" + value;  // Ø (U+00D8)
    }
    return applyUserText(value, dim.userText);
}

const char* dimTypeName(DimType t)
{
    switch (t) {
        case DimType::LINEAR:
            return "LINEAR";
        case DimType::ALIGNED:
            return "ALIGNED";
        case DimType::ANG2LN:
            return "ANG2LN";
        case DimType::ANG3PT:
            return "ANG3PT";
        case DimType::DIAMETER:
            return "DIAMETER";
        case DimType::RADIUS:
            return "RADIUS";
        case DimType::ORDINATE:
            return "ORDINATE";
        default:
            return "NONE";
    }
}

//! Resolve a DIMCLR* colour value produced by parseDimStyles():
//!   0                 -> inherit from the entity / layer colour,
//!   0x01000000 | rgb  -> truecolor,
//!   1..255            -> ACI index.
Base::Color resolveDimColor(unsigned long v, const Entity& e)
{
    unsigned long rgb = 0;
    if (v & 0x01000000UL) {
        rgb = v & 0xFFFFFFUL;
    }
    else if (v != 0UL) {
        rgb = aciRgb(static_cast<int>(v & 0xFFUL));
    }
    else {
        rgb = e.rgb != 0 ? e.rgb : aciRgb(e.colorIndex ? e.colorIndex : 7);
    }
    return Base::Color(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                       static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                       static_cast<float>(rgb & 0xFF) / 255.0f);
}

//! Project a point onto the dimension line (through defPt, direction u).
Vec3 projectOnLine(const Vec3& p, const Vec3& defPt, const Vec3& u)
{
    double t = dot(sub(p, defPt), u);
    return add(defPt, scale(u, t));
}

//! Place the dimension text near \a mid, offset by \a n, and store the
//! center anchor.  The view provider renders with CENTER justification and
//! applies the width factor as an x-scale, so no manual width estimate is
//! needed here.
void placeDimText(DimGeom& g, const Vec3& mid, const Vec3& /*u*/, const Vec3& n,
                  double textH, double gap, int textPos, double rot)
{
    Vec3 center = mid;
    if (textPos == 1 || textPos == 3) {
        double off = gap + textH * 0.5;
        center = add(center, scale(n, textPos == 1 ? off : -off));
    }
    g.textPos = center;
    g.textRot = rot;
}

//! Interpolate a circular arc as straight segments (used for angular dims).
void addArcPolyline(DimGeom& g, const Vec3& center, double r, double a0,
                    double a1, double z)
{
    double sweep = angleSweep(a0, a1);
    int seg = std::max(8, static_cast<int>(std::ceil(std::fabs(sweep) / (PI / 16.0))));
    for (int i = 0; i < seg; ++i) {
        double t0 = a0 + sweep * i / seg;
        double t1 = a0 + sweep * (i + 1) / seg;
        Vec3 q0{r * std::cos(t0), r * std::sin(t0), z};
        Vec3 q1{r * std::cos(t1), r * std::sin(t1), z};
        addDimLine(g, add(center, q0), add(center, q1));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  LibreDWG object -> DimensionData
// ---------------------------------------------------------------------------

bool SWDWG::extractDimension(const Dwg_Data* dwg, const Dwg_Object* obj,
                             DimensionData& dim, int codepage)
{
    if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity) {
        return false;
    }
    const Dwg_Object_Entity* ent = obj->tio.entity;
    if (!ent) {
        return false;
    }

// The DIMENSION_COMMON macro fields are expanded identically into every
// sub-type struct, so the same assignment block works for all of them.
#define DIM_COMMON_FIELDS(D)                                                  \
    dim.defPt = Vec3{(D).def_pt.x, (D).def_pt.y, (D).def_pt.z};              \
    dim.textMid = Vec3{(D).text_midpt.x, (D).text_midpt.y, (D).elevation};   \
    dim.elevation = (D).elevation;                                           \
    dim.flag = (D).flag;                                                     \
    dim.textRotation = (D).text_rotation;                                    \
    dim.horizDir = (D).horiz_dir;                                            \
    dim.actMeasurement = (D).act_measurement;                                \
    dim.extrusion =                                                          \
        Vec3{(D).extrusion.x, (D).extrusion.y, (D).extrusion.z};             \
    if ((D).user_text) {                                                     \
        dim.userText = decodeDwgString((D).user_text, codepage);             \
    }                                                                        \
    if ((D).dimstyle) {                                                      \
        Dwg_Object* sobj =                                                   \
            dwg_ref_object(const_cast<Dwg_Data*>(dwg), (D).dimstyle);        \
        if (sobj) {                                                          \
            int err = 0;                                                     \
            char* n = dwg_obj_table_get_name(sobj, &err);                    \
            if (n) {                                                         \
                dim.dimStyleName = decodeDwgString(n, codepage);             \
            }                                                                \
        }                                                                    \
    }

    switch (obj->fixedtype) {
        case DWG_TYPE_DIMENSION_LINEAR: {
            const Dwg_Entity_DIMENSION_LINEAR* D = ent->tio.DIMENSION_LINEAR;
            if (!D) {
                return false;
            }
            dim.type = DimType::LINEAR;
            DIM_COMMON_FIELDS(*D);
            dim.xline1 = Vec3{D->xline1_pt.x, D->xline1_pt.y, D->xline1_pt.z};
            dim.xline2 = Vec3{D->xline2_pt.x, D->xline2_pt.y, D->xline2_pt.z};
            dim.oblique = D->oblique_angle;
            dim.dimRotation = D->dim_rotation;
            break;
        }
        case DWG_TYPE_DIMENSION_ALIGNED: {
            const Dwg_Entity_DIMENSION_ALIGNED* D = ent->tio.DIMENSION_ALIGNED;
            if (!D) {
                return false;
            }
            dim.type = DimType::ALIGNED;
            DIM_COMMON_FIELDS(*D);
            dim.xline1 = Vec3{D->xline1_pt.x, D->xline1_pt.y, D->xline1_pt.z};
            dim.xline2 = Vec3{D->xline2_pt.x, D->xline2_pt.y, D->xline2_pt.z};
            dim.oblique = D->oblique_angle;
            break;
        }
        case DWG_TYPE_DIMENSION_ANG3PT: {
            const Dwg_Entity_DIMENSION_ANG3PT* D = ent->tio.DIMENSION_ANG3PT;
            if (!D) {
                return false;
            }
            dim.type = DimType::ANG3PT;
            DIM_COMMON_FIELDS(*D);
            dim.xline1 = Vec3{D->xline1_pt.x, D->xline1_pt.y, D->xline1_pt.z};
            dim.xline2 = Vec3{D->xline2_pt.x, D->xline2_pt.y, D->xline2_pt.z};
            dim.centerPt = Vec3{D->center_pt.x, D->center_pt.y, D->center_pt.z};
            break;
        }
        case DWG_TYPE_DIMENSION_ANG2LN: {
            const Dwg_Entity_DIMENSION_ANG2LN* D = ent->tio.DIMENSION_ANG2LN;
            if (!D) {
                return false;
            }
            dim.type = DimType::ANG2LN;
            DIM_COMMON_FIELDS(*D);
            dim.xline1start =
                Vec3{D->xline1start_pt.x, D->xline1start_pt.y, D->xline1start_pt.z};
            dim.xline1end =
                Vec3{D->xline1end_pt.x, D->xline1end_pt.y, D->xline1end_pt.z};
            dim.xline2start =
                Vec3{D->xline2start_pt.x, D->xline2start_pt.y, D->xline2start_pt.z};
            dim.xline2end =
                Vec3{D->xline2end_pt.x, D->xline2end_pt.y, D->xline2end_pt.z};
            break;
        }
        case DWG_TYPE_DIMENSION_DIAMETER: {
            const Dwg_Entity_DIMENSION_DIAMETER* D =
                ent->tio.DIMENSION_DIAMETER;
            if (!D) {
                return false;
            }
            dim.type = DimType::DIAMETER;
            DIM_COMMON_FIELDS(*D);
            dim.firstArcPt =
                Vec3{D->first_arc_pt.x, D->first_arc_pt.y, D->first_arc_pt.z};
            dim.leaderLen = D->leader_len;
            break;
        }
        case DWG_TYPE_DIMENSION_RADIUS: {
            const Dwg_Entity_DIMENSION_RADIUS* D = ent->tio.DIMENSION_RADIUS;
            if (!D) {
                return false;
            }
            dim.type = DimType::RADIUS;
            DIM_COMMON_FIELDS(*D);
            dim.firstArcPt =
                Vec3{D->first_arc_pt.x, D->first_arc_pt.y, D->first_arc_pt.z};
            dim.leaderLen = D->leader_len;
            break;
        }
        case DWG_TYPE_DIMENSION_ORDINATE: {
            const Dwg_Entity_DIMENSION_ORDINATE* D =
                ent->tio.DIMENSION_ORDINATE;
            if (!D) {
                return false;
            }
            dim.type = DimType::ORDINATE;
            DIM_COMMON_FIELDS(*D);
            dim.featurePt =
                Vec3{D->feature_location_pt.x, D->feature_location_pt.y,
                     D->feature_location_pt.z};
            dim.leaderEnd =
                Vec3{D->leader_endpt.x, D->leader_endpt.y, D->leader_endpt.z};
            dim.ordX = (D->flag2 != 0);
            break;
        }
        default:
            return false;
    }
#undef DIM_COMMON_FIELDS

    // Derive the angular measurement for angular dimensions.
    if (dim.type == DimType::ANG3PT) {
        Vec3 v1 = sub(dim.xline1, dim.centerPt);
        Vec3 v2 = sub(dim.xline2, dim.centerPt);
        double a1 = std::atan2(v1.y, v1.x);
        double a2 = std::atan2(v2.y, v2.x);
        dim.angleValue = std::fabs(angleSweep(a1, a2));
    }
    else if (dim.type == DimType::ANG2LN) {
        Vec3 d1 = sub(dim.xline1end, dim.xline1start);
        Vec3 d2 = sub(dim.xline2end, dim.xline2start);
        double a1 = std::atan2(d1.y, d1.x);
        double a2 = std::atan2(d2.y, d2.x);
        dim.angleValue = std::fabs(angleSweep(a1, a2));
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Geometry computation
// ---------------------------------------------------------------------------

bool SWDWG::buildDimensionGeometry(const DimensionData& dim,
                                   const DimStyle& style, DimGeom& out)
{
    const double z = planeZ(dim);
    const double asz = style.arrowSize;
    const double textH = style.textHeight;

    switch (dim.type) {
        case DimType::LINEAR:
        case DimType::ALIGNED: {
            Vec3 p1 = p2d(dim.xline1, z);
            Vec3 p2 = p2d(dim.xline2, z);
            Vec3 defPt = p2d(dim.defPt, z);
            Vec3 m = sub(p2, p1);
            if (norm(m) < 1e-9) {
                return false;
            }
            Vec3 u = normalize(m);
            Vec3 uDim = u;
            if (dim.type == DimType::LINEAR) {
                // DXF group 50 ("angle of rotated, horizontal, or vertical
                // linear dimensions") always carries the effective direction:
                // 0 = horizontal, pi/2 = vertical, otherwise the rotated angle.
                // A stored 0 used to be treated as "orientation unknown" and
                // guessed from the deflection of def_pt relative to the
                // measured points.  That guess flips to the perpendicular axis
                // whenever the def point sits near the 45 degree diagonal, and
                // on the tested drawing it collapsed a 300 dimension to a
                // zero-length dim line (drawn, but invisible) and cut others to
                // cos(error) of their measurement (17 of 20, 21 of 24) - which
                // is what the report described as "the rotated dimensions
                // inside the block are missing" (they are LINEAR =
                // AutoCAD's 转角标注, all with a stored rotation of 0 or pi).
                //
                // The rotation is validated against the stored measurement:
                // act_measurement and these points are both in block-local
                // units, so the projection onto the true direction must equal
                // it.  If the rotation contradicts the measurement but the
                // perpendicular satisfies it, the perpendicular wins - that
                // keeps files which store 0 for a vertical dimension working.
                const double rad = dim.dimRotation;
                const Vec3 cand{std::cos(rad), std::sin(rad), 0.0};
                const Vec3 perp{-cand.y, cand.x, 0.0};
                const double meas = std::fabs(dim.actMeasurement);
                const double dCand = std::fabs(dot(m, cand));
                const double dPerp = std::fabs(dot(m, perp));
                uDim = (std::fabs(dCand - meas) <= std::fabs(dPerp - meas))
                    ? cand
                    : perp;
            }
            // Normal pointing from the measured points towards the dim line.
            Vec3 n = perp2d(uDim);
            Vec3 mid = scale(add(p1, p2), 0.5);
            if (dot(sub(defPt, mid), n) < 0.0) {
                n = scale(n, -1.0);
            }
            Vec3 q1 = projectOnLine(p1, defPt, uDim);
            Vec3 q2 = projectOnLine(p2, defPt, uDim);
            if (norm(sub(q2, q1)) < 1e-9) {
                return false;
            }
            // Extension lines (suppressed by DIMSE1 / DIMSE2).
            if (!style.supExt1) {
                addExtLine(out, add(p1, scale(n, style.extOffset)),
                           add(q1, scale(n, style.extLength)));
            }
            if (!style.supExt2) {
                addExtLine(out, add(p2, scale(n, style.extOffset)),
                           add(q2, scale(n, style.extLength)));
            }
            // Dimension line ends: arrows (DIMASZ) or ticks (DIMTSZ).
            Vec3 uu = normalize(sub(q2, q1));
            // Much like AutoCAD's DIMATFIT=3 ("best fit"), a dimension shorter
            // than its two arrow heads cannot hold them inside: the line used
            // to be trimmed by `asz` at *both* ends and came out exactly zero
            // length - a 300 mm feature at 1:100 is 3 units and DIMASZ is 1.5,
            // so the line vanished and the dimension looked missing from the
            // sheet.  The arrow heads move outside and the line runs the whole
            // way instead.
            const bool arrowsFit = (style.tickSize > 1e-9)
                || (norm(sub(q2, q1)) > 2.0 * asz);
            if (arrowsFit) {
                addDimEnd(out, q1, uu, n, asz, style.tickSize);
                addDimEnd(out, q2, scale(uu, -1.0), n, asz, style.tickSize);
            }
            else {
                addDimEnd(out, q1, scale(uu, -1.0), n, asz, style.tickSize);
                addDimEnd(out, q2, uu, n, asz, style.tickSize);
            }
            if (style.tickSize > 1e-9 || !arrowsFit) {
                // Ticks are centred on the extension lines, and outside arrows
                // leave the dimension line visible between them.
                addDimLine(out, q1, q2);
            }
            else {
                // Arrows span the ends, so the line runs base-to-base.
                addDimLine(out, add(q1, scale(uu, asz)),
                           sub(q2, scale(uu, asz)));
            }
            SWDWG_LOG(
                "[SWDWG DIM GEO] type=%s uDim=(%.4f,%.4f) n=(%.4f,%.4f) "
                "p1=(%.3f,%.3f) p2=(%.3f,%.3f) def=(%.3f,%.3f) "
                "q1=(%.3f,%.3f) q2=(%.3f,%.3f) textMid=(%.3f,%.3f)\n",
                dimTypeName(dim.type),
                uDim.x, uDim.y, n.x, n.y,
                p1.x, p1.y, p2.x, p2.y,
                defPt.x, defPt.y,
                q1.x, q1.y, q2.x, q2.y,
                dim.textMid.x, dim.textMid.y);
            // Text above/inside the dimension line.  Prefer the text
            // midpoint saved in the entity when it differs from the origin;
            // AutoCAD stores the actual text insertion point there.  When the
            // drawing already supplies that point we must not add the DIMTAD
            // gap offset on top of it, otherwise the text drifts away from the
            // dimension line.
            out.text = computeDimText(dim, style);
            const bool hasTextMid = std::fabs(dim.textMid.x) > 1e-9
                || std::fabs(dim.textMid.y) > 1e-9;
            Vec3 tMid = hasTextMid ? p2d(dim.textMid, z)
                                   : scale(add(q1, q2), 0.5);
            double baseRot = std::atan2(uu.y, uu.x);
            double textRot = std::fabs(dim.textRotation) > 1e-9
                ? dim.textRotation
                : readableAngle(baseRot);
            Vec3 uText{std::cos(textRot), std::sin(textRot), 0.0};
            Vec3 nText{-std::sin(textRot), std::cos(textRot), 0.0};
            placeDimText(out, tMid, uText, nText, textH, style.textGap,
                         hasTextMid ? 0 : style.textPos, textRot);
            break;
        }

        case DimType::RADIUS: {
            Vec3 c = p2d(dim.defPt, z);      // centre (DXF 10)
            Vec3 a = p2d(dim.firstArcPt, z); // point on the arc (DXF 15)
            Vec3 rv = sub(a, c);
            if (norm(rv) < 1e-9) {
                return false;
            }
            Vec3 dir = normalize(rv);
            double lead = dim.leaderLen > 0.0 ? dim.leaderLen
                                              : style.leaderLen;
            Vec3 n = perp2d(dir);
            addDimEnd(out, a, scale(dir, -1.0), n, asz, style.tickSize);
            if (lead > 1e-9) {
                addDimLine(out, a, add(a, scale(dir, lead)));
            }
            out.text = computeDimText(dim, style);
            Vec3 tMid = add(a, scale(dir, lead + style.textGap + textH * 0.5));
            double baseRot = std::atan2(dir.y, dir.x);
            double textRot = std::fabs(dim.textRotation) > 1e-9
                ? dim.textRotation
                : readableAngle(baseRot);
            Vec3 uText{std::cos(textRot), std::sin(textRot), 0.0};
            Vec3 nText{-std::sin(textRot), std::cos(textRot), 0.0};
            placeDimText(out, tMid, uText, nText, textH, style.textGap, 0,
                         textRot);
            break;
        }

        case DimType::DIAMETER: {
            Vec3 a = p2d(dim.firstArcPt, z);  // near chord point (DXF 15)
            Vec3 b = p2d(dim.defPt, z);       // far chord point (DXF 10)
            Vec3 dv = sub(b, a);
            if (norm(dv) < 1e-9) {
                return false;
            }
            Vec3 dir = normalize(dv);
            double lead = dim.leaderLen > 0.0 ? dim.leaderLen : 0.0;
            Vec3 n = perp2d(dir);
            addDimEnd(out, a, dir, n, asz, style.tickSize);
            addDimEnd(out, b, scale(dir, -1.0), n, asz, style.tickSize);
            Vec3 end = lead > 1e-9 ? add(b, scale(dir, lead)) : b;
            addDimLine(out, a, end);
            out.text = computeDimText(dim, style);
            Vec3 tMid = add(end, scale(dir, style.textGap + textH * 0.5));
            double baseRot = std::atan2(dir.y, dir.x);
            double textRot = std::fabs(dim.textRotation) > 1e-9
                ? dim.textRotation
                : readableAngle(baseRot);
            Vec3 uText{std::cos(textRot), std::sin(textRot), 0.0};
            Vec3 nText{-std::sin(textRot), std::cos(textRot), 0.0};
            placeDimText(out, tMid, uText, nText, textH, style.textGap, 0,
                         textRot);
            break;
        }

        case DimType::ANG3PT: {
            Vec3 c = p2d(dim.centerPt, z);
            Vec3 p1 = p2d(dim.xline1, z);
            Vec3 p2 = p2d(dim.xline2, z);
            Vec3 v1 = sub(p1, c);
            Vec3 v2 = sub(p2, c);
            if (norm(v1) < 1e-9 || norm(v2) < 1e-9) {
                return false;
            }
            double r = norm(sub(p2d(dim.defPt, z), c));
            if (r < 1e-9) {
                return false;
            }
            double a1 = std::atan2(v1.y, v1.x);
            double a2 = std::atan2(v2.y, v2.x);
            double sweep = angleSweep(a1, a2);
            if (sweep < 0.0) {
                sweep = -sweep;
                std::swap(a1, a2);
            }
            Vec3 u1 = normalize(v1);
            Vec3 u2 = normalize(v2);
            Vec3 arcStart = add(c, scale(u1, r));
            Vec3 arcEnd = add(c, scale(u2, r));
            // Extension lines: measured point -> arc.
            if (!style.supExt1) {
                addExtLine(out, p1, arcStart);
            }
            if (!style.supExt2) {
                addExtLine(out, p2, arcEnd);
            }
            // Arc approximation with arrow heads at both ends.
            addArcPolyline(out, c, r, a1, a1 + sweep, z);
            Vec3 t1{-std::sin(a1), std::cos(a1), 0.0};  // tangent at a1
            addDimEnd(out, arcStart, t1, normalize(sub(c, arcStart)), asz,
                      style.tickSize);
            Vec3 t2{-std::sin(a1 + sweep), std::cos(a1 + sweep), 0.0};
            addDimEnd(out, arcEnd, scale(t2, -1.0), normalize(sub(c, arcEnd)),
                      asz, style.tickSize);
            // Text outside the arc midpoint.
            double amid = a1 + sweep * 0.5;
            Vec3 outDir{std::cos(amid), std::sin(amid), 0.0};
            out.text = computeDimText(dim, style);
            Vec3 tMid = add(c, scale(outDir, r + style.textGap + textH * 0.5));
            double textRot = std::fabs(dim.textRotation) > 1e-9
                ? dim.textRotation
                : readableAngle(amid);
            Vec3 uText{std::cos(textRot), std::sin(textRot), 0.0};
            Vec3 nText{-std::sin(textRot), std::cos(textRot), 0.0};
            placeDimText(out, tMid, uText, nText, textH, style.textGap, 0,
                         textRot);
            break;
        }

        case DimType::ANG2LN: {
            // Two rays: xline1start->xline1end and xline2start->xline2end.
            Vec3 s1 = p2d(dim.xline1start, z);
            Vec3 e1 = p2d(dim.xline1end, z);
            Vec3 s2 = p2d(dim.xline2start, z);
            Vec3 e2 = p2d(dim.xline2end, z);
            Vec3 d1 = sub(e1, s1);
            Vec3 d2 = sub(e2, s2);
            if (norm(d1) < 1e-9 || norm(d2) < 1e-9) {
                return false;
            }
            Vec3 u1 = normalize(d1);
            Vec3 u2 = normalize(d2);
            // Arc centre = intersection of the two rays (fallback: line 1).
            double den = u1.x * u2.y - u1.y * u2.x;
            Vec3 center = s1;
            if (std::fabs(den) > 1e-12) {
                Vec3 w = sub(s2, s1);
                double t = (w.x * u2.y - w.y * u2.x) / den;
                center = add(s1, scale(u1, t));
            }
            double r = norm(sub(p2d(dim.defPt, z), center));
            if (r < 1e-9) {
                r = norm(sub(p2d(dim.defPt, z), s1));
            }
            double a1 = std::atan2(u1.y, u1.x);
            double a2 = std::atan2(u2.y, u2.x);
            double sweep = angleSweep(a1, a2);
            if (sweep < 0.0) {
                sweep = -sweep;
                std::swap(a1, a2);
                std::swap(u1, u2);
            }
            if (!style.supExt1) {
                addExtLine(out, s1, e1);
            }
            if (!style.supExt2) {
                addExtLine(out, s2, e2);
            }
            addArcPolyline(out, center, r, a1, a1 + sweep, z);
            Vec3 arcStart = add(center, scale(u1, r));
            Vec3 arcEnd = add(center, scale(u2, r));
            Vec3 t1{-std::sin(a1), std::cos(a1), 0.0};
            addDimEnd(out, arcStart, t1, normalize(sub(center, arcStart)), asz,
                      style.tickSize);
            Vec3 t2{-std::sin(a1 + sweep), std::cos(a1 + sweep), 0.0};
            addDimEnd(out, arcEnd, scale(t2, -1.0),
                      normalize(sub(center, arcEnd)), asz, style.tickSize);
            out.text = computeDimText(dim, style);
            double amid = a1 + sweep * 0.5;
            Vec3 outDir{std::cos(amid), std::sin(amid), 0.0};
            Vec3 tMid =
                add(center, scale(outDir, r + style.textGap + textH * 0.5));
            double textRot = std::fabs(dim.textRotation) > 1e-9
                ? dim.textRotation
                : readableAngle(amid);
            Vec3 uText{std::cos(textRot), std::sin(textRot), 0.0};
            Vec3 nText{-std::sin(textRot), std::cos(textRot), 0.0};
            placeDimText(out, tMid, uText, nText, textH, style.textGap, 0,
                         textRot);
            break;
        }

        case DimType::ORDINATE: {
            Vec3 f = p2d(dim.featurePt, z);
            Vec3 e = p2d(dim.leaderEnd, z);
            Vec3 corner = dim.ordX ? Vec3{f.x, e.y, z} : Vec3{e.x, f.y, z};
            addExtLine(out, f, corner);
            addDimLine(out, corner, e);
            Vec3 cf = sub(corner, f);
            if (norm(cf) > 1e-9) {
                Vec3 d0 = normalize(cf);
                addDimEnd(out, f, d0, perp2d(d0), asz, style.tickSize);
            }
            out.text = computeDimText(dim, style);
            Vec3 tMid = add(e, Vec3{0.0, textH * 0.6, 0.0});
            double textRot = std::fabs(dim.textRotation) > 1e-9
                ? dim.textRotation
                : readableAngle(0.0);
            Vec3 uText{std::cos(textRot), std::sin(textRot), 0.0};
            Vec3 nText{-std::sin(textRot), std::cos(textRot), 0.0};
            placeDimText(out, tMid, uText, nText, textH, style.textGap, 0,
                         textRot);
            break;
        }

        default:
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  FreeCAD object creation (called by DWGImporter.cpp)
// ---------------------------------------------------------------------------

PROPERTY_SOURCE(SWDWG::DWGDimension, App::DocumentObject)

DWGDimension::DWGDimension()
{
    ADD_PROPERTY_TYPE(DimType,
                      ("LINEAR"),
                      "Dimension",
                      App::Prop_None,
                      "DIMENSION sub-type");
    ADD_PROPERTY_TYPE(DimLines,
                      (),
                      "Dimension",
                      App::Prop_None,
                      "Dimension line/arc/leader segments (two points each)");
    ADD_PROPERTY_TYPE(ExtLines,
                      (),
                      "Dimension",
                      App::Prop_None,
                      "Extension line segments (two points each)");
    ADD_PROPERTY_TYPE(ArrowTriangles,
                      (),
                      "Dimension",
                      App::Prop_None,
                      "Arrow head triangles (three points each)");
    ADD_PROPERTY_TYPE(TextPosition,
                      (0.0, 0.0, 0.0),
                      "Dimension",
                      App::Prop_None,
                      "Center anchor of the dimension text");
    ADD_PROPERTY_TYPE(TextRotation,
                      (0.0),
                      "Dimension",
                      App::Prop_None,
                      "Text rotation about Z (radians)");
    ADD_PROPERTY_TYPE(DimText,
                      (""),
                      "Dimension",
                      App::Prop_None,
                      "Formatted measurement text");
    ADD_PROPERTY_TYPE(TextHeight,
                      (2.5),
                      "Dimension",
                      App::Prop_None,
                      "Text height in world units");
    ADD_PROPERTY_TYPE(Measurement,
                      (0.0),
                      "Dimension",
                      App::Prop_None,
                      "Raw measured value in world units");
    ADD_PROPERTY_TYPE(DimColor,
                      (1.0f, 1.0f, 1.0f),
                      "Dimension",
                      App::Prop_None,
                      "Dimension line and arrow colour");
    ADD_PROPERTY_TYPE(ExtColor,
                      (1.0f, 1.0f, 1.0f),
                      "Dimension",
                      App::Prop_None,
                      "Extension line colour");
    ADD_PROPERTY_TYPE(TextColor,
                      (1.0f, 1.0f, 1.0f),
                      "Dimension",
                      App::Prop_None,
                      "Dimension text colour");
    ADD_PROPERTY_TYPE(Layer,
                      ("0"),
                      "Dimension",
                      App::Prop_None,
                      "DWG layer name");
    ADD_PROPERTY_TYPE(EntityKind,
                      ("DIMENSION"),
                      "Dimension",
                      App::Prop_ReadOnly,
                      "Imported DWG entity kind (tree grouping key)");
    ADD_PROPERTY_TYPE(DimStyleName,
                      (""),
                      "Dimension",
                      App::Prop_None,
                      "Referenced DIMSTYLE name");
    ADD_PROPERTY_TYPE(TextStyleName,
                      (""),
                      "Dimension",
                      App::Prop_None,
                      "Text style name used for dimension text (DIMTXSTY)");
    ADD_PROPERTY_TYPE(TextWidthFactor,
                      (1.0),
                      "Dimension",
                      App::Prop_None,
                      "Horizontal stretch factor of dimension text");
    ADD_PROPERTY_TYPE(FontFile,
                      (""),
                      "Dimension",
                      App::Prop_None,
                      "Text style font file (e.g. txt.shx), empty for TTF");
    ADD_PROPERTY_TYPE(BigFontFile,
                      (""),
                      "Dimension",
                      App::Prop_None,
                      "Text style big-font (CJK) file, empty when none");
    ADD_PROPERTY_TYPE(Codepage,
                      (0),
                      "Dimension",
                      App::Prop_None,
                      "DWG codepage for the dimension text");
}

DWGDimension::~DWGDimension() = default;

void DWGDimension::enterBulkCreate()
{
    ++dimBulkDepth();
}

void DWGDimension::leaveBulkCreate()
{
    if (dimBulkDepth() > 0) {
        --dimBulkDepth();
    }
}

bool DWGDimension::isBulkCreating()
{
    return dimBulkDepth() > 0;
}

void DWGDimension::onChanged(const App::Property* prop)
{
    App::DocumentObject::onChanged(prop);
}

void SWDWG::createDimensionObject(
    App::Document* doc, const Entity& e, const Mat4& parent,
    const std::map<std::string, DimStyle>& dimStyles,
    const std::map<std::string, TextStyle>& textStyles,
    int codepage,
    std::map<std::string, int>& counters, int& created, int& skipped)
{
    const DimensionData& d = e.dim;

    DimStyle st;
    if (!d.dimStyleName.empty()) {
        auto it = dimStyles.find(d.dimStyleName);
        if (it != dimStyles.end()) {
            st = it->second;
        }
    }
    // DIMSCALE applies to every geometrical length.
    const double s = st.scale > 0.0 ? st.scale : 1.0;
    DimStyle use = st;
    // Resolve the width factor and font files from the dimension text style
    // (DIMTXSTY).  The font files let the view provider stroke the text with
    // the classic .shx fonts instead of a TrueType approximation.
    double textWidthFactor = 1.0;
    std::string dimFontFile;
    std::string dimBigFontFile;
    if (!st.textStyle.empty()) {
        auto tsIt = textStyles.find(st.textStyle);
        if (tsIt != textStyles.end()) {
            const TextStyle& ts = tsIt->second;
            textWidthFactor = ts.widthFactor;
            dimFontFile = ts.fontFile;
            dimBigFontFile = ts.bigFontFile;
        }
    }
    use.arrowSize = st.arrowSize * s;
    use.tickSize = st.tickSize * s;
    use.textHeight = st.textHeight * s;
    use.extOffset = st.extOffset * s;
    use.extLength = st.extLength * s;
    use.textGap = st.textGap * s;

    // The import's unit factor lives in the parent matrix (see
    // DWGImporter::run()); a metre drawing arrives scaled by 1000.
    //
    // It is applied to three different things, and they are deliberately not
    // all the same:
    //
    //   * the geometry vertices below go through xform(), and the text height
    //     through uniformScaleOf(parent), so the dimension object lives in mm
    //     like the rest of the model;
    //   * DimText is a transcription of what the drawing itself displays.  A
    //     dimension in AutoCAD shows the measured value in *drawing units*, so
    //     a 30 m road dimension reads "30" on the sheet; formatting the
    //     unscaled actMeasurement keeps the label directly comparable with the
    //     drawing the engineer checks it against (see docs/29);
    //   * Measurement carries the same value in mm, consistent with the
    //     geometry, for anything that reads the model programmatically.
    //
    // Angles are carried by angleValue and are not lengths, so they are
    // neither scaled nor affected.
    const double parentScale = uniformScaleOf(parent);
    const double measuredModel =
        (d.type == DimType::ANG3PT || d.type == DimType::ANG2LN)
        ? d.actMeasurement
        : d.actMeasurement * parentScale;

    DimGeom g;
    if (!buildDimensionGeometry(d, use, g)) {
        ++skipped;
        return;
    }

    // Transform block-local geometry into world space.
    const bool id = (parent.m[0][0] == 1.0 && parent.m[1][1] == 1.0
                     && parent.m[2][2] == 1.0 && parent.m[0][1] == 0.0
                     && parent.m[1][0] == 0.0 && parent.m[3][0] == 0.0
                     && parent.m[3][1] == 0.0 && parent.m[3][2] == 0.0);
    if (!id) {
        for (Vec3& p : g.dimLines) {
            p = xform(p, parent);
        }
        for (Vec3& p : g.extLines) {
            p = xform(p, parent);
        }
        for (Vec3& p : g.arrows) {
            p = xform(p, parent);
        }
        g.textPos = xform(g.textPos, parent);
        // The angle this placement adds to any local direction is the one that
        // takes the local +X axis to world space.  In this row-vector layout
        // (see insertMatrix in DWGTypes.h: m[0][0] = cos*sx, m[0][1] =
        // sin*sx, m[1][0] = -sin*sy) that axis is (m[0][0], m[0][1]), so the
        // angle is atan2(m[0][1], m[0][0]).  Reading column 0 of row 1 instead
        // gave -theta: a dimension inside a block inserted at +80 degrees came
        // out with its text at -80, i.e. rotated the opposite way from the
        // dimension line it belongs to (reported as "the text rotation
        // direction is different").  The geometry was never affected because
        // xform() uses the whole matrix.
        g.textRot += std::atan2(parent.m[0][1], parent.m[0][0]);
        // Text height is a screen-independent property, so it must be scaled
        // explicitly while the geometry vertices are already scaled by xform()
        // - the same factor that already rescaled actMeasurement above.
        if (parentScale > 1e-9) {
            use.textHeight *= parentScale;
        }
    }

    SWDWG_LOG(
        "[SWDWG DIM] type=%s style='%s' scale=%.4f arrow=%.4f tick=%.4f "
        "txtH=%.4f prec=%d dimCol=%lu extCol=%lu txtCol=%lu layer='%s' "
        "e.rgb=%06lX e.ci=%d meas=%.4f userText='%s' finalText='%s' "
        "x1=(%.3f,%.3f) x2=(%.3f,%.3f) def=(%.3f,%.3f) tmid=(%.3f,%.3f) "
        "textPos=(%.3f,%.3f) rot=%.3f\n",
        dimTypeName(d.type),
        st.name.c_str(),
        st.scale,
        use.arrowSize,
        use.tickSize,
        use.textHeight,
        st.precision,
        st.dimColor,
        st.extColor,
        st.textColor,
        e.layer.c_str(),
        e.rgb,
        e.colorIndex,
        d.actMeasurement,
        d.userText.c_str(),
        g.text.c_str(),
        d.xline1.x, d.xline1.y,
        d.xline2.x, d.xline2.y,
        d.defPt.x, d.defPt.y,
        d.textMid.x, d.textMid.y,
        g.textPos.x, g.textPos.y,
        g.textRot);

    std::string name = "Dimension" + std::to_string(++counters["Dimension"]);
    // Open the bulk window BEFORE addObject(): while the object is attached
    // FreeCAD already issues one updateData per property (alphabetical) and
    // that first pass would otherwise rebuild the empty geometry 14+ times.
    // Deferring every rebuild to the final DimText.touch() below leaves a
    // single geometry / glyph build per dimension.
    DWGDimension::enterBulkCreate();
    auto* obj = doc->addObject<DWGDimension>(name.c_str());
    if (!obj) {
        DWGDimension::leaveBulkCreate();
        ++skipped;
        return;
    }

    std::vector<Base::Vector3d> dl;
    dl.reserve(g.dimLines.size());
    for (const Vec3& p : g.dimLines) {
        dl.emplace_back(p.x, p.y, p.z);
    }
    std::vector<Base::Vector3d> el;
    el.reserve(g.extLines.size());
    for (const Vec3& p : g.extLines) {
        el.emplace_back(p.x, p.y, p.z);
    }
    std::vector<Base::Vector3d> av;
    av.reserve(g.arrows.size());
    for (const Vec3& p : g.arrows) {
        av.emplace_back(p.x, p.y, p.z);
    }

    obj->DimType.setValue(dimTypeName(d.type));
    obj->DimLines.setValues(dl);
    obj->ExtLines.setValues(el);
    obj->ArrowTriangles.setValues(av);
    obj->TextPosition.setValue(g.textPos.x, g.textPos.y, g.textPos.z);
    obj->TextRotation.setValue(g.textRot);
    obj->DimText.setValue(g.text.c_str());
    obj->TextHeight.setValue(static_cast<float>(use.textHeight));
    obj->Measurement.setValue(static_cast<float>(measuredModel));
    obj->DimColor.setValue(resolveDimColor(st.dimColor, e));
    obj->ExtColor.setValue(resolveDimColor(st.extColor, e));
    obj->TextColor.setValue(resolveDimColor(st.textColor, e));
    obj->Layer.setValue(e.layer.c_str());
    obj->EntityKind.setValue("DIMENSION");
    obj->DimStyleName.setValue(st.name.c_str());
    obj->TextStyleName.setValue(st.textStyle.c_str());
    obj->TextWidthFactor.setValue(static_cast<float>(textWidthFactor));
    obj->FontFile.setValue(dimFontFile.c_str());
    obj->BigFontFile.setValue(dimBigFontFile.c_str());
    obj->Codepage.setValue(codepage);
    obj->Label.setValue((std::string("Dimension - ") + dimTypeName(d.type)
                         + " - " + g.text)
                            .c_str());
    // Close the bulk window and run the deferred geometry / text rebuild.
    DWGDimension::leaveBulkCreate();
    obj->DimText.touch();

    ++created;
}
