//! Internal cross-translation-unit API of the pure C++ DWG importer.
//!
//! The importer used to live in a single 3k+ line DWGImporter.cpp file and
//! was split along the pipeline stages so each entity can be analysed and
//! debugged independently:
//!
//!   DWGExtract.cpp    LibreDWG object       -> Entity   (per-type parsing)
//!   DWGHatch.cpp      HATCH pattern lines   -> 2D segments
//!   DWGShape.cpp      Entity                -> OCC shape (per-type building)
//!   DWGDimstyle.cpp   DIMSTYLE table        -> DimStyle struct
//!   DWGDimension.cpp  DIMENSION entity      -> DimensionData + geometry
//!   DWGImporter.cpp   document import orchestration + public entry point
//!
//! Everything declared here lives in the SWDWG namespace (not an anonymous
//! namespace) so that it can be shared between the translation units.

#pragma once

// Forward declaration for createDimensionObject() below.  FreeCAD App types
// only appear as pointers in this header, so a forward declaration is enough
// (the implementing TU includes App/Document.h itself).
namespace App
{
class Document;
}

#include "DWGTypes.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

// --- LibreDWG config alignment -------------------------------------------
// The libredwg public headers depend on a few config macros that must match
// the build of libredwg.lib (see build-vs2022/src/config.h). We set the ones
// that affect the public struct layout / text types here so that our ABI
// matches the library exactly.  This must be defined before dwg_api.h in
// every translation unit that includes this header.
#ifndef HAVE_WCHAR_H
#  define HAVE_WCHAR_H 1
#endif
#ifndef SIZEOF_WCHAR_T
#  define SIZEOF_WCHAR_T 2
#endif
// --------------------------------------------------------------------------

extern "C" {
#include <dwg_api.h>
}

// dwg.h does `#define restrict __restrict` for MSVC so that its C99-qualified
// prototypes compile as C++. That macro must not leak into the Qt/MSVC headers
// included later in the same translation unit: they spell
// `__declspec(restrict)` (Q_DECL_MALLOCLIKE in qarraydata.h), which the macro
// turns into the invalid `__declspec(__restrict)` and fails with C2485.
#ifdef restrict
#  undef restrict
#endif

#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

