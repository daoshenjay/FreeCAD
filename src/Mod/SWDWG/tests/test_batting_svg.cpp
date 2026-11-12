//! Standalone BATTING SVG renderer mirroring DWGLineGroup::addSymbolGeometry,
//! including the join-shift that makes the upper BAT's base end meet the
//! lower BAT's base start.
#include "../App/DWGLineCut.h"
#include "../App/DWGShapeFile.h"
#include "../App/DWGTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SWDWG;

struct V2 { double x = 0.0, y = 0.0; };
static V2 add(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
static V2 mul(V2 a, double k) { return {a.x * k, a.y * k}; }

static void drawShapeFull(const ShapeFile& sf, int number,
                         double s, double r, V2 origin,
                         double xOffsetWorld, const char* color)
{
    auto it = sf.find(number);
    if (it == sf.end()) return;
    const double cr = std::cos(r), sr = std::sin(r);
    const auto& segs = it->second.segs;

    // Join shift (same logic as DWGLineGroup.cpp): X offset == half the run
    // width, so the join distance is 2*|X offset|.  When the shape's raw
    // base is longer, shift the symbol so its base end lands on the join.
    double shift = 0.0;
    bool openBase = false;
    if (segs.size() >= 3) {
        double yMin = 1e300;
        for (const ShapeSeg& sh : segs) {
            yMin = std::min(yMin, std::min(sh.y0, sh.y1));
        }
        openBase = std::fabs(segs.front().y0 - yMin) < 1e-6
                   && std::fabs(segs.back().y1 - yMin) < 1e-6;
        if (openBase) {
            const double shapeBase = (segs.back().x1 - segs.front().x0) * s;
            const double joinLen = 2.0 * std::fabs(xOffsetWorld);
            if (joinLen > 1e-9 && joinLen < shapeBase) {
                const double sg = std::signbit(xOffsetWorld) ? -1.0 : 1.0;
                shift = sg * (shapeBase - joinLen);
            }
        }
    }
    V2 cDraw{origin.x + shift, origin.y};

    auto world = [cr, sr, cDraw, s](double lx, double ly) -> V2 {
        V2 local{cr * lx - sr * ly, sr * lx + cr * ly};
        return add(cDraw, mul(local, s));
    };

    for (const ShapeSeg& sh : segs) {
        V2 a = world(sh.x0, sh.y0);
        V2 b = world(sh.x1, sh.y1);
        std::printf("  <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
                    "stroke=\"%s\" stroke-width=\"0.6\"/>\n",
                    a.x, a.y, b.x, b.y, color);
    }
    if (openBase) {
        double yMin = 1e300;
        for (const ShapeSeg& sh : segs) {
            yMin = std::min(yMin, std::min(sh.y0, sh.y1));
        }
        V2 a = world(segs.front().x0, yMin);
        V2 b = world(segs.back().x1, yMin);
        std::printf("  <!-- base end: (%.3f, %.3f) shift=%.3f -->\n",
                    b.x, b.y, shift);
        if (std::fabs(a.x - b.x) > 1e-6) {
            std::printf("  <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
                        "stroke=\"%s\" stroke-width=\"1.6\"/>\n",
                        a.x, a.y, b.x, b.y, color);
        }
    }
}

int main()
{
    const char* shx = "C:/Program Files/Autodesk/AutoCAD 2016/Fonts/ltypeshp.shx";
    ShapeFile sf = parseShapeFile(shx);
    if (sf.empty()) {
        std::printf("FAIL: cannot parse %s\n", shx);
        return 1;
    }

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
    constexpr double PAT_LEN = 10.1625;
    const double L = 2.0 * PAT_LEN * LTS;
    auto segs = computeInkSegments(L, dashes, LTS, PAT_LEN);

    std::printf("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1600\" "
                "height=\"900\" viewBox=\"-60 -60 140 120\">\n");

    for (const InkSeg& ink : segs) {
        if (ink.complex) {
            V2 c{ink.sCenter + ink.xOffset, 0.0};
            std::printf("  <!-- marker sCenter=%.4f xOff=%.4f insert=(%.4f,0) -->\n",
                        ink.sCenter, ink.xOffset, c.x);
            drawShapeFull(sf, 134, ink.symScale, ink.symRotation, c,
                          ink.xOffset, "#d33");
        }
        else if (!ink.dot && ink.s1 - ink.s0 > 1e-9) {
            std::printf("  <line x1=\"%.3f\" y1=\"0\" x2=\"%.3f\" y2=\"0\" "
                        "stroke=\"#07b\" stroke-width=\"0.6\"/>\n",
                        ink.s0, ink.s1);
        }
    }
    std::printf("</svg>\n");
    return 0;
}