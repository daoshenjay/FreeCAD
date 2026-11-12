//! Standalone unit test for the .shx shape file parser (DWGShapeFile.h/cpp).
//!
//! Builds compiled shape file byte streams in memory and verifies the parsed
//! outlines.  Both container layouts are exercised:
//!   - the modern 0x0BF8 layout (location table of 3-byte BE offsets),
//!   - the AutoCAD-86 text-header layout used by ltypeshp.shx since AutoCAD
//!     2.x (location table of <number,length> LE pairs).
//!
//! Single-byte vector encoding follows the real AutoCAD compiled format:
//! the byte is (length << 4) | direction, where length = 1..15 and direction
//! is the 16-direction code 0..15 (0 = east, 1 = 22.5 deg, 2 = 45 deg, ...,
//! counter-clockwise).  This matches the text spec-byte form "0<len><dir>"
//! (e.g. 014 = length 1, direction 4 = north) documented in the AutoCAD
//! Customization Guide.  The real ltypeshp.shx verifies this: ZIG is bytes
//! 12 1E 00, i.e. length 1 NE + length 1 SE, an inverted-V (the two strokes
//! that make AutoCAD's ZIGZAG zig-zag).
//!
//! Build (PowerShell, from tests/):
//!   vcvars64.bat && cl /nologo /EHsc /std:c++17 /I ..\App test_shape.cpp
//!       ..\App\DWGShapeFile.cpp /Fe:test_shape.exe

#include "DWGShapeFile.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace SWDWG;

