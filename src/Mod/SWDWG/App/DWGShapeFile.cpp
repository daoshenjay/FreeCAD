//! Compiled shape file (.shx) parser - implementation.
//!
//! See DWGShapeFile.h for the design.  The parser is pure C++ (no FreeCAD /
//! OCCT dependency) so it compiles into the importer and into the standalone
//! unit test identically.
//!
//! Two container layouts exist in the wild and both are handled:
//!
//! New layout (signature 0x0BF8, used by fonts shipped with newer AutoCAD):
//!   bytes 0-1   LE signature 0x0BF8
//!   bytes 2-3   LE version (1 or 2)
//!   bytes 4-5   LE number of shapes
//!   bytes 6-7   LE max bytes of any shape definition
//!   bytes 8-19  reserved
//!   bytes 20+   location table: 3 bytes (big-endian 24-bit offset) per
//!               shape number slot, up to 326 slots
//! A shape definition starts at the location-table offset:
//!   byte 0      shape number (0-255) or 0xFE (>= 256: 2 following BE bytes)
//!   bytes 1-2   BE length (number of byte-code bytes, name not included)
//!   name        null-terminated shape name
//!   data        the byte codes below, 'length' bytes
//!
//! Old layout (header text "AutoCAD-86 shapes", ltypeshp.shx has shipped
//! in this form since AutoCAD 2.x):
//!   bytes 0-23  ASCII header "AutoCAD-86 shapes 1.1" CR LF 0x1A
//!   bytes 24-29 first shape number, last shape number, shape count (LE)
//!   bytes 30+   location table: 'count' entries of
//!               <shape number:2 LE><definition length:2 LE>
//!   data        'count' definitions in order:
//!               <null-terminated name><byte codes>
//!               where 'definition length' covers the name and the codes
//!   tail        "EOF"
//!
//! Shape byte codes (identical in both layouts):
//!   single-byte vector: high nibble = length 1-15 in shape units, low nibble
//!               = direction 0-15 of the 16-direction table (0 = east, 1 =
//!               22.5 deg, 2 = 45 deg, ..., numbered counter-clockwise).
//!               Always a pen-down draw.  (The spec-byte text form is
//!               "0<length><direction>", e.g. 014 = length 1, direction 4
//!               (north); compiled into one byte this is (length << 4) |
//!               direction.)
//!   two-byte codes: the Special Codes, bytes 0x00..0x0E.
//!   0x00 / 0x0F 0x0F  end of definition
//!   0x01 / 0x02   draw / move (pen down / pen up).  Per AutoCAD's official
//!                 "Special Codes Reference", 0x01 is "Activate Draw mode
//!                 (pen down)" and 0x02 is "Deactivate Draw mode (pen up)":
//!                 they are MODE SWITCHES and carry NO displacement byte of
//!                 their own.  The single-byte vector that usually follows
//!                 (high nibble length 1-15, low nibble direction 0-15, NOT
//!                 a plain length) is then an independent command executed
//!                 under that pen state - ltypeshp TRACK1 "14 02 1C 01 1C 00"
//!                 relies on this: 0x1C = length 1 direction 12 (south)
//!                 draws the two vertical stubs at +-1 that AutoCAD shows.
//!                 Treating that vector as "the parameter of 01/02" gives the
//!                 same geometry, which is why font SHX files keep working.
//!                 CRITICAL: when the byte after 01/02 is itself a Special
//!                 Code (0x00..0x0F, other than the 08/09 displacement
//!                 prefixes) it is a NEW command, never a parameter - e.g.
//!                 AAA.shx writes "02 03 0A" = pen up, then scale DOWN by 10.
//!                 Swallowing the 03 here made its 0A operand look like a
//!                 standalone octant arc and drove the scale into a 1e-6
//!                 runaway (see DWG docs 18-06, bug list item 9).
//!   0x03 / 0x04   draw-scale / scale, one multiplier parameter
//!   0x05 / 0x06   push / pop position (no parameters)
//!   0x07        subshape: 2-byte BE shape number + optional 1-byte scale;
//!               expanded from the same file, position/scale restored after
//!   0x08        X-Y displacement: ONE signed (dx,dy) byte pair - AutoCAD
//!               "8,dx,dy" carries no count; coordinates are -128..127
//!   0x09        X-Y multiple: repeated (dx,dy) byte pairs terminated by
//!               the sentinel (0,0) - AutoCAD "9,dx1,dy1,dx2,dy2,...,(0,0)"
//!   0x0A        octant arc: radius byte + octant byte sSSSCCCC
//!               (s = sign bit: 1 = clockwise, SSS = start octant, CCCC =
//!               octants swept, 0 = full circle)
//!   0x0B        fractional arc, 5 following bytes
//!   0x0C        conic (bulge) arc: offset byte + arc code (+ its data) + end
//!               offset byte
//!   0x0D        multiple arcs, count byte + that many arc codes
//!   0x0E        vertical text: horizontal text skips the next code
//!               instruction (see skipOneCode); vertical would run it
//!   0x0F        escape (0x0F 0x0F = end of definition)
//!
//! The drawing position advances by the scale factor (codes 3/4) times the
//! vector length; shape units are then scaled by the caller (S) when drawn.
//! The octant / fractional / conic / multi arc radii use shape units as
//! well; the conic-arc offset bytes add a fraction of the radius (high
//! nibble, 1/256ths) and an angular correction (low nibble, 1/256ths of 45
//! degrees).

#include "DWGShapeFile.h"
#include "PathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

