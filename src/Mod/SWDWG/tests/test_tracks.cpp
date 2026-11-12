//! Verify TRACKS (positive-length complex dash) layout and the TRACK1
//! shape geometry from ltypeshp.shx.
#include "DWGLineCut.h"
#include "DWGShapeFile.h"
#include "DWGTypes.h"

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
    // TRACK1 shape code 130.
    auto it = sf.find(130);
    if (it == sf.end()) {
        std::printf("FAIL: TRACK1(130) not found\n");
        return 1;
    }
    std::printf("TRACK1 segs:\n");
    double xMax = 0.0;
    for (const auto& s : it->second.segs) {
        std::printf("  (%.4f, %.4f) -> (%.4f, %.4f)\n",
                    s.x0, s.y0, s.x1, s.y1);
        xMax = std::max(xMax, std::max(s.x0, s.x1));
    }
    std::printf("  X span = %.4f\n", xMax);

    // TRACKS exactly as libredwg decodes it from the user DWG:
    //   dash[0] = 3.81 shape(TRACK1), dash[1] = 3.81 plain
    std::vector<LineTypeDash> dashes(2);
    dashes[0].length = 3.81;
    dashes[0].shapeFlag = 2;
    dashes[0].complexShapecode = 130;
    dashes[0].scale = 6.35;
    dashes[0].xOffset = 0.0;
    dashes[0].shapefile = "ltypeshp.shx";
    dashes[1].length = 3.81;
    dashes[1].shapeFlag = 0;

    const double LTS = 1.0;
    const double L = 100.0;   // user-reported line length
    auto segs = computeInkSegments(L, dashes, LTS, 7.62);
    std::printf("\nTRACKS L=%.4f -> %zu segs:\n", L, segs.size());
    for (const InkSeg& s : segs) {
        if (s.complex) {
            std::printf("  ink[%.4f, %.4f] TRACK1@%.4f (xOff=%.2f)\n",
                        s.s0, s.s1, s.sCenter, s.xOffset);
        }
        else if (!s.dot) {
            std::printf("  ink[%.4f, %.4f]\n", s.s0, s.s1);
        }
    }
    return 0;
}