static int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                     \
        const double _a = (a);                                               \
        const double _b = (b);                                               \
        if (std::fabs(_a - _b) > (eps)) {                                    \
            std::printf("FAIL %s:%d  %s=%g  %s=%g\n", __FILE__, __LINE__,    \
                        #a, _a, #b, _b);                                     \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

int main()
{
    // ---- Build a minimal 0x0BF8 .shx in memory ---------------------------
    // Header is 1000 bytes; the location table (3-byte BE offsets per shape
    // number slot) starts at byte 20.
    std::vector<unsigned char> buf(1139, 0);
    buf[0] = 0xF8;  // signature LE 0x0BF8
    buf[1] = 0x0B;
    buf[2] = 0x01;  // version 1
    buf[3] = 0x00;
    buf[4] = 0x0A;  // 10 shapes
    buf[5] = 0x00;
    buf[6] = 0x0A;  // max shape length 10
    buf[7] = 0x00;

    auto putLoc = [&buf](int slot, unsigned long off) {
        const std::size_t i = 20 + (std::size_t)slot * 3;
        buf[i] = (unsigned char)((off >> 16) & 0xFF);
        buf[i + 1] = (unsigned char)((off >> 8) & 0xFF);
        buf[i + 2] = (unsigned char)(off & 0xFF);
    };
    putLoc(1, 1000);
    putLoc(2, 1012);
    putLoc(3, 1026);
    putLoc(4, 1042);
    putLoc(5, 1054);
    putLoc(6, 1068);
    putLoc(7, 1084);
    putLoc(8, 1102);
    putLoc(9, 1114);
    putLoc(10, 1127);

    // Shape 1 "TRI": move 4 (2-byte code 2), then single-byte vectors 0x42
    // (length 4, direction 2 = NE 45 deg) and 0x4A (length 4, direction 10
    // = SW 225 deg).  Path: (0,0) -> (4,0) -> (8,4) -> (4,0).
    const unsigned char d1[] = {1, 0, 5, 'T', 'R', 'I', 0, 0x02, 0x04,
                                0x42, 0x4A, 0x00};
    // Shape 2 "CIRC": push (code 5), full octant arc radius 8 (octant byte
    // 0xC0: sign bit set, start octant 4, count 0 = full circle), pop.
    const unsigned char d2[] = {2, 0, 6, 'C', 'I', 'R', 'C', 0,
                                0x05, 0x0A, 0x08, 0xC0, 0x06, 0x00};
    // Shape 3 "TRK": X-Y multiple (code 9) - a sequence of signed (dx,dy)
    // byte pairs terminated by the sentinel (0,0): (2,0), (0,2), (-2,0).
    // (AutoCAD code 8 is a *single* displacement pair and carries no count;
    // code 9 is the multi-pair form used here.)
    const unsigned char d3[] = {3, 0, 9, 'T', 'R', 'K', 0,
                                0x09, 0x02, 0x00, 0x00, 0x02, 0xFE,
                                0x00, 0x00, 0x00};
    // Shape 4 "SCL": code 4 multiplies the scale by 2 (draw len4 east ->
    // 8 units), code 3 divides it back to 1 (draw len4 east -> 4 more units,
    // ending at 12).  AutoCAD semantics: 03 = divide, 04 = multiply.  AAA.shx
    // (symbol glyphs) relies on this: treating 03 as a multiply too squared
    // every paired 03/04 and turned a "03 FF" tail into a 255^n blow-up.
    const unsigned char d4[] = {4, 0, 5, 'S', 'C', 'L', 0,
                                0x04, 0x02, 0x01, 0x40, 0x03, 0x02,
                                0x01, 0x40, 0x00};
    // Shape 5 "SUB": move 4, then subshape 1 (TRI) with scale 2, then end.
    const unsigned char d5[] = {5, 0, 7, 'S', 'U', 'B', 0,
                                0x02, 0x04, 0x07, 0x00, 0x01, 0x02, 0x00};
    // Shape 6 "CONIC": conic arc around a full octant arc radius 8 with a
    // 1/256 radius fraction (offset byte 0x10), end offset 0x00.
    const unsigned char d6[] = {6, 0, 7, 'C', 'O', 'N', 'I', 'C', 0,
                                0x0C, 0x10, 0x0A, 0x08, 0xC0, 0x00, 0x00};
    // Shape 7 "MULTI": two full octant circles radius 8.
    const unsigned char d7[] = {7, 0, 9, 'M', 'U', 'L', 'T', 'I', 0,
                                0x0D, 0x02, 0x0A, 0x08, 0xC0, 0x0A, 0x08,
                                0xC0, 0x00};
    // Shape 8 "ARCW": clockwise half circle.  Octant byte 0x84: sign bit set
    // (CW), start octant 0, count 4.  From 0 deg clockwise to -180 deg
    // (lower half of the circle centred (-8,0)).
    const unsigned char d8[] = {8, 0, 4, 'A', 'R', 'C', 'W', 0,
                                0x0A, 0x08, 0x84, 0x00};
    // Shape 9 "ARCCW": counter-clockwise half circle.  Octant byte 0x04:
    // CCW, start octant 0, count 4.  Upper half of the circle centred
    // (-8,0).
    const unsigned char d9[] = {9, 0, 4, 'A', 'R', 'C', 'C', 'W', 0,
                                0x0A, 0x08, 0x04, 0x00};
    // Shape 10 "SELF": subshape reference to itself - must terminate through
    // the recursion depth guard instead of hanging.
    const unsigned char d10[] = {10, 0, 4, 'S', 'E', 'L', 'F', 0,
                                 0x07, 0x00, 0x0A, 0x00};
    std::memcpy(&buf[1000], d1, sizeof(d1));
    std::memcpy(&buf[1012], d2, sizeof(d2));
    std::memcpy(&buf[1026], d3, sizeof(d3));
    std::memcpy(&buf[1042], d4, sizeof(d4));
    std::memcpy(&buf[1054], d5, sizeof(d5));
    std::memcpy(&buf[1068], d6, sizeof(d6));
    std::memcpy(&buf[1084], d7, sizeof(d7));
    std::memcpy(&buf[1102], d8, sizeof(d8));
    std::memcpy(&buf[1114], d9, sizeof(d9));
    std::memcpy(&buf[1127], d10, sizeof(d10));

    // ---- Parse and verify -------------------------------------------------
    ShapeFile sf = parseShapeFileBytes(buf.data(), buf.size());
    CHECK(sf.size() == 10);

    // Shape 1: triangle outline.
    {
        auto it = sf.find(1);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "TRI");
            CHECK(it->second.number == 1);
            CHECK(it->second.segs.size() == 2);
            if (it->second.segs.size() == 2) {
                CHECK_NEAR(it->second.segs[0].x0, 4.0, 1e-9);
                CHECK_NEAR(it->second.segs[0].y0, 0.0, 1e-9);
                CHECK_NEAR(it->second.segs[0].x1, 8.0, 1e-9);
                CHECK_NEAR(it->second.segs[0].y1, 4.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].x0, 8.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].y0, 4.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].x1, 4.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].y1, 0.0, 1e-9);
            }
        }
    }
    // Shape 2: full circle radius 8 centred (8, 0).
    {
        auto it = sf.find(2);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "CIRC");
            CHECK(it->second.segs.size() >= 4);
            if (!it->second.segs.empty()) {
                const ShapeSeg& s0 = it->second.segs.front();
                const ShapeSeg& sl = it->second.segs.back();
                CHECK_NEAR(s0.x0, 0.0, 1e-9);   // starts at the pen position
                CHECK_NEAR(s0.y0, 0.0, 1e-9);
                CHECK_NEAR(sl.x1, 0.0, 1e-6);   // closes back to the start
                CHECK_NEAR(sl.y1, 0.0, 1e-6);
                // Mid points lie on the circle centred (8,0).
                const std::size_t mid = it->second.segs.size() / 2;
                const double mx = it->second.segs[mid].x0;
                const double my = it->second.segs[mid].y0;
                const double d = std::sqrt((mx - 8.0) * (mx - 8.0) + my * my);
                CHECK_NEAR(d, 8.0, 0.1);
            }
        }
    }
    // Shape 3: X-Y displacement pairs.
    {
        auto it = sf.find(3);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "TRK");
            CHECK(it->second.segs.size() == 3);
            if (it->second.segs.size() == 3) {
                CHECK_NEAR(it->second.segs[0].x0, 0.0, 1e-9);
                CHECK_NEAR(it->second.segs[0].x1, 2.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].x0, 2.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].y1, 2.0, 1e-9);
                CHECK_NEAR(it->second.segs[2].x0, 2.0, 1e-9);
                CHECK_NEAR(it->second.segs[2].x1, 0.0, 1e-9);
                CHECK_NEAR(it->second.segs[2].y0, 2.0, 1e-9);
            }
        }
    }
    // Shape 4: code 4 = scale x2 -> 8 units; code 3 = divide back to x1
    // -> 4 more units, ending at 12.
    {
        auto it = sf.find(4);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "SCL");
            CHECK(it->second.segs.size() == 2);
            if (it->second.segs.size() == 2) {
                CHECK_NEAR(it->second.segs[0].x1, 8.0, 1e-9);   // 4 * 2
                CHECK_NEAR(it->second.segs[1].x0, 8.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].x1, 12.0, 1e-9);  // + 4 * 1
            }
        }
    }
    // Shape 5: subshape TRI x2 drawn at the current pen; the pen returns to
    // the pre-call position afterwards.
    {
        auto it = sf.find(5);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "SUB");
            CHECK(it->second.segs.size() == 2);
            if (it->second.segs.size() == 2) {
                // TRI is a full shape body, so it carries its own leading
                // move 4.  At the current pen (4,0) with scale 2:
                //   move 4 -> (4 + 8, 0) = (12, 0)
                //   draw dir2 4*2 -> (12+8, 0+8) = (20, 8)
                //   draw dir10 4*2 -> (20-8, 8-8) = (12, 0)
                CHECK_NEAR(it->second.segs[0].x0, 12.0, 1e-9);
                CHECK_NEAR(it->second.segs[0].y0, 0.0, 1e-9);
                CHECK_NEAR(it->second.segs[0].x1, 20.0, 1e-9);
                CHECK_NEAR(it->second.segs[0].y1, 8.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].x0, 20.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].y0, 8.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].x1, 12.0, 1e-9);
                CHECK_NEAR(it->second.segs[1].y1, 0.0, 1e-9);
            }
        }
    }
    // Shape 6: conic arc radius fraction 1/256 -> r = 8.03125, full circle.
    {
        auto it = sf.find(6);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "CONIC");
            CHECK(it->second.segs.size() >= 4);
            if (!it->second.segs.empty()) {
                const double rAdj = 8.0 * (1.0 + 1.0 / 256.0);
                CHECK_NEAR(it->second.segs.front().x0, 0.0, 1e-9);
                CHECK_NEAR(it->second.segs.back().x1, 0.0, 1e-6);
                CHECK_NEAR(it->second.segs.back().y1, 0.0, 1e-6);
                const std::size_t mid = it->second.segs.size() / 2;
                const double mx = it->second.segs[mid].x0;
                const double my = it->second.segs[mid].y0;
                const double d =
                    std::sqrt((mx - rAdj) * (mx - rAdj) + my * my);
                CHECK_NEAR(d, rAdj, 0.1);
            }
        }
    }
    // Shape 7: multiple arcs - two full circles.
    {
        auto it = sf.find(7);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "MULTI");
            CHECK(it->second.segs.size() >= 8);
            if (!it->second.segs.empty()) {
                CHECK_NEAR(it->second.segs.front().x0, 0.0, 1e-9);
                CHECK_NEAR(it->second.segs.back().x1, 0.0, 1e-6);
                CHECK_NEAR(it->second.segs.back().y1, 0.0, 1e-6);
            }
        }
    }
    // Shape 8: clockwise half circle goes down (sign bit in the octant byte).
    {
        auto it = sf.find(8);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "ARCW");
            CHECK(it->second.segs.size() >= 4);
            if (!it->second.segs.empty()) {
                CHECK_NEAR(it->second.segs.front().x0, 0.0, 1e-9);
                CHECK(it->second.segs.front().y1 < 0.0);  // CW -> downward
                // Endpoint at -180 deg: (-16, 0).
                CHECK_NEAR(it->second.segs.back().x1, -16.0, 0.5);
                CHECK_NEAR(it->second.segs.back().y1, 0.0, 0.1);
            }
        }
    }
    // Shape 9: counter-clockwise half circle goes up.
    {
        auto it = sf.find(9);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "ARCCW");
            CHECK(it->second.segs.size() >= 4);
            if (!it->second.segs.empty()) {
                CHECK(it->second.segs.front().y1 > 0.0);  // CCW -> upward
                CHECK_NEAR(it->second.segs.back().x1, -16.0, 0.5);
                CHECK_NEAR(it->second.segs.back().y1, 0.0, 0.1);
            }
        }
    }
    // Shape 10: self-referencing subshape terminates through the depth guard.
    {
        auto it = sf.find(10);
        CHECK(it != sf.end());
        if (it != sf.end()) {
            CHECK(it->second.name == "SELF");
            CHECK(it->second.segs.empty());
        }
    }

    // ---- parseShapeBody on a raw definition ------------------------------
    // A square via a move plus single-byte vectors: move 2, then 0x20 (len 2
    // dir 0 = east), 0x24 (len 2 dir 4 = north), 0x28 (len 2 dir 8 = west),
    // 0x2C (len 2 dir 12 = south):
    //   (2,0) -> (4,0) -> (4,2) -> (2,2) -> (2,0).
    const unsigned char sq[] = {'S', 'Q', 'R', 0, 0x02, 0x02, 0x20, 0x24,
                                0x28, 0x2C, 0x00};
    ShapeDef sdef = parseShapeBody(sq, sizeof(sq));
    CHECK(sdef.name == "SQR");
    CHECK(sdef.segs.size() == 4);
    if (sdef.segs.size() == 4) {
        CHECK_NEAR(sdef.segs[0].x0, 2.0, 1e-9);   // starts at the moved pen
        CHECK_NEAR(sdef.segs[0].x1, 4.0, 1e-9);   // dir 0 (east): +2,+0
        CHECK_NEAR(sdef.segs[0].y1, 0.0, 1e-9);
        CHECK_NEAR(sdef.segs[1].x1, 4.0, 1e-9);   // dir 4 (north): +2 y
        CHECK_NEAR(sdef.segs[1].y1, 2.0, 1e-9);
        CHECK_NEAR(sdef.segs[2].x1, 2.0, 1e-9);   // dir 8 (west): -2,+0
        CHECK_NEAR(sdef.segs[2].y1, 2.0, 1e-9);
        CHECK_NEAR(sdef.segs[3].x1, 2.0, 1e-9);   // dir 12 (south): -2 y
        CHECK_NEAR(sdef.segs[3].y1, 0.0, 1e-9);
    }

    // ---- parseShapeBody with a subshape context --------------------------
    {
        const unsigned char subBody[] = {'B', 0, 0x07, 0x00, 0x01, 0x00};
        ShapeDef sd = parseShapeBody(subBody, sizeof(subBody), &sf);
        CHECK(sd.name == "B");
        CHECK(sd.segs.size() == 2);  // TRI expanded at the origin, scale 1
    }

    // ---- AutoCAD-86 container (ltypeshp.shx layout) ----------------------
    {
        std::vector<unsigned char> old;
        // 24-byte ASCII header.
        static const char kHead[] = "AutoCAD-86 shapes 1.0\r\n\x1A";
        old.insert(old.end(), kHead, kHead + 24);
        // Location table: first 130, last 134, count 5 (all LE).
        const unsigned char loc[] = {0x82, 0x00, 0x86, 0x00, 0x05, 0x00,
                                     0x82, 0x00, 0x0D, 0x00,   // 130, 13
                                     0x83, 0x00, 0x07, 0x00,   // 131,  7
                                     0x84, 0x00, 0x0A, 0x00,   // 132, 10
                                     0x85, 0x00, 0x0A, 0x00,   // 133, 10
                                     0x86, 0x00, 0x0A, 0x00};  // 134, 10
        old.insert(old.end(), loc, loc + sizeof(loc));
        // Shape 130 "TRACK1" (11 bytes, real data from ltypeshp.shx):
        //   0x14       len 1 dir 4 (north)                 -> (0,1)
        //   0x02 0x1C  move 28                             -> (28,1)
        //   0x01 0x1C  draw 28                             -> (56,1)
        const unsigned char t1[] = {'T', 'R', 'A', 'C', 'K', '1', 0,
                                    0x14, 0x02, 0x1C, 0x01, 0x1C, 0x00};
        old.insert(old.end(), t1, t1 + sizeof(t1));
        // Shape 131 "ZIG" (7 bytes, real data from ltypeshp.shx):
        //   0x12       len 1 dir 2 (NE 45 deg)             -> (1,1)
        //   0x1E       len 1 dir 14 (SE 315 deg)           -> (2,0)
        // The inverted-V must come out of these two bytes - a flat or long
        // 22.5-deg stroke here means the nibble order regressed.
        const unsigned char z1[] = {'Z', 'I', 'G', 0, 0x12, 0x1E, 0x00};
        old.insert(old.end(), z1, z1 + sizeof(z1));
        // Shape 132 "BOX" (10 bytes, real data from ltypeshp.shx):
        //   0x14       len 1 dir 4 (north)                 -> (0,1)
        //   0x20       len 2 dir 0 (east)                  -> (2,1)
        //   0x2C       len 2 dir 12 (south)                -> (2,-1)
        //   0x28       len 2 dir 8 (west)                  -> (0,-1)
        //   0x14       len 1 dir 4 (north)                 -> (0,0)
        const unsigned char bx[] = {'B', 'O', 'X', 0,
                                    0x14, 0x20, 0x2C, 0x28, 0x14, 0x00};
        old.insert(old.end(), bx, bx + sizeof(bx));
        // Shape 133 "CIRC1" (8 bytes, real data from ltypeshp.shx):
        //   0x0A 0x01 0xC0  full circle radius 1 (octant byte 0xC0: start 4,
        //                    count 0 = full circle), centred (1,0).
        const unsigned char c1[] = {'C', 'I', 'R', 'C', '1', 0,
                                    0x0A, 0x01, 0xC0, 0x00};
        old.insert(old.end(), c1, c1 + sizeof(c1));
        // Shape 134 "BAT" (10 bytes, real data from ltypeshp.shx):
        //   0x25       len 2 dir 5 (NNW 112.5 deg)         -> (-2t,2)
        //   0x0A 0x02 0xC4  octant arc radius 2, octant 0xC4 (CW, start 4,
        //                    count 4 = 180 deg lower half)
        //   0x2B       len 2 dir 11 (SSW 247.5 deg)        -> (-2t,-2)
        // where t = tan 22.5.  The arc swings from 180 deg (CW) to 0 deg.
        const unsigned char bt[] = {'B', 'A', 'T', 0,
                                    0x25, 0x0A, 0x02, 0xC4, 0x2B, 0x00};
        old.insert(old.end(), bt, bt + sizeof(bt));

        ShapeFile oldSf = parseShapeFileBytes(old.data(), old.size());
        CHECK(oldSf.size() == 5);
        {
            auto it = oldSf.find(130);
            CHECK(it != oldSf.end());
            if (it != oldSf.end()) {
                CHECK(it->second.name == "TRACK1");
                CHECK(it->second.segs.size() == 2);
                if (it->second.segs.size() == 2) {
                    CHECK_NEAR(it->second.segs[0].x0, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs[0].y0, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs[0].x1, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs[0].y1, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].x0, 28.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].y0, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].x1, 56.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].y1, 1.0, 1e-9);
                }
            }
        }
        {
            auto it = oldSf.find(131);
            CHECK(it != oldSf.end());
            if (it != oldSf.end()) {
                CHECK(it->second.name == "ZIG");
                CHECK(it->second.segs.size() == 2);
                if (it->second.segs.size() == 2) {
                    CHECK_NEAR(it->second.segs[0].x0, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs[0].y0, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs[0].x1, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[0].y1, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].x0, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].y0, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].x1, 2.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].y1, 0.0, 1e-9);
                }
            }
        }
        {
            auto it = oldSf.find(132);
            CHECK(it != oldSf.end());
            if (it != oldSf.end()) {
                CHECK(it->second.name == "BOX");
                // 5 vectors: N, E, S, W, N -> closed rectangle [0,2] x [-1,1].
                CHECK(it->second.segs.size() == 5);
                if (it->second.segs.size() == 5) {
                    CHECK_NEAR(it->second.segs[0].y1, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].x1, 2.0, 1e-9);
                    CHECK_NEAR(it->second.segs[1].y1, 1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[2].x1, 2.0, 1e-9);
                    CHECK_NEAR(it->second.segs[2].y1, -1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[3].x1, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs[3].y1, -1.0, 1e-9);
                    CHECK_NEAR(it->second.segs[4].x1, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs[4].y1, 0.0, 1e-9);
                }
            }
        }
        {
            auto it = oldSf.find(133);
            CHECK(it != oldSf.end());
            if (it != oldSf.end()) {
                CHECK(it->second.name == "CIRC1");
                CHECK(it->second.segs.size() >= 4);
                if (!it->second.segs.empty()) {
                    CHECK_NEAR(it->second.segs.front().x0, 0.0, 1e-9);
                    CHECK_NEAR(it->second.segs.back().x1, 0.0, 1e-6);
                    CHECK_NEAR(it->second.segs.back().y1, 0.0, 1e-6);
                    const std::size_t mid = it->second.segs.size() / 2;
                    const double mx = it->second.segs[mid].x0;
                    const double my = it->second.segs[mid].y0;
                    const double d =
                        std::sqrt((mx - 1.0) * (mx - 1.0) + my * my);
                    CHECK_NEAR(d, 1.0, 0.05);
                }
            }
        }
        {
            auto it = oldSf.find(134);
            CHECK(it != oldSf.end());
            if (it != oldSf.end()) {
                CHECK(it->second.name == "BAT");
                // At least the two straight strokes must be present.
                CHECK(it->second.segs.size() >= 2);
                const double t = 0.41421356237309503;
                bool hasUp = false;
                bool hasDown = false;
                bool hasArc = false;
                for (const ShapeSeg& s : it->second.segs) {
                    if (std::fabs(s.x1 - (-2.0 * t)) < 0.02
                        && std::fabs(s.y1 - 2.0) < 0.02) {
                        hasUp = true;
                    }
                    if (std::fabs(s.y0 - 2.0) > 1.5
                        && std::fabs(s.y1 - 2.0) < 1.5
                        && s.y1 < s.y0) {
                        hasDown = true;
                    }
                    if (std::fabs(s.y0 - 2.0) < 0.02
                        && std::fabs(s.y1 - 2.0) > 0.02) {
                        hasArc = true;  // arc leaves the (.,2) line
                    }
                }
                CHECK(hasUp);
                CHECK(hasDown);
                CHECK(hasArc);
            }
        }
        // shapeXSpan drives the complex-linetype run width (run = span x S):
        // ZIG spans [0,2], TRACK1 [0,56], BOX [0,2], CIRC1 [0,2],
        // BAT [-2t, 3.172] ~ 4.0.
        CHECK_NEAR(shapeXSpan(oldSf, 130), 56.0, 1e-9);
        CHECK_NEAR(shapeXSpan(oldSf, 131), 2.0, 1e-9);
        CHECK_NEAR(shapeXSpan(oldSf, 132), 2.0, 1e-9);
        CHECK_NEAR(shapeXSpan(oldSf, 133), 2.0, 1e-9);
        CHECK_NEAR(shapeXSpan(oldSf, 134), 4.0, 0.02);
        CHECK_NEAR(shapeXSpan(oldSf, 999), 0.0, 1e-9);
    }

    // ---- Malformed / missing input ---------------------------------------
    {
        std::vector<unsigned char> bad(1000, 0);
        bad[0] = 'X';  // wrong signature
        CHECK(parseShapeFileBytes(bad.data(), bad.size()).empty());
        CHECK(parseShapeFileBytes(nullptr, 0).empty());
        CHECK(parseShapeFile("no_such_shape_file.shx").empty());
    }

    if (g_fail == 0) {
        std::printf("ALL SHAPE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d SHAPE TEST(S) FAILED\n", g_fail);
    return 1;
}
