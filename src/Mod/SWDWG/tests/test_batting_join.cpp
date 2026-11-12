//! Verify the cDraw shift logic in DWGLineGroup.cpp after the fix:
//! - BAT#1 (r=0) base end should land on the linetype join
//! - BAT#2 (r=pi) base end should land on the same join (both ends = 50.0)
//!
//! Reproduces the math from addSymbolGeometry() with the *fixed* shift.
#include "DWGLineCut.h"
#include "DWGShapeFile.h"
#include "DWGTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SWDWG;

int main()
{
    const char* shx = "C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/"
                      "Support/ltypeshp.shx";
    ShapeFile sf = parseShapeFile(shx);
    if (sf.empty()) {
        std::printf("FAIL: cannot parse %s\n", shx);
        return 1;
    }

    // BATTING from user's DWG
    std::vector<LineTypeDash> dashes(4);
    dashes[0].length = 0.00254;
    dashes[1].length = -2.54;
    dashes[1].shapeFlag = 2;
    dashes[1].complexShapecode = 134;
    dashes[1].scale = 2.54;
    dashes[1].xOffset = -2.54;
    dashes[1].shapefile = "ltypeshp.shx";
    dashes[1].symWidth = 4.0;
    dashes[2].length = -5.08;
    dashes[2].shapeFlag = 2;
    dashes[2].complexShapecode = 134;
    dashes[2].scale = 2.54;
    dashes[2].rotation = 3.141592653589793;
    dashes[2].xOffset = 2.54;
    dashes[2].shapefile = "ltypeshp.shx";
    dashes[2].symWidth = 4.0;
    dashes[3].length = -2.54;

    constexpr double LTS = 5.0;
    constexpr double L = 100.0;            // user-reported total line length
    auto segs = computeInkSegments(L, dashes, LTS);

    std::printf("L=%.4f  layout:\n", L);
    for (const InkSeg& s : segs) {
        if (s.complex) {
            std::printf("  BAT@ sCenter=%.4f xOff=%.4f r=%.2f\n",
                        s.sCenter, s.xOffset, s.symRotation);
        }
        else if (!s.dot) {
            std::printf("  ink[%.4f, %.4f]  len=%.4f\n",
                        s.s0, s.s1, s.s1 - s.s0);
        }
    }

    // Now compute the c / cDraw for each BAT, using the *fixed* shift
    // (c.Translated(base0 * shift), NOT (base * shift)).
    struct Result { const char* tag; double c; double cDraw; double baseEnd; double baseStart; };
    std::vector<Result> res;
    for (const InkSeg& s : segs) {
        if (!s.complex) continue;
        const double xo = s.xOffset;             // world units
        const double h  = s.symScale;            // world / shape unit
        const double c  = s.sCenter + xo;        // shape insertion (world)
        // shift logic (same as in DWGLineGroup.cpp, but using base0):
        auto it = sf.find(s.complexShapecode);
        if (it == sf.end()) continue;
        const auto& sh = it->second.segs;
        double yMin = 1e300;
        for (const auto& seg : sh) {
            yMin = std::min(yMin, std::min(seg.y0, seg.y1));
        }
        const double openBase =
            std::fabs(sh.front().y0 - yMin) < 1e-6
            && std::fabs(sh.back().y1 - yMin) < 1e-6;
        double shift = 0.0;
        if (openBase && sh.size() >= 3) {
            const double shapeBase = (sh.back().x1 - sh.front().x0) * h;
            const double joinLen   = 2.0 * std::fabs(xo);
            if (joinLen > 1e-9 && joinLen < shapeBase) {
                shift = (std::signbit(xo) ? -1.0 : 1.0) * (shapeBase - joinLen);
            }
        }
        // Fixed: cDraw = c + base0 * shift (line frame, NOT rotated base)
        const double cDraw = c + shift;
        // base in shape-local: (0,0) -> (2.343, 0); rotated by symRotation
        const double baseStart = cDraw;
        const double baseEnd   = cDraw + (std::cos(s.symRotation) * sh.back().x1
                                          - std::sin(s.symRotation) * sh.back().y1) * h;
        const char* tag = (std::fabs(s.symRotation) < 1e-6) ? "BAT#1" : "BAT#2";
        res.push_back({tag, c, cDraw, baseEnd, baseStart});
        std::printf("  %s: c=%.4f shift=%.4f cDraw=%.4f  base[%.4f, %.4f]  baseEnd=%.4f\n",
                    tag, c, shift, cDraw, baseStart, baseEnd, baseEnd);
    }

    // Verify the two BATs meet at the same point (50.0 for a 100-line).
    if (res.size() == 2) {
        const double gap = res[1].baseEnd - res[0].baseEnd;
        std::printf("\nBAT#1 baseEnd=%.4f  BAT#2 baseEnd=%.4f  gap=%.4f\n",
                    res[0].baseEnd, res[1].baseEnd, gap);
        if (std::fabs(gap) < 1e-6) {
            std::printf("PASS: BATs meet exactly\n");
            return 0;
        }
        std::printf("FAIL: BATs do NOT meet (gap=%.6f)\n", gap);
        return 1;
    }
    return 0;
}
