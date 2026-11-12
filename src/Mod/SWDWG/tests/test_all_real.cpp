//! Cross-check all user linetypes with their actual DWG pattern_len, LTSCALE,
//! scale and offsets.  Prints the InkSeg layout for each so a line of exactly
//! N periods is verified to be N whole periods (x = e1/2 at each end).
#include "../App/DWGLineCut.h"
#include "../App/DWGTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SWDWG;

struct Def
{
    const char* name;
    double patLen;       // DWG pattern_len (drawing units)
    double scale;        // LTSCALE
    std::vector<LineTypeDash> dashes;
};

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
    std::vector<Def> defs;

    // ---- BATTING (user DWG, LTSCALE=5) ----
    {
        Def d;
        d.name = "BATTING";
        d.patLen = 10.1625;
        d.scale = 5.0;
        d.dashes.resize(4);
        d.dashes[0].length = 0.00254;
        d.dashes[1].length = -2.54;
        d.dashes[1].shapeFlag = 2;
        d.dashes[1].complexShapecode = 134;
        d.dashes[1].scale = 2.54;
        d.dashes[1].xOffset = -2.54;
        d.dashes[1].shapefile = "ltypeshp.shx";
        d.dashes[1].symWidth = 4.0;
        d.dashes[2].length = -5.08;
        d.dashes[2].shapeFlag = 2;
        d.dashes[2].complexShapecode = 134;
        d.dashes[2].scale = 2.54;
        d.dashes[2].rotation = 3.141592653589793;
        d.dashes[2].xOffset = 2.54;
        d.dashes[2].shapefile = "ltypeshp.shx";
        d.dashes[2].symWidth = 4.0;
        d.dashes[3].length = -2.54;
        defs.push_back(std::move(d));
    }
    // ---- GAS_LINE (user DWG, LTSCALE=5) ----
    {
        Def d;
        d.name = "GAS_LINE";
        d.patLen = 22.86;
        d.scale = 5.0;
        d.dashes.resize(3);
        d.dashes[0].length = 12.7;
        d.dashes[1].length = -5.08;
        d.dashes[1].shapeFlag = 4;
        d.dashes[1].text = "GAS";
        d.dashes[1].scale = 2.54;
        d.dashes[1].xOffset = -2.54;
        d.dashes[1].yOffset = -1.27;
        d.dashes[2].length = -6.35;
        defs.push_back(std::move(d));
    }
    // ---- ZIGZAG (user DWG, LTSCALE=1) ----
    {
        Def d;
        d.name = "ZIGZAG";
        d.patLen = 20.3225;
        d.scale = 1.0;
        d.dashes.resize(4);
        d.dashes[0].length = 0.00254;
        d.dashes[1].length = -5.08;
        d.dashes[1].shapeFlag = 2;
        d.dashes[1].complexShapecode = 131;
        d.dashes[1].scale = 5.08;
        d.dashes[1].xOffset = -5.08;
        d.dashes[1].shapefile = "ltypeshp.shx";
        d.dashes[1].symWidth = 2.0;
        d.dashes[2].length = -10.16;
        d.dashes[2].shapeFlag = 2;
        d.dashes[2].complexShapecode = 131;
        d.dashes[2].scale = 5.08;
        d.dashes[2].rotation = 3.141592653589793;
        d.dashes[2].xOffset = 5.08;
        d.dashes[2].shapefile = "ltypeshp.shx";
        d.dashes[2].symWidth = 2.0;
        d.dashes[3].length = -5.08;
        defs.push_back(std::move(d));
    }
    // ---- TRACKS (user DWG, LTSCALE=1) ----
    {
        Def d;
        d.name = "TRACKS";
        d.patLen = 7.62;
        d.scale = 1.0;
        d.dashes.resize(2);
        d.dashes[0].length = 3.81;
        d.dashes[1].length = 3.81;
        d.dashes[1].shapeFlag = 2;
        d.dashes[1].complexShapecode = 130;
        d.dashes[1].scale = 6.35;
        d.dashes[1].shapefile = "ltypeshp.shx";
        d.dashes[1].symWidth = 56.0;
        defs.push_back(std::move(d));
    }

    for (const Def& d : defs) {
        std::printf("=== %s  patLen=%.6g  LTSCALE=%.1f ===\n", d.name,
                    d.patLen, d.scale);
        // 2 full periods in world units
        const double L = 2.0 * d.patLen * d.scale;
        auto segs = computeInkSegments(L, d.dashes, d.scale, d.patLen);
        std::printf("  L=%.6g -> %zu segs:\n", L, segs.size());
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const InkSeg& s = segs[i];
            if (s.complex) {
                std::printf("    [%zu] %s@%.4f insert=%.4f rot=%.2f scl=%.4g\n",
                            i, s.shapeFlag == 4 ? "TXT" : "SHP", s.sCenter,
                            s.sCenter + s.xOffset, s.symRotation, s.symScale);
            }
            else if (s.dot) {
                std::printf("    [%zu] dot@%.4f\n", i, s.s0);
            }
            else {
                std::printf("    [%zu] ink[%.4f, %.4f]\n", i, s.s0, s.s1);
            }
        }
        // Verify first ink ends at e1/2 (x) and last ink starts at L - x,
        // where e1 is the scaled first-dash width (patLen may rescale it).
        {
            double psum = 0.0;  // drawing units
            for (const LineTypeDash& dd : d.dashes) {
                psum += dashLayoutWidth(dd);
            }
            double e1 = dashLayoutWidth(d.dashes[0]) * d.scale;
            const double psumWorld = psum * d.scale;
            const double pWorld = d.patLen * d.scale;
            if (d.patLen > 0.0 && psumWorld > 1e-12
                && pWorld >= 0.5 * psumWorld && pWorld <= 2.0 * psumWorld) {
                e1 *= pWorld / psumWorld;
            }
            if (!segs.empty() && !segs.front().complex && !segs.front().dot) {
                check(std::fabs(segs.front().s1 - e1 * 0.5) < 1e-3,
                      "first ink = e1/2 (whole periods)");
            }
            if (!segs.empty() && !segs.back().complex && !segs.back().dot) {
                check(std::fabs((L - segs.back().s0) - e1 * 0.5) < 1e-3,
                      "last ink = e1/2 (whole periods)");
            }
        }
        std::printf("\n");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}