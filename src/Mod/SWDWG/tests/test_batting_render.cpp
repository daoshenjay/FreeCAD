//! Visual verification: run the real BATTING dashes (from
//! sample_AC1015.dwg) through computeInkSegments and render the BAT
//! shapes back to SVG.  Compare with AutoCAD's reference output to confirm
//! the two bulbs meet at the centre instead of being separated.
#include "DWGLineCut.h"
#include "DWGShapeFile.h"
#include "DWGTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SWDWG;

static double g_x = 0.0;
static double g_y = 0.0;

static void seg(double x0, double y0, double x1, double y1, const char* c)
{
    std::printf("  <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
                "stroke=\"%s\" stroke-width=\"1.4\"/>\n",
                g_x + x0, g_y - y0, g_x + x1, g_y - y1, c);
}

static void drawShape(const ShapeFile& sf, int number, double s, double r,
                      double xo, double yo, const char* color)
{
    auto it = sf.find(number);
    if (it == sf.end()) {
        return;
    }
    const double cr = std::cos(r);
    const double sr = std::sin(r);
    const auto& segs = it->second.segs;
    for (const ShapeSeg& sh : segs) {
        const double x0 = sh.x0 * cr - sh.y0 * sr;
        const double y0 = sh.x0 * sr + sh.y0 * cr;
        const double x1 = sh.x1 * cr - sh.y1 * sr;
        const double y1 = sh.x1 * sr + sh.y1 * cr;
        seg(xo + x0 * s, yo + y0 * s, xo + x1 * s, yo + y1 * s, color);
    }
    // Implicit base: complex outline (>=3 segs) with first and last
    // endpoints on y_min has its base drawn automatically (BAT in BATTING
    // is the canonical case).  Simple zigzags (2 segs) are left alone.
    if (segs.size() >= 3) {
        double yMin = 1e300;
        for (const ShapeSeg& sh : segs) {
            yMin = std::min(yMin, std::min(sh.y0, sh.y1));
        }
        const double yFirst = segs.front().y0;
        const double yLast = segs.back().y1;
        if (std::fabs(yFirst - yMin) < 1e-6
            && std::fabs(yLast - yMin) < 1e-6) {
            const double ax = segs.front().x0 * cr - segs.front().y0 * sr;
            const double bx = segs.back().x1 * cr - segs.back().y1 * sr;
            if (std::fabs(ax - bx) > 1e-6) {
                seg(xo + ax * s, yo + yFirst * s,
                    xo + bx * s, yo + yLast * s, color);
            }
        }
    }
}

int main()
{
    const char* shx = "C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/"
                      "Support/ltypeshp.shx";
    ShapeFile sf = parseShapeFile(shx);
    if (sf.empty()) {
        std::printf("FAIL: cannot parse %s\n", shx);
        return 1;
    }

    // BATTING from user's actual DWG (Layer_2 log dump):
    //   A,0.00254,-2.54,[BAT,s=2.54,r=0,x=-2.54],-5.08,
    //       [BAT,s=2.54,r=pi,x=+2.54],-2.54
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

    constexpr double LTS = 5.0;            // LTSCALE from log
    const double P = 10.16254 * LTS;       // pattern_len in world units

    // Diagnostic: for a handful of line lengths print the ink / BAT layout.
    // The user expects the head/tail endpoints to *meet*: the first dash
    // (e1) must sit exactly half at each end ('A' alignment), so a closed
    // path stays periodic.
    const double lengths[] = {P, 1.5 * P, 2.0 * P, 3.0 * P, 3.8 * LTS};
    for (const double L : lengths) {
        auto segs = computeInkSegments(L, dashes, LTS);
        std::printf("L=%.6f  (%zu segs):", L, segs.size());
        for (const InkSeg& s : segs) {
            if (s.complex) {
                std::printf(" BAT@%.4f(r=%.2f,xOff=%.2f)",
                            s.sCenter, s.symRotation, s.xOffset);
            }
            else if (s.dot) {
                std::printf(" dot@%.4f", s.s0);
            }
            else {
                std::printf(" ink[%.4f,%.4f]", s.s0, s.s1);
            }
        }
        std::printf("\n");
        // Also print the two "virtual" neighbours that a closed path would
        // connect: head-BAT and tail-BAT wrap-around distance.
        for (const InkSeg& s : segs) {
            if (s.complex) {
                const double ins = s.sCenter + s.xOffset;  // insertion point
                std::printf("    insertion=%.6f  ", ins);
            }
        }
        std::printf("\n");
    }
    return 0;
}