namespace SWDWG
{

namespace
{

constexpr double SHX_PI = 3.14159265358979323846;

//! 16-direction displacement table for the single-byte vector codes (index
//! = AutoCAD direction code 0-15).
//!
//! AutoCAD's "16 directions" are named like compass points in 22.5-degree
//! increments, but the displacement is a GRID MOVE, not a unit vector on that
//! angle.  The grid has half-unit steps:  direction 1 is (1, 0.5)
//! (arctan 0.5 = 26.565 deg), direction 3 is (0.5, 1) (63.435 deg), and so
//! on.  Autodesk's own table in the Customization Guide reads exactly:
//!     0 (1,0)  1 (1,0.5)  2 (1,1)  3 (0.5,1)  4 (0,1)
//!     5 (-0.5,1)  6 (-1,1)  7 (-1,0.5)  8 (-1,0)  9 (-1,-0.5)
//!     10 (-1,-1)  11 (-0.5,-1)  12 (0,-1)  13 (0.5,-1)
//!     14 (1,-1)  15 (1,-0.5)
//!
//! This matters: ltypeshp.shx BAT = 25 0A 02 C4 2B 00.  Code 25 (length 2,
//! direction 5) must give the displacement (-1, 2) = 116.565 deg - that is
//! what AutoCAD actually draws (measured: 116.565 deg).  A tan(22.5) = 0.414
//! table gives (-0.828, 2) = 112.5 deg, which is wrong.  It also makes the
//! BAT base 2.0 units wide instead of 2.343, which matches the linetype's
//! 2*|X offset| join width exactly.
constexpr double DIR_DX[16] = {
    1.0, 1.0, 1.0, 0.5, 0.0, -0.5, -1.0, -1.0,
    -1.0, -1.0, -1.0, -0.5, 0.0, 0.5, 1.0, 1.0};
constexpr double DIR_DY[16] = {
    0.0, 0.5, 1.0, 1.0, 1.0, 1.0, 1.0, 0.5,
    0.0, -0.5, -1.0, -1.0, -1.0, -1.0, -1.0, -0.5};

//! Subshape recursion guard (self-referencing shape files must not hang).
constexpr int kMaxSubshapeDepth = 8;

inline double toRad(double deg)
{
    return deg * SHX_PI / 180.0;
}

//! Mutable state while decoding one shape body: current pen, scale factor,
//! push/pop stack, stroke grouping and the vector-repetition cache.
struct DecodeCtx
{
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;
    double lastDx = 0.0;
    double lastDy = 0.0;
    bool havePrev = false;
    //! Pen state.  AutoCAD shapes start with the pen DOWN; code 01 (draw)
    //! keeps it down while code 02 (move) lifts it.  Vector codes, X-Y
    //! displacements (08/09) and arcs draw ONLY when the pen is down -
    //! with the pen up they move the pen to the target without emitting a
    //! segment.  Font glyphs (txt.shx / romans.shx) begin with a 02 lift to
    //! the first stroke's origin, so skipping pen-up moves is what removes
    //! the spurious "start" chord and the trailing width mark that every
    //! glyph would otherwise show.
    bool penDown = true;
    double stack[16] = {0.0};
    int sp = 0;
    bool sawSubshape = false;
    //! Stroke grouping: addSeg opens a stroke on first use; the main loop
    //! calls flushStroke() at pen-up / end-of-shape boundaries so each
    //! contiguous pen-down run becomes its own stroke in def.strokes.
    std::vector<ShapeSeg>* cur = nullptr;

