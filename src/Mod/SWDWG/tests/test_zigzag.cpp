//! Visual check of complex-linetype shape placement: renders ZIGZAG / BATTING
//! from the real ltypeshp.shx with the ZIG shape decoded the AutoCAD way
//! (byte = length<<4 | direction), plus the shape outlines, into an SVG.
//!
//! The shape dash width in a linetype run is the shape's X span times the
//! element scale S (AutoCAD computes this and stores it in the DWG).  X/Y
//! offsets move the insertion point in the unrotated frame, measured from
//! the run end ("linetype definition vertex end" per the Customization
//! Guide).
//!
//! Build (PowerShell, from tests/):
//!   vcvars64.bat && cl /nologo /EHsc /std:c++17 /I ..\App test_zigzag.cpp
//!       ..\App\DWGShapeFile.cpp /Fe:test_zigzag.exe

#include "DWGShapeFile.h"

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
    for (const ShapeSeg& sh : it->second.segs) {
        const double x0 = sh.x0 * cr - sh.y0 * sr;
        const double y0 = sh.x0 * sr + sh.y0 * cr;
        const double x1 = sh.x1 * cr - sh.y1 * sr;
        const double y1 = sh.x1 * sr + sh.y1 * cr;
        seg(xo + x0 * s, yo + y0 * s, xo + x1 * s, yo + y1 * s, color);
    }
}

//! X span (shape units) of a shape outline.
static double xSpan(const ShapeFile& sf, int number)
{
    auto it = sf.find(number);
    if (it == sf.end()) {
        return 0.0;
    }
    double xmin = 1e18, xmax = -1e18;
    for (const ShapeSeg& s : it->second.segs) {
        if (s.x0 < xmin) xmin = s.x0;
        if (s.x1 < xmin) xmin = s.x1;
        if (s.x0 > xmax) xmax = s.x0;
        if (s.x1 > xmax) xmax = s.x1;
    }
    return (xmax > xmin) ? xmax - xmin : 0.0;
}

