//! Verify computePlainCut (dot-leading fallback) handles complex (shape)
//! elements correctly across periods: the shape marker's sCenter must
//! advance with each period.
#include "../App/DWGLineCut.h"
#include "../App/DWGTypes.h"

#include <cstdio>
#include <vector>

using namespace SWDWG;

static int g_fail = 0;

static void check(bool ok, const char* msg)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) {
        ++g_fail;
    }
}

int main()
{
    // Dot-leading pattern with a negative-length shape element:
    //   0(dot) -1(gap) -0.5(gap + BAT) -1(gap)   P = 2.5
    std::vector<LineTypeDash> dashes(4);
    dashes[0].length = 0.0;  // dot
    dashes[1].length = -1.0;
    dashes[2].length = -0.5;
    dashes[2].shapeFlag = 2;
    dashes[2].complexShapecode = 134;
    dashes[2].scale = 0.5;
    dashes[2].xOffset = -0.5;
    dashes[2].shapefile = "ltypeshp.shx";
    dashes[2].symWidth = 4.0;
    dashes[3].length = -1.0;

    // L = 2 full periods exactly.
    const double P = 2.5;
    auto segs = computeInkSegments(5.0, dashes, 1.0);

    std::printf("L=5.0 (dot-leading + shape) -> %zu segs:\n", segs.size());
    std::vector<double> sCenters;
    for (const InkSeg& s : segs) {
        if (s.complex) {
            std::printf("  shape@%.4f\n", s.sCenter);
            sCenters.push_back(s.sCenter);
        }
        else if (s.dot) {
            std::printf("  dot@%.4f\n", s.s0);
        }
        else {
            std::printf("  ink[%.4f, %.4f]\n", s.s0, s.s1);
        }
    }
    check(sCenters.size() == 2, "two shape markers (one per period)");
    if (sCenters.size() == 2) {
        check(std::fabs(sCenters[0] - 1.5) < 1e-6,
              "period 1 shape at 1.5 (run end of gap 0.5)");
        check(std::fabs(sCenters[1] - 4.0) < 1e-6,
              "period 2 shape at 4.0 (2.5 + 1.5)");
    }

    // L = 1.5 periods -> remainder truncation must still stamp correctly.
    auto segs2 = computeInkSegments(3.75, dashes, 1.0);
    std::printf("L=3.75 -> %zu segs:\n", segs2.size());
    bool haveShapes = false;
    for (const InkSeg& s : segs2) {
        if (s.complex) {
            std::printf("  shape@%.4f\n", s.sCenter);
            haveShapes = true;
        }
    }
    check(haveShapes, "remainder still stamps the shape marker");

    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}