    void flushStroke() { cur = nullptr; }
};

//! Append a pen-down segment, snapping zero-length chords.  Also pushes
//! the segment into the current stroke of \a ctx (opening a new one if no
//! stroke is open).  Pen-up boundaries (code 02 / end-of-shape) must call
//! ctx.flushStroke() to close the current stroke before the next draw.
//! \a chord marks the segment as the sampled chord of a circular arc so
//! curve-capable renderers can skip it and draw the real ShapeArc instead.
void addSeg(ShapeDef& def, DecodeCtx& ctx, double x0, double y0,
            double x1, double y1, bool chord = false)
{
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    if (dx * dx + dy * dy < 1e-12) {
        return;  // zero-length move
    }
    ShapeSeg s{x0, y0, x1, y1};
    s.arcChord = chord;
    def.segs.push_back(s);
    if (!ctx.cur) {
        def.strokes.emplace_back();
        ctx.cur = &def.strokes.back();
    }
    ctx.cur->push_back(s);
}

//! Sample a circular arc into short chords AND keep the exact arc definition
//! in \a def.arcs.  (cx, cy) centre, r radius, from angle a0 to a1 (degrees;
//! a clockwise source arc arrives as a1 < a0).  The current pen position
//! (px, py) must lie on the arc at a0; it is advanced.  The chord segments
//! are tagged arcChord so curve renderers can substitute the ShapeArc.
void sampleArc(ShapeDef& def, DecodeCtx& ctx, double& px, double& py,
               double cx, double cy, double r, double a0, double a1)
{
    if (r < 1e-9) {
        return;
    }
    double sweep = a1 - a0;
    if (std::fabs(sweep) < 1e-9) {
        return;
    }
    // Record the exact arc.  Normalise to a counter-clockwise sweep so every
    // consumer gets one convention regardless of the source direction.
    {
        double start = a0;
        double s = sweep;
        if (s < 0.0) {
            start = a1;
            s = -s;
        }
        start = std::fmod(start, 360.0);
        if (start < 0.0) {
            start += 360.0;
        }
        def.arcs.push_back(ShapeArc{cx, cy, r, start, s});
    }
    // Keep the chords short: one per 5 degrees, at least 4.
    int n = static_cast<int>(std::ceil(std::fabs(sweep) / 5.0));
    if (n < 4) {
        n = 4;
    }
    for (int i = 1; i <= n; ++i) {
        const double a = a0 + sweep * (double)i / (double)n;
        const double x = cx + r * std::cos(toRad(a));
        const double y = cy + r * std::sin(toRad(a));
        addSeg(def, ctx, px, py, x, y, true);
        px = x;
        py = y;
    }
}

//! Octant arc (code 0x0A).  \a p points at the radius/octant bytes.  \a radFrac
//! (conic-arc radius adjustment, 1/256ths) and \a angAdj (degrees) tweak the
//! radius and the starting angle.
//!
//! Layout: <radius byte (shape units)> <sSSSCCCC> where s = sign bit
//! (1 = clockwise), SSS = starting octant (0-7), CCCC = octants swept
//! (0 = full circle, 1-8 otherwise).
bool parseOctantArc(const unsigned char* data, std::size_t size,
                    std::size_t& p, DecodeCtx& ctx, ShapeDef& def,
                    double radFrac = 0.0, double angAdj = 0.0)
{
    if (p + 1 >= size) {
        return false;
    }
    const unsigned char radByte = data[p++];  // radius in shape units
    const unsigned char o = data[p++];        // sSSSCCCC
    // SCALE (codes 3/4) scales vector displacements; it scales arc radii too
    // (AutoCAD shape semantics).  Forgetting ctx.scale here inflated every
    // arc drawn while a small scale was active (AAA.shx glyphs draw fine
    // details at scale 0.1..0.001), which turned a compact chain of arcs
    // into a growing spiral tens of thousands of units wide.
    const double r = (double)radByte * (1.0 + radFrac) * ctx.scale;
    const bool cw = (o & 0x80) != 0;
    const double s = (double)((o >> 4) & 0x07);
    const int cnt = o & 0x07;  // 0 = full circle
    const double a0 = 45.0 * s + angAdj;
    double sweep;
    if (cnt == 0) {
        sweep = cw ? -360.0 : 360.0;  // full circle
    }
    else {
        sweep = cw ? -(double)cnt * 45.0 : (double)cnt * 45.0;
    }
    const double a0r = toRad(a0);
    const double cx = ctx.x - r * std::cos(a0r);
    const double cy = ctx.y - r * std::sin(a0r);
    sampleArc(def, ctx, ctx.x, ctx.y, cx, cy, r, a0, a0 + sweep);
    return true;
}

//! Fractional arc (code 0x0B): 0B,so,eo,rh,rl,sSSSCCCC (5 bytes).
bool parseFracArc(const unsigned char* data, std::size_t size,
                  std::size_t& p, DecodeCtx& ctx, ShapeDef& def,
                  double radFrac = 0.0, double angAdj = 0.0)
{
    if (p + 4 >= size) {
        return false;
    }
    const unsigned char so = data[p];
    const unsigned char eo = data[p + 1];
    const unsigned char rh = data[p + 2];
    const unsigned char rl = data[p + 3];
    const unsigned char fl = data[p + 4];
    p += 5;
    // high_radius is a full byte (0 when the radius fits in one byte).
    // Radius is scaled by the active SCALE factor like every other length
    // (see parseOctantArc) so 0x0B arcs honour 03/04 divisions too.
    const double rad =
        (double)(((unsigned)rh << 8) | rl) * (1.0 + radFrac) * ctx.scale;
    const bool cw = (fl & 0x80) != 0;
    const double s2 = (double)((fl >> 4) & 0x07);
    const int cnt = fl & 0x07;  // 0 = full circle
    // Start angle: octant boundary + fractional offset.  For a clockwise arc
    // the stored offset is the absolute value of a negative angle.
    const double sOff = cw ? -(double)so : (double)so;
    const double eOff =
        cw ? -(double)((eo == 0) ? 256 : eo)
           : (double)((eo == 0) ? 256 : eo);
    double sa = sOff * 45.0 / 256.0 + 45.0 * s2 + angAdj;
    double sweep;
    if (cnt == 0) {
        sweep = cw ? -360.0 : 360.0;
    }
    else {
        const int endOct = (int)s2 + (cw ? -1 : 1) * (cnt - 1);
        double ea = (double)endOct * 45.0 + eOff * 45.0 / 256.0;
        sweep = ea - sa;
        if (cw && sweep > 0.0) {
            sweep -= 360.0;
        }
        if (!cw && sweep < 0.0) {
            sweep += 360.0;
        }
    }
    const double a0r = toRad(sa);
    const double cx = ctx.x - rad * std::cos(a0r);
    const double cy = ctx.y - rad * std::sin(a0r);
    sampleArc(def, ctx, ctx.x, ctx.y, cx, cy, rad, sa, sa + sweep);
    return true;
}

//! Advance \a p past one code instruction (used by code 0x0E, which the
//! horizontal-text path skips).  Handles the same opcodes as the main loop;
//! the skip is intentionally conservative - unknown opcodes consume nothing
//! rather than guessing a length.
void skipOneCode(const unsigned char* data, std::size_t size,
                 std::size_t& p)
{
    if (p >= size) {
        return;
    }
    const unsigned char c = data[p++];
    if (c >= 0x10 && c <= 0xEF) {
        return;  // single-byte vector
    }
    switch (c) {
        case 0x01:  // draw / move are MODE SWITCHES (official code 001/002)
        case 0x02:  // "Activate / Deactivate Draw mode") - no payload of their
                    // own.  They only consume the following byte(s) when that
                    // byte is a real displacement: 0x08 (one signed pair) /
                    // 0x09 (pairs until (0,0)) / a single vector byte; a
                    // Special Code (0x00..0x0F, other than 08/09) is a NEW
                    // command and must NOT be consumed (mirrors decodeCodes).
            if (p < size) {
                const unsigned char v = data[p];
                if ((v >> 4) == 0 && v != 0x08 && v != 0x09) {
                    return;  // next byte is a separate command
                }
                ++p;
                if (v == 0x08 && p + 1 < size) {
                    p += 2;
                }
                else if (v == 0x09) {
                    while (p + 1 < size) {
                        const signed char dxb = (signed char)data[p];
                        const signed char dyb = (signed char)data[p + 1];
                        p += 2;
                        if (dxb == 0 && dyb == 0) {
                            break;
                        }
                    }
                }
            }
            return;
        case 0x03:  // scale codes: one byte
        case 0x04:
            if (p < size) {
                ++p;
            }
            return;
        case 0x05:  // push / pop: no payload
        case 0x06:
        case 0x00:
            return;
        case 0x07:  // subshape: 2-byte number + optional 1-byte scale
            if (p + 1 < size) {
                p += 2;
                if (p < size && data[p] != 0x00 && data[p] != 0xFF) {
                    ++p;
                }
            }
            return;
        case 0x08:  // X-Y displacement: one signed pair
            if (p + 1 < size) {
                p += 2;
            }
            return;
        case 0x09:  // X-Y multiple: pairs until (0,0)
            while (p + 1 < size) {
                const signed char dxb = (signed char)data[p];
                const signed char dyb = (signed char)data[p + 1];
                p += 2;
                if (dxb == 0 && dyb == 0) {
                    return;
                }
            }
            return;
        case 0x0A:  // octant arc: radius + sSSSCCCC
            if (p + 1 < size) {
                p += 2;
            }
            return;
        case 0x0B:  // fractional arc: 5 bytes
            if (p + 4 < size) {
                p += 5;
            }
            return;
        case 0x0E:  // nested vertical-text conditional: skip its target too
            skipOneCode(data, size, p);
            return;
        default:  // 0x0C / 0x0D / 0x0F: too complex to skip safely - stop
            return;
    }
}

//! Forward declaration: conic arcs may nest multiple arcs and multiple arcs
//! may nest conic arcs, so the two decoders recurse into each other.
bool parseMultiArc(const unsigned char* data, std::size_t size,
                   std::size_t& p, DecodeCtx& ctx, ShapeDef& def,
                   double radFrac = 0.0);

//! Conic (bulge) arc (code 0x0C): offset1, <arc code + its data>, offset2.
//! The offset bytes scale the radius (upper nibble, 1/256ths) and shift the
//! start / end angle (lower nibble, 1/256ths of 45 degrees).
bool parseConicArc(const unsigned char* data, std::size_t size,
                   std::size_t& p, DecodeCtx& ctx, ShapeDef& def)
{
    if (p >= size) {
        return false;
    }
    const unsigned char ofs1 = data[p++];
    const double radFrac = (double)((ofs1 >> 4) & 0x0F) / 256.0;
    const double angAdj = (double)(ofs1 & 0x0F) * 45.0 / 256.0;
    if (p >= size) {
        return false;
    }
    const unsigned char arc = data[p++];
    bool ok = false;
    switch (arc) {
        case 0x0A:
            ok = parseOctantArc(data, size, p, ctx, def, radFrac, angAdj);
            break;
        case 0x0B:
            ok = parseFracArc(data, size, p, ctx, def, radFrac, angAdj);
            break;
        case 0x0D:
            ok = parseMultiArc(data, size, p, ctx, def, radFrac);
            break;
        case 0x0C:
            ok = parseConicArc(data, size, p, ctx, def);
            break;
        default:
            --p;  // not an arc: hand the byte back to the main loop
            return false;
    }
    // End offset byte (same layout; only the radius fraction matters for a
    // single arc, which is already drawn - consume and discard).
    if (p < size) {
        ++p;
    }
    return ok;
}

//! Multiple arcs (code 0x0D): count byte + that many arc definitions, each
//! with its own leading code byte (0x0A / 0x0B / 0x0C / 0x0D).
//! (The default radFrac lives on the forward declaration above.)
bool parseMultiArc(const unsigned char* data, std::size_t size,
                   std::size_t& p, DecodeCtx& ctx, ShapeDef& def,
                   double radFrac)
{
    if (p >= size) {
        return false;
    }
    const unsigned char cnt = data[p++];
    for (int i = 0; i < (int)cnt && p < size; ++i) {
        const unsigned char ac = data[p++];
        if (ac == 0x0A) {
            parseOctantArc(data, size, p, ctx, def, radFrac, 0.0);
        }
        else if (ac == 0x0B) {
            parseFracArc(data, size, p, ctx, def, radFrac, 0.0);
        }
        else if (ac == 0x0C) {
            parseConicArc(data, size, p, ctx, def);
        }
        else if (ac == 0x0D) {
            parseMultiArc(data, size, p, ctx, def, radFrac);
        }
        else {
            break;  // malformed: stop consuming
        }
    }
    return true;
}

//! Decode a stream of shape byte codes (after the name) into outline
//! segments.  \a file, when non-null, supplies subshape definitions (code 7);
//! subshapes are drawn at the current pen with the current scale times the
//! subshape scale, then the pen and scale are restored (AutoCAD semantics).
bool decodeCodes(const unsigned char* data, std::size_t size, DecodeCtx& ctx,
                 ShapeDef& def, const ShapeFile* file, int depth)
{
    std::size_t p = 0;
    while (p < size) {
        const unsigned char code = data[p++];
        if (code == 0x00) {
            break;  // end of definition
        }
        if (code == 0x0F) {
            // Escape: 0x0F 0x0F ends the definition; other payloads are
            // skipped.
            if (p < size && data[p] == 0x0F) {
                break;
            }
            if (p < size) {
                ++p;
            }
            continue;
        }
        // A single-byte vector occupies the 0x10..0xEF range: its high nibble
        // is the length (1-15) and its low nibble is the direction (0-15 of
        // the 16-direction table).
        const int len = (code >> 4) & 0x0F;
        if (len >= 1 && len <= 15) {
            const int dir = code & 0x0F;
            const double dx = DIR_DX[dir] * (double)len * ctx.scale;
            const double dy = DIR_DY[dir] * (double)len * ctx.scale;
            const double nx = ctx.x + dx;
            const double ny = ctx.y + dy;
            if (ctx.penDown) {
                addSeg(def, ctx, ctx.x, ctx.y, nx, ny);
            }
            ctx.x = nx;
            ctx.y = ny;
            ctx.lastDx = dx;
            ctx.lastDy = dy;
            ctx.havePrev = true;
            continue;
        }

        // Two-byte codes: opcode = code (0x01..0x0E), parameters follow.
        switch (code) {
            case 0x01: {  // Activate Draw mode = pen down (official "code 001").
                // AutoCAD's Special Codes Reference defines 001 as a MODE
                // SWITCH with no payload of its own: the command that follows
                // (a single-byte vector, an 08 X-Y displacement or a 09 X-Y
                // list) runs with the pen down and therefore draws.
                ctx.penDown = true;
                //   - 0x08: an X-Y displacement pair follows (2 signed bytes),
                //     e.g. 01 08 FC F2 draws to (-4,-14)  - this is how text
                //     fonts (txt.shx / romans.shx) encode every stroke;
                //   - 0x09: one or more X-Y pairs follow, terminated by (0,0);
                //   - anything else: a standard vector code, high nibble =
                //     length 1-15, low nibble = direction 0-15 (same encoding
                //     as the standalone 0x10-0xEF vector commands).
                // ltypeshp TRACK1 (14 02 1C 01 1C 00) proves the vector-code
                // reading: 0x1C = length 1, direction 12 (S) produces the two
                // vertical stubs at +-1 that AutoCAD draws - treating 0x1C as
                // a length of 28 would make a 57-unit-tall bar instead.
                // A following byte that is itself a Special Code (0x00..0x0F,
                // not 08/09) is handed back to the main loop below (--p).
                if (p < size) {
                    const unsigned char v = data[p++];
                    if (v == 0x08 && p + 1 < size) {
                        // X-Y displacement pair: two signed bytes.
                        const double dx =
                            (double)(signed char)data[p] * ctx.scale;
                        const double dy =
                            (double)(signed char)data[p + 1] * ctx.scale;
                        p += 2;
                        addSeg(def, ctx, ctx.x, ctx.y, ctx.x + dx, ctx.y + dy);
                        ctx.x += dx;
                        ctx.y += dy;
                    }
                    else if (v == 0x09) {
                        // X-Y multiple: pairs until the (0,0) sentinel.
                        while (p + 1 < size) {
                            const signed char dxb = (signed char)data[p];
                            const signed char dyb = (signed char)data[p + 1];
                            p += 2;
                            if (dxb == 0 && dyb == 0) {
                                break;
                            }
                            const double dx = (double)dxb * ctx.scale;
                            const double dy = (double)dyb * ctx.scale;
                            addSeg(def, ctx, ctx.x, ctx.y, ctx.x + dx,
                                   ctx.y + dy);
                            ctx.x += dx;
                            ctx.y += dy;
                        }
                    }
                    else if ((v >> 4) == 0) {
                        // A byte in 0x00..0x0F is a shape COMMAND (scale 03/04,
                        // push/pop 05/06, arcs 0A..0D, ...), never a valid
                        // displacement parameter of a draw/move.  Hand it back
                        // to the main loop so it executes as its own command.
                        // AAA.shx glyphs put a scale code right after a draw/
                        // move ("02 03 0A"): swallowing the 03 here would make
                        // the scale's 0A parameter look like a standalone
                        // octant arc, shifting every following byte and
                        // dividing the scale into a runaway 1e-6 tail.
                        --p;
                    }
                    else {
                        const int len = (v >> 4) & 0x0F;
                        const int dir = v & 0x0F;
                        if (len >= 1 && len <= 15) {
                            const double dx =
                                DIR_DX[dir] * (double)len * ctx.scale;
                            const double dy =
                                DIR_DY[dir] * (double)len * ctx.scale;
                            addSeg(def, ctx, ctx.x, ctx.y, ctx.x + dx,
                                   ctx.y + dy);
                            ctx.x += dx;
                            ctx.y += dy;
                        }
                    }
                }
                continue;
            }
            case 0x02: {  // Deactivate Draw mode = pen up (official "code 002").
                // Mode switch; the following vector / 08 / 09 command runs
                // with the pen up (moves without drawing).  Same back-off rule
                // as 0x01 for a following Special Code.
                ctx.penDown = false;
                if (p < size) {
                    const unsigned char v = data[p++];
                    if (v == 0x08 && p + 1 < size) {
                        const double dx =
                            (double)(signed char)data[p] * ctx.scale;
                        const double dy =
                            (double)(signed char)data[p + 1] * ctx.scale;
                        p += 2;
                        ctx.x += dx;
                        ctx.y += dy;
                    }
                    else if (v == 0x09) {
                        while (p + 1 < size) {
                            const signed char dxb = (signed char)data[p];
                            const signed char dyb = (signed char)data[p + 1];
                            p += 2;
                            if (dxb == 0 && dyb == 0) {
                                break;
                            }
                            ctx.x += (double)dxb * ctx.scale;
                            ctx.y += (double)dyb * ctx.scale;
                        }
                    }
                    else if ((v >> 4) == 0) {
                        --p;  // command byte; let the main loop handle it
                    }
                    else {
                        const int len = (v >> 4) & 0x0F;
                        const int dir = v & 0x0F;
                        if (len >= 1 && len <= 15) {
                            ctx.x += DIR_DX[dir] * (double)len * ctx.scale;
                            ctx.y += DIR_DY[dir] * (double)len * ctx.scale;
                        }
                    }
                }
                ctx.flushStroke();  // pen-up ends the current stroke
                continue;
            }
            case 0x03:  // divide scale (AutoCAD "3,scale": restore a local
                // 4,scale multiplication - e.g. AAA.shx pairs 03 0A with
                // 04 0A so the whole symbol returns to unit scale, and a
                // glyph may end with several 03 FF steps that shrink the
                // drawing 255x each.  Treating 03 as a multiply (like 04)
                // squared every paired 03/04 and turned a lone "03 FF x6"
                // tail into a 255^6 = 2.7e14 blow-up.)
                if (p < size) {
                    int m = data[p++];
                    if (m == 0) {
                        m = 1;  // defensive
                    }
                    ctx.scale /= (double)m;
                }
                continue;
            case 0x04: {  // scale (multiply)
                if (p < size) {
                    int m = data[p++];
                    if (m == 0) {
                        m = 1;  // defensive
                    }
                    ctx.scale *= (double)m;
                }
                continue;
            }
            case 0x05:  // push position
                if (ctx.sp < 16) {
                    ctx.stack[ctx.sp++] = ctx.x;
                    ctx.stack[ctx.sp++] = ctx.y;
                }
                continue;
            case 0x06:  // pop position
                if (ctx.sp >= 2) {
                    ctx.y = ctx.stack[--ctx.sp];
                    ctx.x = ctx.stack[--ctx.sp];
                }
                continue;
            case 0x07: {  // subshape: 2-byte BE number + optional scale
                ctx.sawSubshape = true;
                if (p + 1 < size) {
                    const int subNum = ((int)data[p] << 8) | (int)data[p + 1];
                    p += 2;
                    double subScale = 1.0;
                    if (p < size && data[p] != 0x00 && data[p] != 0xFF) {
                        subScale = (double)data[p];
                        ++p;
                    }
                    if (file && depth < kMaxSubshapeDepth) {
                        const auto it = file->find(subNum);
                        if (it != file->end() && !it->second.codes.empty()) {
                            // Draw the subshape at the current pen with the
                            // combined scale; restore position and scale after.
                            const double sx = ctx.x;
                            const double sy = ctx.y;
                            const double ss = ctx.scale;
                            ctx.scale *= subScale;
                            DecodeCtx sub = ctx;  // independent push/pop stack
                            decodeCodes(it->second.codes.data(),
                                        it->second.codes.size(), sub, def, file,
                                        depth + 1);
                            ctx.x = sx;
                            ctx.y = sy;
                            ctx.scale = ss;
                        }
                    }
                }
                continue;
            }
            case 0x08: {  // X-Y displacement: ONE signed (dx,dy) byte pair.
                // AutoCAD "8,dx,dy" - there is no count byte; the two
                // coordinates are a single displacement (each -128..127).
                // Draws only with the pen down - with the pen up it is a
                // move (txt.shx glyphs use this to fly to the first stroke).
                if (p + 1 < size) {
                    const double dx =
                        (double)(signed char)data[p] * ctx.scale;
                    const double dy =
                        (double)(signed char)data[p + 1] * ctx.scale;
                    p += 2;
                    const double nx = ctx.x + dx;
                    const double ny = ctx.y + dy;
                    if (ctx.penDown) {
                        addSeg(def, ctx, ctx.x, ctx.y, nx, ny);
                    }
                    ctx.x = nx;
                    ctx.y = ny;
                }
                continue;
            }
            case 0x09: {  // X-Y multiple: any number of (dx,dy) pairs,
                // terminated by the sentinel (0,0).  Each coordinate is a
                // signed byte; the all-zero pair marks the end.
                while (p + 1 < size) {
                    const signed char dxb = (signed char)data[p];
                    const signed char dyb = (signed char)data[p + 1];
                    p += 2;
                    if (dxb == 0 && dyb == 0) {
                        break;  // (0,0) terminator
                    }
                    const double dx = (double)dxb * ctx.scale;
                    const double dy = (double)dyb * ctx.scale;
                    const double nx = ctx.x + dx;
                    const double ny = ctx.y + dy;
                    if (ctx.penDown) {
                        addSeg(def, ctx, ctx.x, ctx.y, nx, ny);
                    }
                    ctx.x = nx;
                    ctx.y = ny;
                }
                continue;
            }
            case 0x0A:
                parseOctantArc(data, size, p, ctx, def);
                continue;
            case 0x0B:
                parseFracArc(data, size, p, ctx, def);
                continue;
            case 0x0C:
                parseConicArc(data, size, p, ctx, def);
                continue;
            case 0x0D:
                parseMultiArc(data, size, p, ctx, def);
                continue;
            case 0x0E:  // vertical text: in horizontal text (the common
                // import path) the NEXT code instruction is skipped.
                // AutoCAD: "if vertical text, process the next code;
                // otherwise (horizontal) skip it."
                skipOneCode(data, size, p);
                continue;
            default:
                break;  // unknown byte: skip it defensively.
        }
    }
    return true;
}

//! Decode a shape body: null-terminated name + byte codes into outline
//! segments.  \a file, when non-null, expands subshape references.
bool decodeShapeBody(const unsigned char* data, std::size_t size,
                     ShapeDef& def, bool& sawSubshape,
                     const ShapeFile* file = nullptr)
{
    std::size_t p = 0;
    while (p < size && data[p] != 0) {
        def.name.push_back(static_cast<char>(data[p]));
        ++p;
    }
    if (p < size) {
        ++p;  // name terminator
    }
    DecodeCtx ctx;
    decodeCodes(data + p, size - p, ctx, def, file, 0);
    sawSubshape = ctx.sawSubshape;
    def.penEndX = ctx.x;
    def.penEndY = ctx.y;
    return true;
}

}  // namespace

ShapeDef parseShapeBody(const unsigned char* data, std::size_t size,
                        const ShapeFile* file)
{
    ShapeDef def;
    bool sub = false;
    decodeShapeBody(data, size, def, sub, file);
    return def;
}

//! True when the buffer starts with the AutoCAD-86 header for a regular
//! shape / font file ("AutoCAD-86 shapes 1.0"/"1.1", e.g. txt.shx and
//! ltypeshp.shx).  These use the classic sequential layout.
bool isAutoCad86ShapeHeader(const unsigned char* data, std::size_t size)
{
    static const char kHead[] = "AutoCAD-86 shapes ";
    constexpr std::size_t kN = sizeof(kHead) - 1;  // 18
    return size >= 24 && std::memcmp(data, kHead, kN) == 0;
}

//! True when the buffer starts with the AutoCAD-86 header for a big-font
//! (double-byte CJK) file, e.g. gbcbig.shx / FSDB.SHX.  These have a
//! completely different layout: 8-byte-per-character index table, not the
//! sequential entry list of the classic shape files.
bool isAutoCad86BigfontHeader(const unsigned char* data, std::size_t size)
{
    static const char kHead[] = "AutoCAD-86 bigfont ";
    constexpr std::size_t kN = sizeof(kHead) - 1;  // 19
    return size >= 32 && std::memcmp(data, kHead, kN) == 0;
}

ShapeFile parseShapeFileBytes(const unsigned char* data, std::size_t size)
{
    ShapeFile out;
    if (!data || size < 20) {
        return out;
    }
    const unsigned sig = (unsigned)data[0] | ((unsigned)data[1] << 8);

    // Shape definitions (number, name, raw byte codes) collected from
    // whichever container layout the file uses.
    struct Entry
    {
        int number = 0;
        std::string name;
        std::vector<unsigned char> codes;
    };
    std::vector<Entry> entries;

    if (sig == 0x0BF8) {
        // New layout: location table of 3-byte big-endian offsets, one slot
        // per shape number.
        const std::size_t nSlots = std::min<std::size_t>((size - 20) / 3, 326);
        for (std::size_t i = 0; i < nSlots; ++i) {
            const std::size_t off = 20 + i * 3;
            if (off + 3 > size) {
                break;
            }
            const unsigned long loc = ((unsigned long)data[off] << 16)
                | ((unsigned long)data[off + 1] << 8)
                | (unsigned long)data[off + 2];
            if (loc == 0 || loc >= size) {
                continue;
            }
            std::size_t p = loc;
            int number = data[p];
            ++p;
            if (number == 0xFE) {
                // Shape number >= 256: two following big-endian bytes.
                if (p + 1 >= size) {
                    continue;
                }
                number = ((int)data[p] << 8) | (int)data[p + 1];
                p += 2;
            }
            if (p + 2 > size) {
                continue;
            }
            const int len = ((int)data[p] << 8) | (int)data[p + 1];
            p += 2;
            // The length field counts the byte codes only; the
            // null-terminated name precedes them.
            std::size_t nameLen = 0;
            while (p + nameLen < size && data[p + nameLen] != 0) {
                ++nameLen;
            }
            if (p + nameLen >= size) {
                continue;  // no name terminator -> malformed definition
            }
            ++nameLen;  // include the terminator
            if (p + nameLen + (std::size_t)len > size) {
                continue;
            }
            Entry e;
            e.number = number;
            e.name.assign(reinterpret_cast<const char*>(data + p), nameLen - 1);
            e.codes.assign(data + p + nameLen, data + p + nameLen + len);
            entries.push_back(std::move(e));
        }
    }
    else if (isAutoCad86ShapeHeader(data, size)) {
        // Old layout: the ASCII header ("AutoCAD-86 shapes 1.1\r\n\x1a") is
        // followed by 6 bytes of <first number:2 LE><last number:2 LE>
        // <entry count:2 LE>, then 'count' entries of <number:2 LE>
        // <definition length:2 LE>.  Definitions follow in order as
        // <name\0><byte codes>; the stored length covers the whole
        // definition.  The exact byte length of the ASCII header varies with
        // the version string, so we skip past the 0x1A terminator instead of
        // assuming a fixed byte.
        std::size_t p = 0;
        while (p < size && data[p] != 0x1A) {
            ++p;
        }
        if (p >= size || ++p >= size) {
            return out;
        }
        if (p + 6 > size) {
            return out;
        }
        const int count = (int)data[p + 4] | ((int)data[p + 5] << 8);
        p += 6;
        if (count <= 0 || count > 4096) {
            return out;
        }
        if (p + (std::size_t)count * 4 > size) {
            return out;
        }
        std::size_t dataPos = p + (std::size_t)count * 4;
        for (int i = 0; i < count; ++i) {
            const int num = (int)data[p + i * 4]
                | ((int)data[p + i * 4 + 1] << 8);
            const int len = (int)data[p + i * 4 + 2]
                | ((int)data[p + i * 4 + 3] << 8);
            if (len <= 0 || dataPos + (std::size_t)len > size) {
                break;
            }
            const std::size_t end = dataPos + (std::size_t)len;
            std::size_t q = dataPos;
            std::string name;
            while (q < end && data[q] != 0) {
                name.push_back((char)data[q]);
                ++q;
            }
            if (q < end) {
                ++q;  // skip the name terminator
            }
            Entry e;
            e.number = num;
            e.name = std::move(name);
            e.codes.assign(data + q, data + end);
            entries.push_back(std::move(e));
            dataPos = end;
        }
    }
    else if (isAutoCad86BigfontHeader(data, size)) {
        // Big-font (double-byte CJK) layout.  Header layout is NOT the
        // sequential entry list of the classic shape files:
        //   0x00..0x15  "AutoCAD-86 bigfont 1.0"
        //   0x16..0x18  0D 0A 1A
        //   0x19..0x1A  08 00 (fixed)
        //   0x1B..0x1C  character count (LE)
        //   0x1D..0x1E  range count (LE)
        //   then 'range count' ranges of 4 bytes (start code :2 LE,
        //     end code :2 LE) - the code space covered by the font;
        //   then a character index of 8 bytes per character:
        //     +0 char code :2 LE, +2 definition length :2 LE,
        //     +4 absolute file offset of the definition :4 LE;
        //   the definitions themselves live at those absolute offsets as
        //   <name\0><byte codes> (same shape byte code stream as a classic
        //   font, including subshape code 7 references).
        if (size < 0x1F + 4u) {
            return out;
        }
        const std::size_t charCount = (std::size_t)data[0x1B]
            | ((std::size_t)data[0x1C] << 8);
        const std::size_t rangeCount = (std::size_t)data[0x1D]
            | ((std::size_t)data[0x1E] << 8);
        if (charCount == 0 || charCount > 2000000) {
            return out;
        }
        std::size_t p = 0x1F + rangeCount * 4;
        if (p + charCount * 8 > size) {
            return out;
        }
        for (std::size_t i = 0; i < charCount; ++i) {
            const std::size_t q = p + i * 8;
            if (q + 8 > size) {
                break;
            }
            const int number = (int)data[q]
                | ((int)data[q + 1] << 8);
            const int len = (int)data[q + 2]
                | ((int)data[q + 3] << 8);
            const unsigned long addr = (unsigned long)data[q + 4]
                | ((unsigned long)data[q + 5] << 8)
                | ((unsigned long)data[q + 6] << 16)
                | ((unsigned long)data[q + 7] << 24);
            if (len <= 0 || addr == 0 || addr >= size
                || addr + (std::size_t)len > size) {
                continue;
            }
            const std::size_t end = addr + (std::size_t)len;
            std::size_t r = addr;
            std::string name;
            while (r < end && data[r] != 0) {
                name.push_back((char)data[r]);
                ++r;
            }
            if (r < end) {
                ++r;  // skip the name terminator
            }
            Entry e;
            e.number = number;
            e.name = std::move(name);
            e.codes.assign(data + r, data + end);
            entries.push_back(std::move(e));
        }
    }
    else {
        return out;  // not a shape file
    }

    // Second pass: decode every shape (subshape references resolve against
    // the map built from the first pass).  We keep *every* entry that was
    // successfully read from the entry table, even when its byte codes
    // turn out to be empty (name and segs both empty).  Big-font files
    // such as FSDB.SHX consist almost entirely of stub entries that defer
    // the actual drawing to a base font via subshape (code 7) references;
    // discarding those empty stubs would leave the returned ShapeFile
    // empty and the caller would reject the file as "unreadable".
    for (const Entry& e : entries) {
        ShapeDef def;
        def.number = e.number;
        def.name = e.name;
        def.codes = e.codes;
        DecodeCtx ctx;
        decodeCodes(e.codes.data(), e.codes.size(), ctx, def, &out, 0);
        def.penEndX = ctx.x;
        def.penEndY = ctx.y;
        out[e.number] = std::move(def);
    }
    return out;
}

ShapeFile parseShapeFile(const std::string& path)
{
    std::FILE* f = std::fopen(toLocalPath(path).c_str(), "rb");
    if (!f) {
        return ShapeFile();
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return ShapeFile();
    }
    std::vector<unsigned char> buf((std::size_t)sz);
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) {
        return ShapeFile();
    }
    return parseShapeFileBytes(buf.data(), buf.size());
}