int main()
{
    const char* shx =
        "C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/Support/"
        "ltypeshp.shx";
    ShapeFile sf = parseShapeFile(shx);
    if (sf.empty()) {
        std::printf("FAIL: cannot parse %s\n", shx);
        return 1;
    }

    std::printf("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1400\" "
                "height=\"900\" viewBox=\"0 0 140 90\">\n");

    // ---- Row 1: ZIG / TRACK1 / BAT outlines (shape units, 1:1) ----------
    g_x = 4.0;
    g_y = 8.0;
    std::printf("<text x=\"%.1f\" y=\"%.1f\" font-size=\"2.2\">ZIG 1:1"
                "</text>\n", g_x, g_y - 2.5);
    drawShape(sf, 131, 1.0, 0.0, 0.0, 0.0, "#036");

    g_x = 22.0;
    std::printf("<text x=\"%.1f\" y=\"%.1f\" font-size=\"2.2\">TRACK1 "
                "x0.2</text>\n", g_x, g_y - 2.5);
    drawShape(sf, 130, 0.2, 0.0, 0.0, 0.0, "#036");

    g_x = 60.0;
    std::printf("<text x=\"%.1f\" y=\"%.1f\" font-size=\"2.2\">BAT s0.2"
                "</text>\n", g_x, g_y - 2.5);
    drawShape(sf, 134, 0.2, 0.0, 0.0, 0.0, "#036");

    // ---- Row 2: ZIGZAG - insertion at run end + X offset ----------------
    // acad.lin: A,.0001,-.2,[ZIG,x=-.2,s=.2],-.4,[ZIG,r=180,x=.2,s=.2],-.2
    // ZIG X span = 2 shape units, so its run width = 2 * 0.2 = 0.4.
    g_y = 22.0;
    std::printf("<text x=\"2\" y=\"%.1f\" font-size=\"2.2\">ZIGZAG "
                "(AutoCAD rule)</text>\n", g_y - 3);
    const double zW = xSpan(sf, 131) * 0.2;
    const double PZ = 0.0001 + 0.2 + zW + 0.4 + zW + 0.2;
    for (double s0 = 0.0; s0 < 8.0; s0 += PZ) {
        const double r1 = s0 + 0.0001 + 0.2;
        g_x = r1 + zW - 0.2;                       // run end + X offset
        drawShape(sf, 131, 0.2, 0.0, 0.0, 0.0, "#d33");
        const double r2 = r1 + zW + 0.4;
        g_x = r2 + zW + 0.2;
        drawShape(sf, 131, 0.2, 3.141592653589793, 0.0, 0.0, "#07b");
    }
    std::printf("  <line x1=\"0\" y1=\"%.3f\" x2=\"8\" y2=\"%.3f\" "
                "stroke=\"#bbb\" stroke-width=\"0.25\"/>\n", g_y, g_y);

    // ---- Row 3: ZIGZAG - insertion at run centre + X offset (FreeCAD) ----
    g_y = 34.0;
    std::printf("<text x=\"2\" y=\"%.1f\" font-size=\"2.2\">ZIGZAG "
                "(run centre + X offset, current FreeCAD)</text>\n",
                g_y - 3);
    for (double s0 = 0.0; s0 < 8.0; s0 += PZ) {
        const double r1 = s0 + 0.0001 + 0.2;
        g_x = r1 + zW * 0.5 - 0.2;                 // centre + X offset
        drawShape(sf, 131, 0.2, 0.0, 0.0, 0.0, "#d33");
        const double r2 = r1 + zW + 0.4;
        g_x = r2 + zW * 0.5 + 0.2;
        drawShape(sf, 131, 0.2, 3.141592653589793, 0.0, 0.0, "#07b");
    }
    std::printf("  <line x1=\"0\" y1=\"%.3f\" x2=\"8\" y2=\"%.3f\" "
                "stroke=\"#bbb\" stroke-width=\"0.25\"/>\n", g_y, g_y);

    // ---- Row 4: BATTING ------------------------------------------------
    // acad.lin: A,0.001,[BAT,s=.1,r=180,x=-.1],-.001,0.0,
    //           [BAT,s=.1,x=.1],-.001
    g_y = 50.0;
    std::printf("<text x=\"2\" y=\"%.1f\" font-size=\"2.2\">BATTING</text>\n",
                g_y - 3);
    const double bW = xSpan(sf, 134) * 0.1;
    const double PB = 0.001 + bW + 0.001 + bW + 0.001;
    for (double s0 = 0.0; s0 < 8.0; s0 += PB) {
        const double r1 = s0 + 0.001;
        g_x = r1 + bW - 0.1;
        drawShape(sf, 134, 0.1, 3.141592653589793, 0.0, 0.0, "#d33");
        const double r2 = r1 + bW + 0.001;
        g_x = r2 + bW + 0.1;
        drawShape(sf, 134, 0.1, 0.0, 0.0, 0.0, "#07b");
    }
    std::printf("  <line x1=\"0\" y1=\"%.3f\" x2=\"8\" y2=\"%.3f\" "
                "stroke=\"#bbb\" stroke-width=\"0.25\"/>\n", g_y, g_y);

    // ---- Row 5: TRACKS --------------------------------------------------
    // acad.lin: A,.15,[TRACK1,s=.05],-.2,[TRACK1,s=.05],-.2
    g_y = 68.0;
    std::printf("<text x=\"2\" y=\"%.1f\" font-size=\"2.2\">TRACKS</text>\n",
                g_y - 3);
    const double tW = xSpan(sf, 130) * 0.05;
    const double PT = 0.15 + tW + 0.2 + tW + 0.2;
    for (double s0 = 0.0; s0 < 8.0; s0 += PT) {
        const double r1 = s0 + 0.15;
        g_x = r1;
        drawShape(sf, 130, 0.05, 0.0, 0.0, 0.0, "#d33");
        const double r2 = r1 + tW + 0.2;
        g_x = r2;
        drawShape(sf, 130, 0.05, 0.0, 0.0, 0.0, "#07b");
    }
    std::printf("  <line x1=\"0\" y1=\"%.3f\" x2=\"8\" y2=\"%.3f\" "
                "stroke=\"#bbb\" stroke-width=\"0.25\"/>\n", g_y, g_y);

    std::printf("</svg>\n");
    return 0;
}
