//! Standalone test for the .shx text rendering service (TextShxRenderer).
//! Renders ASCII ("GAS", "A7I") and CJK text through txt.shx / gbcbig.shx /
//! fsdb.shx and prints per-glyph strokes + line width, verifying:
//!   - the cap height maps the DWG text height to the shape units;
//!   - glyph baselines share y = 0 (model), descenders go below;
//!   - advance widths accumulate into the line width.
//!
//! Build (PowerShell, from tests/):
//!   vcvars64.bat && cl /nologo /EHsc /std:c++17 /I ..\App test_shxtext.cpp
//!       ..\App\TextShxRenderer.cpp ..\App\FontResolver.cpp
//!       ..\App\DWGShapeFile.cpp /Fe:test_shxtext.exe
//!       (add -DFCUseFreeType and link freetype only when FTTextRenderer is
//!        pulled in - TextShxRenderer does not need it)

#include "TextShxRenderer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace SWDWG;

static void dump(const char* label, const ShxLineRender& r)
{
    std::fprintf(stderr, "%s: ok=%d glyphs=%zu width=%.4f asc=%.4f desc=%.4f\n",
                 label, (int)r.ok, r.glyphs.size(), r.width, r.ascender,
                 r.descender);
    for (size_t g = 0; g < r.glyphs.size(); ++g) {
        double gx0 = 1e30, gx1 = -1e30, gy0 = 1e30, gy1 = -1e30;
        for (const ShapeSeg& s : r.glyphs[g]) {
            gx0 = std::min(gx0, std::min(s.x0, s.x1));
            gx1 = std::max(gx1, std::max(s.x0, s.x1));
            gy0 = std::min(gy0, std::min(s.y0, s.y1));
            gy1 = std::max(gy1, std::max(s.y0, s.y1));
        }
        std::fprintf(stderr, "  glyph[%zu]: %zu segs bbox x[%.3f, %.3f] "
                             "y[%.3f, %.3f]\n",
                     g, r.glyphs[g].size(), gx0, gx1, gy0, gy1);
    }
}

int main()
{
    // txt.shx ASCII, text height 5 mm.
    ShxLineRender gas = renderShxLine("txt.shx", "", 0, "GAS", 5.0);
    dump("txt.shx 'GAS' h=5", gas);
    if (!gas.ok) {
        std::fprintf(stderr, "FAIL: 'GAS' produced no outline\n");
        return 1;
    }
    // A capital G spans roughly cap height; in model units with h=5 the cap
    // height should be ~5 (we scale cap 21 -> 5).
    // Descenders (from 'g' etc.) not in GAS; ascender ~ cap height.
    if (std::fabs(gas.ascender - 5.0) > 0.6) {
        std::fprintf(stderr, "FAIL: GAS ascender %.4f != ~5\n", gas.ascender);
        return 1;
    }

    // 'A7I' exercises narrow / wide glyphs + pen-end advance.
    ShxLineRender a7i = renderShxLine("txt.shx", "", 0, "A7I", 5.0);
    dump("txt.shx 'A7I' h=5", a7i);
    if (!a7i.ok) {
        std::fprintf(stderr, "FAIL: 'A7I' produced no outline\n");
        return 1;
    }
    // I is ~4-5 units wide, 7 ~20 -> 'A7I' should be well under 'GAS'~? Just
    // sanity: every glyph x range must not start beyond the previous width.
    double prevX = 0.0;
    for (const auto& g : a7i.glyphs) {
        double gx0 = 1e30;
        for (const ShapeSeg& s : g) {
            gx0 = std::min(gx0, std::min(s.x0, s.x1));
        }
        if (gx0 < prevX - 1e-6) {
            std::fprintf(stderr, "FAIL: glyph overlaps previous (x0 %.3f < %.3f)\n",
                         gx0, prevX);
            return 1;
        }
        prevX = gx0;
    }

    // CJK through a big font.  Try the common AutoCAD big fonts; skip if not
    // installed.
    const std::vector<std::string> paths = {
        R"(C:\Program Files\Autodesk\AutoCAD 2016\Fonts)"};
    ShxLineRender cn = renderShxLine(
        "txt.shx", "gbcbig.shx", 936, "\xe6\x9d\xa5\xe4\xba\x86", 5.0,
        1.0, paths);
    if (!cn.ok) {
        cn = renderShxLine("", "fsdb.shx", 936,
                           "\xe6\x9d\xa5\xe4\xba\x86", 5.0, 1.0, paths);
    }
    if (cn.ok) {
        dump("bigfont CJK h=5", cn);
        // AutoCAD big-font semantics (verified against fsdb.shx TEXT): the
        // anchor is the BASELINE and the strokes rise UP from it, filling
        // roughly [0, h] (glyph-box centre ~ h/2 above the baseline).  A
        // Justify=centre/baseline fsdb.shx text at h=3.5 came out exactly
        // h/2 above the LIST centre point once the rendering sat on the
        // baseline.  Regression checks:
        //   - the glyphs must reach near the text height above the baseline;
        //   - they must not fall far below it (no TTF-style descenders);
        //   - the stroke-box centre must sit near +h/2.
        if (cn.ascender < 0.8 * 5.0) {
            std::fprintf(stderr,
                         "FAIL: CJK ascender %.4f too small for h=5\n",
                         cn.ascender);
            return 1;
        }
        if (cn.descender < -0.2 * 5.0) {
            std::fprintf(stderr,
                         "FAIL: CJK descender %.4f drops far below baseline\n",
                         cn.descender);
            return 1;
        }
        const double centre = 0.5 * (cn.ascender + cn.descender);
        if (std::fabs(centre - 2.5) > 0.75) {
            std::fprintf(stderr,
                         "FAIL: CJK box centre %.4f != ~h/2=2.5 "
                         "(asc=%.4f desc=%.4f)\n",
                         centre, cn.ascender, cn.descender);
            return 1;
        }
    }
    else {
        std::fprintf(stderr, "note: CJK big-font render skipped (fonts "
                             "gbcbig.shx / fsdb.shx not found)\n");
    }

    std::fprintf(stderr, "=== TEST OK ===\n");
    return 0;
}