namespace
{

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    std::FILE* f = std::fopen(toLocalPath(path).c_str(), "rb");
    if (f) {
        std::fclose(f);
        return true;
    }
    return false;
}

}  // namespace

//! Uncached scan (kept separate so the public findShapeFile can memoise).
static std::string findShapeFileUncached(
    const std::string& name,
    const std::vector<std::string>& extraPaths);

//! Locate \a name on disk with a process-wide positive + negative cache.
//!
//! The scan touches Autodesk / Bentley install trees and is surprisingly
//! expensive; text rendering / linetype expansion call this once per style
//! name, so without a cache a drawing with thousands of text objects repeats
//! the whole directory walk every time.  Cache key includes the extra paths,
//! and a "not found" result is cached too (empty), so a missing font is only
//! scanned for once.
std::string findShapeFile(const std::string& name,
                          const std::vector<std::string>& extraPaths)
{
    if (name.empty()) {
        return "";
    }
    std::string key = name;
    key.push_back('\x1f');
    for (const std::string& dir : extraPaths) {
        key += dir;
        key.push_back('\x1f');
    }
    static std::mutex mtx;
    static std::map<std::string, std::string> resolved;  // key -> path or ""
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = resolved.find(key);
        if (it != resolved.end()) {
            return it->second;
        }
    }
    const std::string result = findShapeFileUncached(name, extraPaths);
    {
        std::lock_guard<std::mutex> lock(mtx);
        resolved[key] = result;
    }
    return result;
}