namespace SWDWG
{

// ---------------------------------------------------------------------------
//  Diagnostics counters (shared between DWGShape.cpp and DWGImporter.cpp)
// ---------------------------------------------------------------------------

// Spline-fit polyline rendering diagnostics (imports are single-threaded).
extern int s_fitPolyTried;
extern int s_fitPolyQuadOk;
extern int s_fitPolyCubicOk;
extern int s_fitPolyNotDone;
extern int s_fitPolyCurveNull;
extern int s_fitPolyEdgeNull;
extern int s_fitPolyException;

// ---------------------------------------------------------------------------
//  Text / color helpers (implemented in DWGExtract.cpp)
// ---------------------------------------------------------------------------

//! AutoCAD Color Index (ACI) -> packed 0xRRGGBB.
unsigned long aciRgb(int idx);

//! Pick a CJK-capable font family name.
std::string findCjkFont();

//! Pick a TrueType font name for DWG text.
std::string dwgFontName(const std::string& text);

//! Map an AutoCAD text style font (usually .shx) plus optional big font to a
//! TrueType family name for rendering (implemented in DWGExtract.cpp).
std::string mapTextFont(const std::string& fontFile,
                        const std::string& bigFontFile,
                        const std::string& utf8Text);

//! Decode a raw DWG string to UTF-8 using the declared code page.
std::string decodeDwgString(const std::string& raw, int dwgCodepage);

// splitUtf8Chars() is declared in DWGText.h (libredwg-free) so that the Gui
// module can use it without depending on the libredwg headers.

// ---------------------------------------------------------------------------
//  Diagnostics helpers (implemented in DWGImporter.cpp)
// ---------------------------------------------------------------------------

//! Compact bounding-box string of a shape for diagnostics.
std::string shapeBBox(const TopoDS_Shape& s);

//! Shapes that land near the origin (instead of the drawing area) indicate a
//! lost INSERT transform.  Log the first few with their provenance.
void checkOriginShape(const char* what, const std::string& layer,
                      const TopoDS_Shape& s, const Vec3* insPt = nullptr,
                      const Vec3* scale = nullptr, double rotation = 0.0);

// ---------------------------------------------------------------------------
//  LibreDWG object -> Entity (implemented in DWGExtract.cpp)
// ---------------------------------------------------------------------------

// A VERTEX_2D/VERTEX_3D that belongs to a POLYLINE.  We collect these into a
// per-owner table because the generic owned-entity chain of a POLYLINE is not
// reliably decoded by libredwg.
struct PolyVert
{
    Vec3 p;
    double bulge = 0.0;
    // VERTEX_2D/3D DXF 70 flag: bit1 = extra/curve-fit vertex,
    // bit8 = spline-fit point, bit16 = spline frame control point.
    unsigned char flag = 0;
};
using PolyVertMap = std::map<const Dwg_Object*, std::vector<PolyVert>>;

//! Extract one DWG entity object into the intermediate Entity model.
//! Returns false for unsupported / degenerate entities.
//! \a textStyles (may be null) is the STYLE table built by parse(); TEXT and
//! MTEXT entities use it to resolve their text style reference.
bool extractEntity(const Dwg_Data* dwg, const Dwg_Object* obj, Entity& e,
                   const PolyVertMap& polyVerts, int codepage,
                   const std::map<std::string, TextStyle>* textStyles = nullptr);

// ---------------------------------------------------------------------------
//  DIMSTYLE table (implemented in DWGDimstyle.cpp)
// ---------------------------------------------------------------------------

//! Scan every DIMSTYLE object of the DWG and build a name -> DimStyle table.
//! DIMENSION entities reference a style by name; attributes missing on the
//! style fall back to the AutoCAD defaults baked into DimStyle.
std::map<std::string, DimStyle> parseDimStyles(const Dwg_Data* dwg,
                                               int codepage);

// ---------------------------------------------------------------------------
//  DIMENSION entities (implemented in DWGDimension.cpp)
// ---------------------------------------------------------------------------

//! Map one LibreDWG DIMENSION object (any sub-type) into the intermediate
//! DimensionData model.  Returns false for unsupported objects.
bool extractDimension(const Dwg_Data* dwg, const Dwg_Object* obj,
                      DimensionData& dim, int codepage);

//! Create a DWGDimension document object from an extracted DIMENSION entity.
//! The geometry (dimension line, extension lines, arrow heads, text anchor)
//! is computed here via buildDimensionGeometry() - the importer only supplies
//! the raw Entity and the DIMSTYLE table.
//! \a parent is the block INSERT matrix (world = local x parent), \a counters
//! supplies the per-type object counters (key "Dimension").
void createDimensionObject(
    App::Document* doc, const Entity& e, const Mat4& parent,
    const std::map<std::string, DimStyle>& dimStyles,
    const std::map<std::string, TextStyle>& textStyles,
    int codepage,
    std::map<std::string, int>& counters, int& created, int& skipped);

// ---------------------------------------------------------------------------
//  HATCH pattern lines (implemented in DWGHatch.cpp)
// ---------------------------------------------------------------------------

//! Return a built-in hatch pattern definition for common AutoCAD pattern
//! names.  DWG files often reference predefined patterns (e.g. ANSI31,
//! AR-B816) by name only; libredwg does not embed the .pat definition, so we
//! supply a small set of standard definitions ourselves.
std::vector<HatchDefLine> builtinHatchPattern(const std::string& name);

//! Generate hatch pattern lines: for each pattern definition line family,
//! sweep parallel lines across the boundary bounding box and clip them to
//! the boundary polygons.  Returns model-space (pre-transform) 2D segments.
std::vector<std::pair<Vec3, Vec3>> hatchPatternLines(
    const std::vector<std::vector<Vec3>>& outerPolys,
    const std::vector<std::vector<Vec3>>& innerPolys,
    const std::vector<HatchDefLine>& deflines, double scale, double rot,
    double z);

// ---------------------------------------------------------------------------
//  OCC shape building (implemented in DWGShape.cpp)
// ---------------------------------------------------------------------------

//! Straight segment or bulge arc edge (bulge b = tan(theta/4)).
TopoDS_Edge makeSegEdge(const Vec3& p1, const Vec3& p2, double b,
                        const Mat4& M);

//! Circle or ellipse through a center and two perpendicular axis vectors.
TopoDS_Shape makeCircleLike(const Vec3& c, const Vec3& vx, const Vec3& vy);

//! Build the OCC shape for one entity (input coordinates are already in
//! final model space after applying transform M).
//!
//! \a M is taken by value because the builder folds the entity's OCS rotation
//! (see Entity::extrusion) into it before building - a 2D entity whose
//! extrusion is not +Z would otherwise be placed mirrored.
TopoDS_Shape buildShape(const Entity& e, Mat4 M);

}  // namespace SWDWG
