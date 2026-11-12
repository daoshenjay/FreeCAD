//! Compiled shape file (.shx) parser - pure C++, no FreeCAD / OCCT dependency.
//!
//! AutoCAD complex linetypes may embed shapes (e.g. "[TRACK1,ltypeshp.shx,
//! S=1]" in TRACKS).  The DWG only stores the shape number
//! (LineTypeDash::complexShapecode) plus the STYLE reference that names the
//! .shx file; the shape body lives in the external shape file.  This module
//! parses that compiled binary format and turns each shape definition into a
//! polyline outline (arcs are sampled), so the L2 cutter
//! (DWGLineGroup::addSymbolGeometry) can draw the real symbol instead of a
//! placeholder diamond.
//!
//! Shape coordinates are in "shape units": a shape is authored on a grid of
//! 1/8-inch cells and rendered at scale S by multiplying every vector by S
//! (AutoCAD complex-linetype semantics).  The parser keeps the raw units; the
//! caller scales.
//!
//! The parser is compiled into the importer and into the standalone unit test
//! (tests/test_shape.cpp) as identical code.

#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! One polyline segment of a shape outline, in shape file units.
struct ShapeSeg
{
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    //! True when this segment is one of the short chords a decoded ARC was
    //! sampled into.  Pure stroke/move segments are false.  Renderers that
    //! can draw real curves should skip arcChord segments and draw the
    //! matching ShapeArc instead; polyline renderers (text glyphs etc.) keep
    //! them and are unchanged.
    bool arcChord = false;
};

//! One circular arc of a shape outline, kept alongside its sampled chords so
//! OCC-based consumers (SHX viewer, linetype symbols) can build a real
//! circular edge instead of tens of tiny line segments.
//! Angles are degrees on the shape-unit grid; the arc runs counter-clockwise
//! from \a startDeg for \a sweepDeg (a clockwise source arc is stored as its
//! CCW equivalent; sweepDeg >= 359.999 means a full circle).
struct ShapeArc
{
    double cx = 0.0;      //!< arc centre X (shape units)
    double cy = 0.0;      //!< arc centre Y (shape units)
    double r = 0.0;       //!< radius (shape units)
    double startDeg = 0.0;  //!< start angle, degrees (0..360)
    double sweepDeg = 0.0;  //!< CCW sweep, degrees (0..360)
};

//! One shape definition parsed from a .shx file.
struct ShapeDef
{
    int number = 0;       //!< shape number (same as the map key)
    std::string name;     //!< shape name (e.g. "TRACK1")
    //! Pen position at the end of the definition, in shape units.  For a
    //! font glyph this is the advance: the horizontal distance AutoCAD moves
    //! to the next character (often the definition ends with a pen-up move
    //! to (advance, 0)).  The bbox width cannot be used ('I' is a zero-width
    //! vertical stroke but still advances ~4 units).
    double penEndX = 0.0;
    double penEndY = 0.0;
    //! Outline segments (drawing only, pen moves are not stored).
    //! Arcs are sampled into short chords.  Kept for back-compat and as a
    //! flat fallback when the stroke grouping below is empty.
    std::vector<ShapeSeg> segs;
    //! Outline strokes grouped by pen-up boundaries: each inner vector is one
    //! continuous "draw" run between two moves.  A glyph like "8" has three
    //! strokes (top circle, crossbar, bottom circle).  The viewer closes each
    //! stroke into a wire and turns it into a planar face so the glyph reads
    //! as a filled character instead of a tangled polyline.  Subshape (code 7)
    //! segments are still kept in `segs` but not in `strokes`.
    std::vector<std::vector<ShapeSeg>> strokes;
    //! Raw byte codes (after the null-terminated name).  Kept so subshape
    //! references (code 7) can recurse into the definition without re-parsing
    //! the file; subshapes of subshapes are expanded up to a fixed depth.
    std::vector<unsigned char> codes;
    //! Original circular arcs of the definition (see ShapeArc).  Every arc
    //! also contributes its sampled chords to \a segs with arcChord=true, so
    //! polyline consumers stay unchanged while curve-capable consumers can
    //! reconstruct exact arcs.
    std::vector<ShapeArc> arcs;
};

//! A whole shape file: shape number -> definition.
using ShapeFile = std::map<int, ShapeDef>;

//! X span of a shape outline in shape-file units (max - min of every segment
//! endpoint X; 0 for a missing / empty / degenerate shape).  A zero-length
//! shape dash of a complex linetype reserves a run of exactly this width
//! scaled by S: AutoCAD sizes the element from the shape itself
//! ("the shift ... computed from the end of the linetype definition vertex"
//! and the occupied run length = shape X width x S).  E.g. ltypeshp ZIG
//! spans [0,2] -> 2, TRACK1 spans [0,56] -> 56.
inline double shapeXSpan(const ShapeFile& sf, int number)
{
    const auto it = sf.find(number);
    if (it == sf.end()) {
        return 0.0;
    }
    bool have = false;
    double xmin = 0.0;
    double xmax = 0.0;
    for (const ShapeSeg& s : it->second.segs) {
        if (!have) {
            xmin = std::min(s.x0, s.x1);
            xmax = std::max(s.x0, s.x1);
            have = true;
        }
        else {
            xmin = std::min(xmin, std::min(s.x0, s.x1));
            xmax = std::max(xmax, std::max(s.x0, s.x1));
        }
    }
    return have ? xmax - xmin : 0.0;
}

//! Parse a compiled .shx file from disk.  Returns an empty map when the file
//! is missing or malformed.
ShapeFile parseShapeFile(const std::string& path);

//! Parse shape data from an in-memory buffer (used by the unit test so it can
//! hand-build a byte stream without writing a file).
ShapeFile parseShapeFileBytes(const unsigned char* data, std::size_t size);

//! Parse a single shape definition body (after the header bytes): reads the
//! null-terminated name, then decodes the shape byte codes into segments.
//! Used by the unit test to decode a hand-written definition directly.
//! \a file, when non-null, supplies the definitions for subshape references
//! (code 7); passing null leaves subshapes unexpanded.
ShapeDef parseShapeBody(const unsigned char* data, std::size_t size,
                        const ShapeFile* file = nullptr);

//! Search the common AutoCAD support locations for a shape file.  \a name is
//! the file name from the DWG STYLE record (e.g. "ltypeshp.shx", may carry a
//! directory).  \a extraPaths are tried before the standard locations
//! (AutoCAD support path files, the current directory).  Returns the first
//! existing file, or an empty string.
std::string findShapeFile(const std::string& name,
                          const std::vector<std::string>& extraPaths);

}  // namespace SWDWG
