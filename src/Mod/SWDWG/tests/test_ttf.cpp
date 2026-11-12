//! Standalone test for FTTextRenderer: resolve fonts, render ASCII + CJK
//! text lines, print outline stats and bbox.
#include "FTTextRenderer.h"

#include <cstdio>
#include <string>

using namespace SWDWG;

int main()
{
    // ASCII via arial.ttf (GAS_LINE's font)
    std::string f = resolveFontFile("arial.ttf");
    std::fprintf(stderr, "resolveFontFile('arial.ttf') -> '%s'\n", f.c_str());
    if (!f.empty()) {
        TTFRenderResult r = renderTextLine("GAS", f, 2.54 * 5.0);
        std::fprintf(stderr,
                     "GAS: ok=%d contours=%zu width=%.4f asc=%.4f desc=%.4f\n",
                     (int)r.ok, r.contours.size(), r.width, r.ascender,
                     r.descender);
        double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
        for (const auto& c : r.contours) {
            for (const auto& p : c.pts) {
                x0 = x0 < p.first ? x0 : p.first;
                x1 = x1 > p.first ? x1 : p.first;
                y0 = y0 < p.second ? y0 : p.second;
                y1 = y1 > p.second ? y1 : p.second;
            }
        }
        std::fprintf(stderr, "  bbox x[%.3f, %.3f] y[%.3f, %.3f]\n", x0, x1,
                     y0, y1);
    }

    // CJK via the shared font mapper -> STFangsong / SimSun etc.
    std::string fcjk = resolveFontFile("STFangsong");
    std::fprintf(stderr, "resolveFontFile('STFangsong') -> '%s'\n",
                 fcjk.c_str());
    if (fcjk.empty()) {
        fcjk = resolveFontFile("SimSun");
        std::fprintf(stderr, "resolveFontFile('SimSun') -> '%s'\n",
                     fcjk.c_str());
    }
    if (!fcjk.empty()) {
        TTFRenderResult r = renderTextLine("道路", fcjk, 2.54);
        std::fprintf(stderr, "CJK '道路': ok=%d contours=%zu width=%.4f\n",
                     (int)r.ok, r.contours.size(), r.width);
    }

    // measureTextWidth: family-based measurement used for DWG text layout.
    double wA = measureTextWidth("arial.ttf", "GAS", 2.54);
    double wCjk = measureTextWidth("STFangsong", "道路", 2.54);
    if (wCjk < 0.0) {
        wCjk = measureTextWidth("SimSun", "道路", 2.54);
    }
    std::fprintf(stderr, "measureTextWidth: GAS@arial=%.4f 道路@CJK=%.4f\n",
                 wA, wCjk);
    // A fullwidth CJK char is ~1 em and 'GAS' with widthFactor 1 is wider
    // than its cap height; sanity checks only (fonts vary by platform).
    if (wA > 0.0 && wA > 2.0 * 2.54) {
        std::fprintf(stderr, "  FAIL: GAS width %.4f too large\n", wA);
        return 1;
    }
    if (wCjk > 0.0 && wCjk > 3.5 * 2.54) {
        std::fprintf(stderr, "  FAIL: CJK width %.4f too large\n", wCjk);
        return 1;
    }
    return 0;
}
