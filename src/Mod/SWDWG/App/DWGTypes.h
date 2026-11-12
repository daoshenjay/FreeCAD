//! Shared types and small math helpers for the pure C++ DWG importer.
//!
//! This header is deliberately dependency-free (no FreeCAD, OCC or
//! LibreDWG headers) so that every translation unit that only needs the
//! geometric model can include it cheaply.

#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace SWDWG
{

constexpr double PI = 3.14159265358979323846;

// ---------------------------------------------------------------------------
//  Drawing units
// ---------------------------------------------------------------------------
//! $INSUNITS code -> length of one drawing unit in millimetres (the internal
//! unit of a FreeCAD model).
//!
//! The table is deliberately the same one FreeCAD's DXF importer uses
//! (`src/Mod/Import/App/dxf/dxf.cpp`, `DxfUnits::Factor`), so a .dwg and the
//! equivalent .dxf import at the same size.  Code 4 is millimetres and maps to
//! 1.0, so a drawing that declares millimetres - which FreeCAD already assumes -
//! is unaffected.
//!
//! Code 0 means "unspecified" and defaults to metres (1000.0 mm).  Civil /
//! road DWG files that omit $INSUNITS are authored with metres in mind far more
//! often than with millimetres, and the millimetre default produced 1000x
//! under-sized models for the bulk of this project's input; the metre default
//! is consistent with what the software that wrote those drawings meant.
inline double drawingUnitToMm(int insunits)
{
    switch (insunits) {
        case 1:  return 25.4;              // Inches
        case 2:  return 25.4 * 12.0;       // Feet
        case 3:  return 1609344.0;         // Miles
        case 4:  return 1.0;               // Millimetres
        case 5:  return 10.0;              // Centimetres
        case 6:  return 1000.0;            // Metres
        case 7:  return 1000000.0;         // Kilometres
        case 8:  return 25.4 / 1000.0;     // Microinches
        case 9:  return 25.4 / 1000.0;     // Mils
        case 10: return 3.0 * 12.0 * 25.4; // Yards
        case 11: return 1e-7;              // Angstroms
        case 12: return 1e-6;              // Nanometres
        case 13: return 1e-3;              // Microns
        case 14: return 100.0;             // Decimetres
        case 15: return 10000.0;           // Decametres
        case 16: return 100000.0;          // Hectometres
        case 17: return 1e12;              // Gigametres
        case 18: return 1.4959787069e14;   // Astronomical units
        case 19: return 9.4542549555e18;   // Light years
        case 20: return 3.0856774879e19;   // Parsecs
        default: return 1000.0;            // 0 = unspecified -> metres
    }
}

//! Human readable name of an $INSUNITS code (for reports / diagnostics).
inline const char* drawingUnitName(int insunits)
{
    switch (insunits) {
        case 1:  return "Inches";
        case 2:  return "Feet";
        case 3:  return "Miles";
        case 4:  return "Millimetres";
        case 5:  return "Centimetres";
        case 6:  return "Metres";
        case 7:  return "Kilometres";
        case 8:  return "Microinches";
        case 9:  return "Mils";
        case 10: return "Yards";
        case 11: return "Angstroms";
        case 12: return "Nanometres";
        case 13: return "Microns";
        case 14: return "Decimetres";
        case 15: return "Decametres";
        case 16: return "Hectometres";
        case 17: return "Gigametres";
        case 18: return "AstronomicalUnits";
        case 19: return "LightYears";
        case 20: return "Parsecs";
        default: return "Unspecified";
    }
}

//! Unit code to actually work with: the declared $INSUNITS, or 0 (unspecified)
//! when the drawing does not declare one.  Code 0 is the "default" sentinel for
//! drawingUnitToMm() to apply the civil/road metres assumption (see above).
//!
//! FreeCAD's DXF importer additionally falls back to $MEASUREMENT (imperial ->
//! inches) when $INSUNITS is absent.  That fallback is deliberately *not*
//! applied here: LibreDWG leaves $MEASUREMENT at 0 when the variable is missing
//! from the file, which is indistinguishable from a drawing that really
//! declares imperial units, so falling back would rescale an unlabelled drawing
//! by 25.4 for no reason.  An unlabelled drawing is treated as metres.
inline int effectiveDrawingUnit(int insunits)
{
    return (insunits > 0 && insunits <= 20) ? insunits : 0;
}

// ---------------------------------------------------------------------------
//  Small 3D helpers
// ---------------------------------------------------------------------------

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline double norm(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

inline Vec3 sub(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 add(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 scale(const Vec3& a, double s)
{
    return Vec3{a.x * s, a.y * s, a.z * s};
}

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 normalize(const Vec3& a)
{
    double n = norm(a);
    if (n < 1e-15) {
        return Vec3{};
    }
    return Vec3{a.x / n, a.y / n, a.z / n};
}

// ---------------------------------------------------------------------------
//  4x4 row-vector matrix, p' = p * M.  Layout and conventions match
//  FreeCAD Base::Matrix4D exactly.
// ---------------------------------------------------------------------------

struct Mat4
{
    double m[4][4] = {};
};

inline Mat4 matIdentity()
{
    Mat4 M;
    M.m[0][0] = M.m[1][1] = M.m[2][2] = M.m[3][3] = 1.0;
    return M;
}

inline Mat4 matMul(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j]
                + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        }
    }
    return r;
}

inline Vec3 xform(const Vec3& p, const Mat4& M)
{
    Vec3 r;
    r.x = p.x * M.m[0][0] + p.y * M.m[1][0] + p.z * M.m[2][0] + M.m[3][0];
    r.y = p.x * M.m[0][1] + p.y * M.m[1][1] + p.z * M.m[2][1] + M.m[3][1];
    r.z = p.x * M.m[0][2] + p.y * M.m[1][2] + p.z * M.m[2][2] + M.m[3][2];
    return r;
}

//! Rotation from the object coordinate system (OCS) of a 2D entity into world
//! coordinates, following AutoCAD's "arbitrary axis algorithm".
//!
//! Every 2D entity (ARC, CIRCLE, ELLIPSE, LWPOLYLINE, SOLID, HATCH, ...) stores
//! its coordinates in the plane defined by its *extrusion* (DXF 210-230), not
//! in world coordinates.  The default extrusion (0,0,1) makes the two coincide,
//! which is why ignoring the field goes unnoticed on most drawings - but any
//! entity that has been through AutoCAD MIRROR, or drawn in a UCS whose plane is
//! flipped, carries (0,0,-1), and then the stored numbers are mirrored:
//!
//!   OCS (-33871.802, 46535.477) with extrusion (0,0,-1)
//!     -> WCS ( 33871.802, 46535.477)
//!
//! Reading such a point as if it were already world coordinates puts the entity
//! ~67 m away from where AutoCAD draws it, i.e. it looks missing (see docs/31).
//!
//! The returned matrix is a pure rotation (the basis is right-handed, so the
//! determinant is +1 even for the mirrored cases) and is meant to be composed
//! *before* the entity's placement: with the row-vector convention of this
//! project, world = local * ocsToWorld(extrusion) * placement.
inline Mat4 ocsToWorld(const Vec3& extrusion)
{
    const double len = norm(extrusion);
    if (len < 1e-12) {
        return matIdentity();  // no usable normal: assume the world XY plane
    }
    const Vec3 n = scale(extrusion, 1.0 / len);
    if (n.z > 0.0 && std::fabs(n.x) < 1e-12 && std::fabs(n.y) < 1e-12) {
        return matIdentity();  // +Z: OCS == WCS, the overwhelmingly common case
    }

    // Arbitrary axis algorithm: pick the OCS X axis from the world axis the
    // normal is least aligned with, then complete a right-handed basis.
    Vec3 ax = (std::fabs(n.x) < 1.0 / 64.0 && std::fabs(n.y) < 1.0 / 64.0)
        ? cross(Vec3{0.0, 1.0, 0.0}, n)
        : cross(Vec3{0.0, 0.0, 1.0}, n);
    const double axLen = norm(ax);
    if (axLen < 1e-12) {
        return matIdentity();
    }
    ax = scale(ax, 1.0 / axLen);
    const Vec3 ay = normalize(cross(n, ax));

    // Row-vector layout (see xform()): row i holds the image of basis vector i.
    Mat4 m = matIdentity();
    m.m[0][0] = ax.x;
    m.m[0][1] = ax.y;
    m.m[0][2] = ax.z;
    m.m[1][0] = ay.x;
    m.m[1][1] = ay.y;
    m.m[1][2] = ay.z;
    m.m[2][0] = n.x;
    m.m[2][1] = n.y;
    m.m[2][2] = n.z;
    m.m[3][0] = m.m[3][1] = m.m[3][2] = 0.0;
    return m;
}

//! Uniform scale factor carried by a transform (1.0 for a pure rotation or
//! translation).
//!
//! Vertices are handled by xform(), but values that are plain numbers rather
//! than points - a text height, a measured value - have to be scaled by hand,
//! and this is the factor to use.  It is the geometric mean of the two axis
//! scales, so a matrix that is not a similarity transform still yields one
//! usable number.
inline double uniformScaleOf(const Mat4& m)
{
    const double sx = std::sqrt(m.m[0][0] * m.m[0][0] + m.m[1][0] * m.m[1][0]);
    const double sy = std::sqrt(m.m[0][1] * m.m[0][1] + m.m[1][1] * m.m[1][1]);
    return std::sqrt(std::max(sx * sy, 0.0));
}

//! Matrix for an INSERT: scale(sx,sy,sz) -> rotateZ(rotation) -> translate.
inline Mat4 insertMatrix(const Vec3& insPt, const Vec3& scale, double rotation)
{
    double c = std::cos(rotation);
    double s = std::sin(rotation);
    Mat4 M;
    M.m[0][0] = c * scale.x;
    M.m[0][1] = s * scale.x;
    M.m[1][0] = -s * scale.y;
    M.m[1][1] = c * scale.y;
    M.m[2][2] = scale.z;
    M.m[3][0] = insPt.x;
    M.m[3][1] = insPt.y;
    M.m[3][2] = insPt.z;
    M.m[3][3] = 1.0;
    return M;
}

// ---------------------------------------------------------------------------
//  Entity model
// ---------------------------------------------------------------------------

enum class EntType
{
    NONE,
    LINE,
    CIRCLE,
    ARC,
    ELLIPSE,
    POINT,
    INSERT,
    LWPOLYLINE,
    TEXT,
    MTEXT,
    SPLINE,
    SOLID,
    FACE3D,
    HATCH,
    DIMENSION,
    //! LEADER (引线标注): a polyline from the arrow head to the annotation,
    //! optionally with a hook line at the annotation end.  Unlike DIMENSION it
    //! is plain line work on a layer, so it goes through the layer compounds
    //! rather than the dimension-object pipeline.
    LEADER
};

//! Human readable name of an entity type (implemented in DWGExtract.cpp).
const char* typeLabel(EntType t);

//! One edge of a HATCH boundary path.  Coordinates are in the HATCH plane
//! (z = HATCH elevation), stored in block/model local space.
struct HatchSeg
{
    //! DXF 72 edge type: 1 LINE, 2 CIRCULAR ARC, 3 ELLIPTICAL ARC, 4 SPLINE.
    int curveType = 1;
    // LINE
    Vec3 p1, p2;
    // CIRCULAR ARC
    Vec3 center;
    double radius = 0.0;
    // ELLIPTICAL ARC: endpoint = major-axis endpoint relative to center,
    // ratio = minor/major axis ratio.
    Vec3 endpoint;
    double ratio = 1.0;
    // ARC / ELLIPTICAL ARC parameter range (radians, from the major axis).
    double startAngle = 0.0;
    double endAngle = 0.0;
    // DXF 73: 1 = counter-clockwise, 0 = clockwise.
    bool ccw = true;
    // SPLINE
    std::vector<Vec3> ctrlPts;
    std::vector<Vec3> fitPts;
};

//! One boundary loop of a HATCH entity.
struct HatchPath
{
    bool closed = true;
    //! Polyline path (DXF path flag bit 1): vertices + bulge to the next one.
    bool isPolyline = false;
    //! True if this path is an outer boundary loop (path flag bit 0 or bit 4).
    bool isOuter = true;
    std::vector<Vec3> polyPts;
    std::vector<double> polyBulges;
    //! Edge (segment) path: one entry per boundary edge.
    std::vector<HatchSeg> segs;
};

//! DWG STYLE (text style table) record.  TEXT/MTEXT entities reference a
//! style by name; the style carries the default font, width factor, oblique
//! angle and (optionally) a fixed text height.  Attributes missing on the
//! entity fall back to these values, then to hard-coded defaults.
struct TextStyle
{
    std::string name;
    //! Font file name of the style, usually an AutoCAD .shx file (may be
    //! empty for the built-in Standard style).
    std::string fontFile;
    //! Big font (large font / CJK) file name, e.g. "gbcbig.shx".
    std::string bigFontFile;
    //! Fixed text height; 0 = the height varies per entity.
    double textSize = 0.0;
    //! Horizontal stretch factor (width/height ratio).
    double widthFactor = 1.0;
    //! Oblique angle in degrees (positive leans to the right).
    double oblique = 0.0;
    //! DXF 70 generation flags: 1 = upside-down, 2 = backward (mirrored).
    int generation = 0;
    //! STYLE vertical text direction (DXF 70 bit 1).
    bool isVertical = false;
};

//! One pattern definition line family (DXF 78/79), used to rebuild the
//! hatch pattern lines.  The line family consists of all lines
//!   p = pt0 + k * offset  (k integer)  in direction (cos angle, sin angle).
struct HatchDefLine
{
    double angle = 0.0;  // radians
    Vec3 pt0;
    Vec3 offset;
    //! Alternating pen-up/pen-down dash lengths (empty = continuous).
    std::vector<double> dashes;
};

// ---------------------------------------------------------------------------
//  Linetype (LINETYPE) model
// ---------------------------------------------------------------------------

//! One dash element of a DWG LINETYPE pattern.
struct LineTypeDash
{
    //! Dash length in DWG units: positive = dash, negative = space, 0 = dot.
    double length = 0.0;
    //! Complex element: 0 = simple, 2 = shape (complex_shapecode = shape
    //! number), 4 = index into the strings area (see Dwg_LTYPE_dash).
    int shapeFlag = 0;
    //! Complex element code: shape number or strings-area index.
    int complexShapecode = 0;
    //! Embedded text of a complex dash (e.g. "GAS" of GAS_LINE).
    std::string text;
    //! Shape file (.shx) the embedded shape lives in (e.g. "ltypeshp.shx"),
    //! resolved from the dash's STYLE table reference.  Empty for text.
    std::string shapefile;
    //! Complex element placement: scale / rotation (radians) / offsets.
    double scale = 1.0;
    double rotation = 0.0;
    double xOffset = 0.0;
    double yOffset = 0.0;
    //! Embedded shape X span in shape-file units (e.g. ltypeshp ZIG = 2,
    //! TRACK1 = 56).  A zero-length shape dash reserves a run of width
    //! symWidth * scale (AutoCAD sizes it from the shape's bounding box);
    //! filled by the importer from the resolved .shx, 0 = unknown.
    double symWidth = 0.0;
    //! Horizontal stretch factor of the STYLE the embedded text uses
    //! (DXF 41 width factor).  Text glyphs (shapeFlag == 4) are scaled by
    //! this in X so the rendered width matches AutoCAD (e.g. "GAS" in the
    //! default Standard style renders ~1.5x wider than raw arial metrics).
    double widthFactor = 1.0;
};

//! A DWG LINETYPE table record: the name plus the dash pattern definition.
struct LineTypeInfo
{
    std::string name;
    std::string desc;
    //! Alignment code (always 'A'): dashes are aligned so that every line
    //! begins and ends with a dash (AutoCAD endpoint compensation).
    char alignment = 'A';
    //! Total length of one pattern cycle (sum of |dash| lengths).
    double patternLen = 0.0;
    std::vector<LineTypeDash> dashes;
};

//! Effective pattern scale for one line group.
//!
//! \a groupScale is the group's own DWGLineScale: LTSCALE times *that*
//! entity's linetype scale, resolved at import.  Entities sharing one linetype
//! routinely carry different per-entity scales (measured 0.2 .. 10 in a single
//! drawing), so this value is a property of the line, not of the linetype.
//! \a sharedOverride is DWGLineType::ScaleOverride, one multiplier applied on
//! top of every linked line, which is what makes "edit one definition, all
//! lines follow" meaningful for lines that start out at different scales.
//!
//! Every renderer must resolve the pattern through this helper.  Deriving it
//! from the shared definition alone - i.e. stroking every line at whichever
//! scale the *first* group that created the definition happened to have - is
//! what made 13 of 22 linked groups in a test drawing come out between 25x too
//! fine and 20x too coarse.
inline double effectiveLineScale(double groupScale, double sharedOverride)
{
    const double base = (groupScale > 1e-12) ? groupScale : 1.0;
    const double mult = (sharedOverride > 1e-12) ? sharedOverride : 1.0;
    return base * mult;
}

//! Quantize a dash pattern (world-unit lengths) into a 16-bit line pattern
//! for Coin3D SoDrawStyle::linePattern (level L1 of the mapping system).
//! Returns 0xffff for empty / degenerate patterns (CONTINUOUS).
//!
//! The pattern period is normalised to 16 cells: positive lengths mark
//! on-cells, negative lengths mark off-cells, and a dot (length == 0) forces
//! the first cell of its slot on so that point patterns such as "0,-6.35"
//! stay visible instead of quantising to all-off.  A dot does not advance
//! the position (its on-length is zero); the following gap does.
//!
//! This is a screen-space approximation of the true world-unit pattern.
//! Geometry-level expansion (L2) is the exact fallback that respects the
//! LTSCALE / CELTSCALE factors.
inline unsigned short quantizeLinePattern(const std::vector<LineTypeDash>& dashes)
{
    if (dashes.empty()) {
        return 0xffff;  // CONTINUOUS
    }
    double total = 0.0;
    bool anyInk = false;
    for (const LineTypeDash& d : dashes) {
        double a = std::fabs(d.length);
        total += a;
        if (d.length > 0.0 || d.length == 0.0) {
            anyInk = true;  // dash or dot
        }
    }
    if (total <= 0.0 || !anyInk) {
        return 0xffff;  // continuous / all-space
    }
    const double cell = total / 16.0;
    unsigned short pattern = 0x0000;
    double pos = 0.0;
    for (const LineTypeDash& d : dashes) {
        const double len = d.length;
        if (len == 0.0) {
            // Dot: occupy one on-cell at the current position.
            int b = static_cast<int>(std::floor(pos / cell + 1e-9));
            b = ((b % 16) + 16) % 16;
            pattern |= (unsigned short)(1u << (15 - b));
        }
        else {
            const double w = std::fabs(len);
            const int b0 = static_cast<int>(std::floor(pos / cell + 1e-9));
            const int b1 = static_cast<int>(std::floor((pos + w - 1e-9) / cell));
            if (len > 0.0) {  // dash: mark on-cells
                for (int b = b0; b <= b1; ++b) {
                    const int bb = ((b % 16) + 16) % 16;
                    pattern |= (unsigned short)(1u << (15 - bb));
                }
            }
            pos += w;
        }
    }
    if (pattern == 0x0000) {
        return 0xffff;  // degenerate -> continuous
    }
    return pattern;
}

// ---------------------------------------------------------------------------
//  Dimension (DIMENSION) model
// ---------------------------------------------------------------------------

//! DIMENSION sub-type (DXF 70 & 31): 0 linear, 1 aligned, 2 angular-2-line,
//! 3 diameter, 4 radius, 5 angular-3-point, 6 ordinate.
enum class DimType
{
    NONE,
    LINEAR,
    ALIGNED,
    ANG2LN,
    DIAMETER,
    RADIUS,
    ANG3PT,
    ORDINATE
};

//! Resolved DWG DIMSTYLE (dimension style table) record.
//!
//! All lengths are stored in world units *before* DIMSCALE is applied; the
//! caller multiplies by \a scale when generating geometry.  Attributes that
//! are missing in the style file fall back to the AutoCAD defaults shown
//! below.
struct DimStyle
{
    std::string name;           //!< table entry name (e.g. "ISO-25")
    double scale = 1.0;         //!< DIMSCALE, overall scale factor
    double arrowSize = 2.5;     //!< DIMASZ, arrow head length
    double tickSize = 0.0;      //!< DIMTSZ, architectural tick size
                                //!< (0 = arrows are used, non-zero = ticks
                                //!< replace the arrows and DIMASZ is ignored)
    double textHeight = 2.5;    //!< DIMTXT, text height
    double extOffset = 0.625;   //!< DIMEXO, extension line offset from origin
    double extLength = 1.25;    //!< DIMEXE, extension line past the dim line
    double textGap = 0.625;     //!< DIMGAP, gap between dim line and text
    double textOffset = 0.0;    //!< DIMTVP, text vertical position (0 = centered)
    double roundOff = 0.0;      //!< DIMRND, measurement rounding
    double leaderLen = 3.0;     //!< extra leader beyond the arc for R/D dims
    bool supExt1 = false;       //!< DIMSE1, suppress first extension line
    bool supExt2 = false;       //!< DIMSE2, suppress second extension line
    bool forceLine = false;     //!< DIMTOFL, draw dim line even if text outside
    bool textInside = false;    //!< DIMTIX, force text between extension lines
    int textPos = 0;            //!< DIMTAD: 0 centered, 1 above, 2 angle, 3 below
    double textAngle = 0.0;     //!< DIMTAD == 2: absolute text angle (radians)
    int precision = 2;          //!< DIMDEC, decimal places
    int zeroSuppression = 0;    //!< DIMZIN zero suppression:
                                //!< bit4 = suppress leading zeros,
                                //!< bit8 = suppress trailing zeros
    int unitType = 2;           //!< DIMLUNIT: 1 sci, 2 decimal, 3 eng, 4 arch, 5 frac
    unsigned long dimColor = 0; //!< DIMCLRD, dim line + arrows (0 = ByBlock)
    unsigned long extColor = 0; //!< DIMCLRE, extension lines (0 = ByBlock)
    unsigned long textColor = 0;//!< DIMCLRT, dim text (0 = ByBlock)
    std::string prefix;         //!< DIMPOST prefix (before <measurement>)
    std::string suffix;         //!< DIMPOST suffix
    std::string textStyle;      //!< DIMTXSTY, text style name
};

//! Intermediate model of one DIMENSION entity, independent of the source
//! library.  DWGDimension.cpp fills it from LibreDWG objects and computes the
//! final geometry with buildDimensionGeometry().
struct DimensionData
{
    DimType type = DimType::NONE;
    Vec3 extrusion = {0.0, 0.0, 1.0};
    // DIMENSION_COMMON
    Vec3 defPt;                  //!< def_pt (DXF 10, dim-line position point)
    Vec3 textMid;                //!< text_midpt (DXF 11)
    double elevation = 0.0;
    int flag = 0;                //!< DXF 70: type & 31, bit5 ordinate axis,
                                 //!<        bit7 = non-default text position
    std::string userText;        //!< user_text ("<>" = measurement placeholder)
    double textRotation = 0.0;   //!< text_rotation (radians)
    double horizDir = 0.0;       //!< horiz_dir (radians)
    double actMeasurement = 0.0; //!< act_measurement (world units)
    std::string dimStyleName;    //!< referenced DIMSTYLE table entry
    // LINEAR / ALIGNED / ANG3PT
    Vec3 xline1, xline2;         //!< xline1_pt / xline2_pt
    double oblique = 0.0;        //!< oblique_angle (radians)
    double dimRotation = 0.0;    //!< dim_rotation (radians, linear only)
    // ANG3PT
    Vec3 centerPt;               //!< center_pt
    // ANG2LN
    Vec3 xline1start, xline1end, xline2start, xline2end;
    // RADIUS / DIAMETER
    Vec3 firstArcPt;             //!< first_arc_pt (DXF 15)
    double leaderLen = 0.0;      //!< leader_len (DXF 40)
    // ORDINATE
    Vec3 featurePt;              //!< feature_location_pt
    Vec3 leaderEnd;              //!< leader_endpt
    bool ordX = false;           //!< ordinate measures the X axis
    // Angular value in radians (derived from the defining points).
    double angleValue = 0.0;
};

//! Intermediate model of one DWG entity, independent of the source library.
//! DWGExtract.cpp fills it from LibreDWG objects, DWGShape.cpp turns it into
//! an OCC shape.
struct Entity
{
    EntType type = EntType::NONE;
    std::string layer;
    int colorIndex = 0;
    unsigned long rgb = 0;
    // Line weight in 1/100 mm (0 = ByLayer, -1 = ByBlock, >0 = explicit).
    int lineWeight = 0;
    // Linetype name; empty or "ByLayer"/"ByBlock" means inherited.
    std::string ltype;
    // Entity-level linetype scale (DXF 48 / CELTSCALE override); 1.0 default.
    // The effective pattern length is dashLength * LTSCALE * ltypeScale.
    double ltypeScale = 1.0;

    // Points used by most types:
    //  LINE       pts[0], pts[1]
    //  LWPOLYLINE pts[0..n-1]
    //  POINT      pts[0]
    //  TEXT/MTEXT pts[0] = insertion point
    //  SOLID/3DFACE pts[0..3]
    //  SPLINE     pts = control points
    std::vector<Vec3> pts;

    //! Plane normal (extrusion, DXF 210-230) of a 2D entity's coordinates.
    //! (0,0,1) = the stored numbers are already world coordinates; anything
    //! else is an OCS that ocsToWorld() has to rotate into place.  (0,0,-1) is
    //! the one that shows up in real drawings, from AutoCAD MIRROR.
    Vec3 extrusion = {0.0, 0.0, 1.0};

    Vec3 center;
    double radius = 0.0;
    double startAngle = 0.0;
    double endAngle = 0.0;

    Vec3 smAxis;
    double axisRatio = 1.0;

    Vec3 insPt;
    Vec3 scale;
    double rotation = 0.0;
    std::string blockName;

    std::vector<double> bulges;
    bool closed = false;
    // POLYLINE flagged as curve-fit / spline-fit (rendered as a smooth spline).
    bool fitSpline = false;
    double elevation = 0.0;

    double degree = 0.0;
    bool splineClosed = false;
    std::vector<Vec3> fitPts;

    std::string text;
    double textHeight = 0.0;

    // TEXT alignment (matches DWG enumerators).
    int textHAlign = 0;  // 0 left, 1 center, 2 right, 3 aligned, 4 middle, 5 fit
    int textVAlign = 0;  // 0 baseline, 1 bottom, 2 middle, 3 top
    // MTEXT attachment point: 1 top-left ... 9 bottom-right (default 1).
    int mtextAttachment = 1;
    // MTEXT measured extents (world units), used to align multi-line text.
    double mtextWidth = 0.0;
    double mtextHeight = 0.0;

    //! Text appearance attributes (style + metrics) shared by TEXT/MTEXT.
    struct TextAttrs
    {
        //! STYLE table entry name this entity references.
        std::string styleName;
        //! Style font file (e.g. "txt.shx"), empty for the default style.
        std::string fontFile;
        //! Style big font file (e.g. "gbcbig.shx"), empty if none.
        std::string bigFontFile;
        //! Horizontal stretch factor (width/height ratio), entity or style.
        double widthFactor = 1.0;
        //! Oblique angle in degrees (positive leans to the right).
        double oblique = 0.0;
        //! DXF 71 text generation flags: 1 = upside-down, 2 = backward.
        int generation = 0;
        //! STYLE vertical text direction (DXF 70 bit 1).
        bool isVertical = false;
        //! TEXT Aligned(3) / Fit(5): second baseline point.
        Vec3 alignPt2;
        bool hasAlignPt2 = false;
        //! MTEXT boundary rectangle (layout width / height, world units).
        double mtextRectW = 0.0;
        double mtextRectH = 0.0;
        //! MTEXT line spacing factor and style (1 = at least, 2 = exactly).
        double lsFactor = 1.0;
        int lsStyle = 1;
        //! Text plane normal (extrusion), defaults to +Z.
        Vec3 normal = {0.0, 0.0, 1.0};
    };
    TextAttrs textAttrs;

    // HATCH ---------------------------------------------------------------
    std::vector<HatchPath> hatchPaths;
    std::vector<HatchDefLine> hatchDefLines;
    double hatchAngle = 0.0;   // DXF 52, overall pattern rotation (radians)
    double hatchScale = 1.0;   // DXF 41, overall pattern scale
    bool hatchSolid = false;   // solid fill flag
    std::string hatchPattern;  // pattern name (ANSI31, SOLID, ...)
    //! HATCH pattern type (DWG pattern_type / DXF 76): 0 = user defined,
    //! 1 = predefined, 2 = custom.  A *user-defined* hatch carries no
    //! definition lines - AutoCAD derives them from the angle and the spacing
    //! - so this field says whether an empty hatchDefLines is expected.
    int hatchPatternType = -1;
    //! HATCH gradient fill (AutoCAD 渐变色).  Such a hatch has
    //! is_solid_fill = 0 *and* no definition lines and *and* no pattern in
    //! acad.pat - its name is "SOLID" - so nothing in the pattern data says
    //! "draw something": only this flag plus the colour ramp does.  Ignoring it
    //! left the whole fill out, which is what the 88 hatches reported as
    //! missing turned out to be.
    bool hatchGradient = false;
    //! Number of gradient stops (1 = single-colour tint ramp, 2+ = blend).
    int hatchGradientColors = 0;
    //! ACI of the first gradient stop (0 when the file carries raw RGB only).
    int hatchGradientAci = 0;
    //! Raw RGB of the first gradient stop, in LibreDWG's packed form.
    unsigned long hatchGradientRgb = 0;
    //! Gradient shape name (LINEAR, CYLINDER, SPHERICAL, ...).
    std::string hatchGradientName;

    //! LEADER: draw a closed-filled arrow head (arrowhead_on / DXF 71).
    bool leaderArrow = false;
    //! LEADER arrow length, i.e. DIMASZ.  The entity carries its own copy
    //! instead of referring to a DIMSTYLE, so no style lookup is needed.
    double leaderArrowSize = 0.0;
    //! LEADER arrow head type (DXF 77): 0 = closed filled (the default).
    int leaderArrowType = 0;
    //! LEADER: short hook line drawn at the annotation end (DXF 75).
    bool leaderHook = false;
    //! Hook line direction: true when the hook runs along X (DXF 74).
    bool leaderHookX = false;
    //! LEADER path type (DXF 72): 0 = straight segments, 1 = spline.
    int leaderPathType = 0;
    //! Name of the DIMSTYLE the entity references.  A LEADER takes its arrow
    //! length from that style (DIMASZ x DIMSCALE) when it does not carry its
    //! own copy - the tested file stores 0 on the entity itself.
    std::string dimStyleName;
    //! LEADER annotation box (DXF 40/41) and the two offsets that describe
    //! where the annotation sits relative to the leader (DXF 212/213).
    //! Diagnostics only, but they are what settles which end of the vertex
    //! list carries the arrow: the list itself does not say.
    double leaderBoxWidth = 0.0;
    double leaderBoxHeight = 0.0;
    Vec3 leaderInsptOffset = {0.0, 0.0, 0.0};
    Vec3 leaderEndProj = {0.0, 0.0, 0.0};

    // DIMENSION ----------------------------------------------------------
    DimensionData dim;
};

}  // namespace SWDWG
