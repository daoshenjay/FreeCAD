// Quick standalone test: feed the BATTING pattern extracted from the user's
// DWG into computeInkSegments and print the resulting InkSegs.  Used to
// verify that negative-length complex dashes are no longer treated as ink.
#include "../App/DWGLineCut.h"
#include "../App/DWGTypes.h"

#include <cstdio>

using namespace SWDWG;

int main()
{
    // Real BATTING from sample_AC1015.dwg:
    //   0.0001 dash, [BAT r=0 x=-0.1 S=0.1 len=-0.1],
    //   [BAT r=pi x=+0.1 S=0.1 len=-0.2], -0.1 gap
    std::vector<LineTypeDash> dashes(4);
    dashes[0].length = 0.0001;
    dashes[0].shapeFlag = 0;
    dashes[1].length = -0.1;
    dashes[1].shapeFlag = 2;
    dashes[1].complexShapecode = 134;
    dashes[1].scale = 0.1;
    dashes[1].rotation = 0.0;
    dashes[1].xOffset = -0.1;
    dashes[1].yOffset = 0.0;
    dashes[1].shapefile = "ltypeshp.shx";
    dashes[1].symWidth = 4.0;
    dashes[2].length = -0.2;
    dashes[2].shapeFlag = 2;
    dashes[2].complexShapecode = 134;
    dashes[2].scale = 0.1;
    dashes[2].rotation = 3.141592653589793;
    dashes[2].xOffset = 0.1;
    dashes[2].yOffset = 0.0;
    dashes[2].shapefile = "ltypeshp.shx";
    dashes[2].symWidth = 4.0;
    dashes[3].length = -0.1;
    dashes[3].shapeFlag = 0;

    constexpr double L = 2.0;  // 2 mm line: 5 full periods (5*0.4001=2.0005)
    std::vector<InkSeg> segs = computeInkSegments(L, dashes, 1.0);

    std::printf("P=%.6f  L=%.6f  -> %zu ink segments\n", 0.4001, L, segs.size());
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const InkSeg& s = segs[i];
        std::printf("  [%zu] s0=%.6f s1=%.6f len=%.6f%s%s sCenter=%.6f "
                    "shape=%d code=%d xOff=%.4f yOff=%.4f\n",
                    i, s.s0, s.s1, s.s1 - s.s0,
                    s.dot ? " DOT" : "",
                    s.complex ? " COMPLEX" : "",
                    s.sCenter, s.shapeFlag, s.complexShapecode,
                    s.xOffset, s.yOffset);
    }
    return 0;
}