static std::string findShapeFileUncached(
    const std::string& name,
    const std::vector<std::string>& extraPaths)
{
    if (name.empty()) {
        return "";
    }
    if (fileExists(name)) {
        return name;
    }
    // Caller supplied paths first (e.g. FreeCAD bin / module dirs).
    for (const std::string& dir : extraPaths) {
        if (dir.empty()) {
            continue;
        }
        std::string cand = dir;
        if (cand.back() != '/' && cand.back() != '\\') {
            cand += '/';
        }
        cand += name;
        if (fileExists(cand)) {
            return cand;
        }
    }
    // Common AutoCAD font directories (ltypeshp.shx ships with AutoCAD).
    // The installed version is unknown, so enumerate every "AutoCAD <year>"
    // directory under the standard Autodesk roots (this covers old releases
    // such as 2016 that a hard-coded year list would miss).
    namespace fs = std::filesystem;
    const char* autodeskRoots[] = {
        "C:/Program Files/Autodesk",
        "C:/Program Files (x86)/Autodesk",
    };
    for (const char* root : autodeskRoots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) {
            continue;
        }
        for (const auto& de : fs::directory_iterator(root, ec)) {
            if (!de.is_directory(ec)) {
                continue;
            }
            const std::string dn = de.path().filename().string();
            if (dn.rfind("AutoCAD", 0) != 0) {
                continue;
            }
            // User Data Cache Support directories (zh-cn / en / ...): the
            // localised install keeps the *matching* ltypeshp.shx here rather
            // than in Fonts (e.g. AutoCAD 2016 zh-cn).  Prefer it over the
            // plain Fonts copy so the shape bodies match the acadiso.lin the
            // DWG was authored against.
            const fs::path cacheRoot = de.path() / "UserDataCache";
            std::error_code ec2;
            if (fs::is_directory(cacheRoot, ec2)) {
                for (const auto& sub : fs::directory_iterator(cacheRoot, ec2)) {
                    if (!sub.is_directory(ec2)) {
                        continue;
                    }
                    const fs::path cand2 = sub.path() / "Support" / name;
                    if (fileExists(cand2.string())) {
                        return cand2.string();
                    }
                }
            }
            // Standard Fonts directory (older / English installs).
            const fs::path cand = de.path() / "Fonts" / name;
            if (fileExists(cand.string())) {
                return cand.string();
            }
        }
    }
    // Bentley CAD products (OpenRoads Designer, MicroStation, ...) also ship
    // ltypeshp.shx in their Fonts directories.
    const char* bentleyRoots[] = {
        "C:/Program Files/Bentley",
        "C:/Program Files (x86)/Bentley",
    };
    for (const char* root : bentleyRoots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) {
            continue;
        }
        for (const auto& de : fs::directory_iterator(root, ec)) {
            if (!de.is_directory(ec)) {
                continue;
            }
            const fs::path fonts = de.path() / "Fonts";
            std::error_code ec2;
            if (!fs::is_directory(fonts, ec2)) {
                continue;
            }
            // Search two levels deep (Bentley nests Fonts inside a product
            // sub-folder, e.g. OpenRoadsDesigner/Default/Fonts).
            for (const auto& sub : fs::recursive_directory_iterator(
                     fonts, fs::directory_options::skip_permission_denied, ec2)) {
                if (sub.is_regular_file(ec2)
                    && sub.path().filename() == name) {
                    return sub.path().string();
                }
            }
        }
    }
    return "";
}

}  // namespace SWDWG
