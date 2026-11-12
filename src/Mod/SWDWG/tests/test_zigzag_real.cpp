//! Render the user's actual ZIGZAG (LTSCALE=1) to SVG and dump InkSegs so
//! we can see exactly where every element lands.  Compare with AutoCAD.
#include "../App/DWGLineCut.h"
#include "../App/DWGShapeFile.h"
#include "../App/DWGTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SWDWG;

int main()
{
    // User DWG ZIGZAG: 0.00254, -5.08(ZIG r0 x=-5.08 s=5.08), -10.16(ZIG r=pi x=+5.08), -5.08
    std::vector<LineTypeDash> dashes(4);
    dashes[0].length = 0.00254;
    dashes[1].length = -5.08;
    dashes[1].shapeFlag = 2;
    dashes[1].complexShapecode = 131;
    dashes[1].scale = 5.08;
    dashes[1].xOffset = -5.08;
    dashes[1].shapefile = "ltypeshp.shx";
    dashes[1].symWidth = 2.0;   // ZIG X span
    dashes[2].length = -10.16;
    dashes[2].shapeFlag = 2;
    dashes[2].complexShapecode = 131;
    dashes[2].scale = 5.08;
    dashes[2].rotation = 3.141592653589793;
    dashes[2].xOffset = 5.08;
    dashes[2].shapefile = "ltypeshp.shx";
    dashes[2].symWidth = 2.0;
    dashes[3].length = -5.08;

    const double LTS = 1.0;   // ZIGZAG group scale from user log
    const double P = 20.3225; // 0.00254 + 5.08 + 10.16 + 5.08

    // 2 full periods so we see the wrap
    const double L = 2.0 * P;
    // Print the internally computed period so we can see float noise.
    double Pcalc = 0.0;
    for (const LineTypeDash& d : dashes) {
        Pcalc += dashLayoutWidth(d) * LTS;
    }
    std::printf("P calc = %.17g  L = %.17g  L/P = %.17g  DWG patLen=%.6g\n",
                Pcalc, L, L / Pcalc, 20.3225);
    auto segs = computeInkSegments(L, dashes, LTS, 20.3225);

    std::printf("ZIGZAG LTSCALE=%.1f  L=%.4f (%zu segs):\n", LTS, L, segs.size());
    std::printf("  pattern: ");
    for (const LineTypeDash& d : dashes) {
        std::printf("%.4f%s ", d.length, d.shapeFlag ? "(s)" : "");
    }
    std::printf("  P=%.4f\n", P);

    for (std::size_t i = 0; i < segs.size(); ++i) {
        const InkSeg& s = segs[i];
        if (s.complex) {
            const double ins = s.sCenter + s.xOffset;
            std::printf("  [%zu] ZIG@%.4f (sCenter) insert=%.4f rot=%.1f "
                        "scl=%.4f\n",
                        i, s.sCenter, ins, s.symRotation, s.symScale);
        }
        else if (s.dot) {
            std::printf("  [%zu] dot@%.4f\n", i, s.s0);
        }
        else {
            std::printf("  [%zu] ink[%.4f, %.4f]\n", i, s.s0, s.s1);
        }
    }
    return 0;
}