//! Standalone verification of the L2 'A'-alignment cutting (DWGLineCut.h).
//!
//! Compiles with plain MSVC / clang / gcc - no FreeCAD / OCCT needed:
//!   cl /EHsc /std:c++17 /I ..\App test_align.cpp
//!
//! Each case asserts the exact ink/dot sequence AutoCAD produces for the
//! given pattern and line length (see docs/18-线形映射体系.md 18.10.2).

#include "DWGTypes.h"
#include "DWGLineCut.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

using namespace SWDWG;

static int g_fail = 0;
static int g_case = 0;

static void check(bool cond, const char* what)
{
    if (!cond) {
        std::printf("  FAIL: %s\n", what);
        ++g_fail;
    }
}

static void closeTo(double a, double b, const char* what)
{
    if (std::fabs(a - b) > 1e-6) {
        std::printf("  FAIL: %s  (got %.9g, want %.9g)\n", what, a, b);
        ++g_fail;
    }
}

//! Parse "12.7,-6.35,0" into dashes (same rules as DWGLineType.cpp).
static std::vector<LineTypeDash> parse(const char* text)
{
    std::vector<LineTypeDash> dashes;
    const char* p = text;
    while (p && *p) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == ';') {
            ++p;
        }
        if (!*p) {
            break;
        }
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) {
            break;
        }
        LineTypeDash d;
        d.length = v;
        dashes.push_back(d);
        p = end;
    }
    return dashes;
}

//! "ink[0,12.5] dot@15.5 sym[20.282,24.854]@24.854(GAS) ..."
static std::string fmt(const std::vector<InkSeg>& segs)
{
    std::string s;
    for (const InkSeg& sg : segs) {
        char buf[96];
        if (sg.complex) {
            std::snprintf(buf, sizeof(buf), "sym[%.3f,%.3f]@%.3f(%s) ",
                          sg.s0, sg.s1, sg.sCenter,
                          (sg.shapeFlag == 4) ? sg.text.c_str() : "shape");
        }
        else if (sg.dot) {
            std::snprintf(buf, sizeof(buf), "dot@%.3f ", sg.s0);
        }
        else {
            std::snprintf(buf, sizeof(buf), "ink[%.3f,%.3f] ", sg.s0, sg.s1);
        }
        s += buf;
    }
    return s;
}

//! Validate general invariants shared by every cut: segments sorted, inside
//! [0, L], dots zero-length, ink positive, no overlap.
static void checkInvariants(const std::vector<InkSeg>& segs, double L)
{
    double prev = 0.0;
    for (const InkSeg& sg : segs) {
        check(sg.s0 >= -1e-9 && sg.s1 <= L + 1e-9, "segment inside [0,L]");
        if (sg.dot) {
            closeTo(sg.s1, sg.s0, "dot has zero length");
            check(sg.s0 >= prev - 1e-9, "dot not before previous segment");
            prev = sg.s0;
        }
        else {
            check(sg.s1 - sg.s0 > 1e-9, "ink segment has positive length");
            check(sg.s0 >= prev - 1e-9, "segments sorted / no overlap");
            prev = sg.s1;
        }
    }
    // A cut line must start and end on ink (or be empty / continuous).
    if (!segs.empty()) {
        closeTo(segs.front().s0, 0.0, "cut starts at 0");
        if (!segs.front().dot) {
            closeTo(segs.front().s0, 0.0, "starts on ink");
        }
    }
}

static void beginCase(const char* name)
{
    ++g_case;
    std::printf("[case %d] %s\n", g_case, name);
}

//! Convenience: run and dump a case.
static std::vector<InkSeg> run(const char* pattern, double L, double scale)
{
    std::vector<InkSeg> segs = computeInkSegments(L, parse(pattern), scale);
    std::printf("    L=%.4f scale=%.2f -> %s\n", L, scale, fmt(segs).c_str());
    return segs;
}

int main()
{
    // --- ISO05W100 (24,-3,0,-3,0,-3), P=33, on a 100 mm line ---
    // The user-observed AutoCAD sequence: 12.5 | dot dot | 24 | dot dot | 24
    // | dot dot | 12.5  (NOT 24 | dot dot | 24 | dot dot | 24 | dot dot | 1).
    beginCase("ISO05W100, L=100 (user-observed case)");
    {
        auto s = run("24,-3,0,-3,0,-3", 100.0, 1.0);
        checkInvariants(s, 100.0);
        check(s.size() == 10, "10 segments (4 ink + 6 dots)");
        closeTo(s[0].s0, 0.0, "first ink starts at 0");
        closeTo(s[0].s1, 12.5, "first ink = half first dash +1/2");
        closeTo(s[1].s0, 15.5, "dot 1 pos");
        closeTo(s[2].s0, 18.5, "dot 2 pos");
        closeTo(s[3].s0, 21.5, "middle dash start");
        closeTo(s[3].s1, 45.5, "middle dash = 24");
        closeTo(s[4].s0, 48.5, "dot 3 pos");
        closeTo(s[5].s0, 51.5, "dot 4 pos");
        closeTo(s[6].s0, 54.5, "middle dash 2 start");
        closeTo(s[6].s1, 78.5, "middle dash 2 = 24");
        closeTo(s[7].s0, 81.5, "dot 5 pos");
        closeTo(s[8].s0, 84.5, "dot 6 pos");
        closeTo(s[9].s0, 87.5, "last ink starts at 87.5");
        closeTo(s[9].s1, 100.0, "last ink ends at line end");
        closeTo(s[9].s1 - s[9].s0, 12.5, "last ink = 12.5");
    }

    // --- ISO05W100, L=60: m=0, x=25.5 > e1=24 (end dash grows, AutoCAD) ---
    beginCase("ISO05W100, L=60 (end dash > e1)");
    {
        auto s = run("24,-3,0,-3,0,-3", 60.0, 1.0);
        checkInvariants(s, 60.0);
        check(s.size() == 4, "4 segments (2 ink + 2 dots)");
        closeTo(s[0].s1, 25.5, "first ink = 25.5 (lengthened)");
        closeTo(s[1].s0, 28.5, "dot 1");
        closeTo(s[2].s0, 31.5, "dot 2");
        closeTo(s[3].s0, 34.5, "last ink start");
        closeTo(s[3].s1, 60.0, "last ink end");
        closeTo(s[3].s1 - s[3].s0, 25.5, "last ink = 25.5 (lengthened)");
    }

    // --- ISO05W100, L=40: single period, x=15.5 ---
    beginCase("ISO05W100, L=40");
    {
        auto s = run("24,-3,0,-3,0,-3", 40.0, 1.0);
        checkInvariants(s, 40.0);
        closeTo(s[0].s1, 15.5, "first ink = 15.5");
        closeTo(s[3].s1, 40.0, "last ink reaches line end");
        closeTo(s[3].s1 - s[3].s0, 15.5, "last ink = 15.5");
    }

    // --- ISO02 (12,-3), L=100: m=5, x=11 ---
    beginCase("ISO02, L=100");
    {
        auto s = run("12,-3", 100.0, 1.0);
        checkInvariants(s, 100.0);
        check(s.size() == 7, "7 ink segments");
        closeTo(s[0].s1 - s[0].s0, 11.0, "first ink = 11");
        closeTo(s[6].s1 - s[6].s0, 11.0, "last ink = 11");
        closeTo(s[6].s1, 100.0, "last ink ends at L");
        for (int i = 1; i < 6; ++i) {
            closeTo(s[i].s1 - s[i].s0, 12.0, "middle ink = 12");
        }
    }

    // --- ISO06 (24,-3,0,-3,0,-3,0,-3), P=36, L=100: m=1, x=26 ---
    // AutoCAD shows: 26 | 3 dots | 24 | 3 dots | 26 (end dashes > 24).
    beginCase("ISO06, L=100");
    {
        auto s = run("24,-3,0,-3,0,-3,0,-3", 100.0, 1.0);
        checkInvariants(s, 100.0);
        check(s.size() == 9, "9 segments (3 ink + 6 dots)");
        closeTo(s[0].s1, 26.0, "first ink = 26 (lengthened)");
        closeTo(s[1].s0, 29.0, "dot 1");
        closeTo(s[2].s0, 32.0, "dot 2");
        closeTo(s[3].s0, 35.0, "dot 3");
        closeTo(s[4].s1 - s[4].s0, 24.0, "middle dash 1 = 24");
        closeTo(s[5].s0, 65.0, "dot 4");
        closeTo(s[6].s0, 68.0, "dot 5");
        closeTo(s[7].s0, 71.0, "dot 6");
        closeTo(s[8].s1 - s[8].s0, 26.0, "last ink = 26 (lengthened)");
        closeTo(s[8].s1, 100.0, "last ink ends at L");
    }

    // --- CENTER (31.75,-6.35,6.35,-6.35), L=100: m=0, x=40.475 ---
    // AutoCAD shows: 40.475 | 6.35 | 40.475 (long-short-long, end dashes > e1).
    beginCase("CENTER, L=100");
    {
        auto s = run("31.75,-6.35,6.35,-6.35", 100.0, 1.0);
        checkInvariants(s, 100.0);
        check(s.size() == 3, "3 ink segments (long-short-long)");
        closeTo(s[0].s1 - s[0].s0, 40.475, "first ink = 40.475");
        closeTo(s[0].s0, 0.0, "first ink starts at 0");
        closeTo(s[1].s1 - s[1].s0, 6.35, "short dash = 6.35");
        closeTo(s[1].s0, 46.825, "short dash start");
        closeTo(s[2].s1 - s[2].s0, 40.475, "last ink = 40.475");
        closeTo(s[2].s1, 100.0, "last ink ends at L");
    }

    // --- DASHED (12.7,-6.35), L=20: x=6.825 ---
    beginCase("DASHED, L=20");
    {
        auto s = run("12.7,-6.35", 20.0, 1.0);
        checkInvariants(s, 20.0);
        check(s.size() == 2, "2 ink segments");
        closeTo(s[0].s1, 6.825, "first ink = 6.825");
        closeTo(s[1].s0, 13.175, "last ink start");
        closeTo(s[1].s1, 20.0, "last ink end = L");
    }

    // --- DASHED, L exactly one period (19.05): x = e1/2 = 6.35 ---
    beginCase("DASHED, L = P exactly");
    {
        auto s = run("12.7,-6.35", 19.05, 1.0);
        checkInvariants(s, 19.05);
        closeTo(s[0].s1, 6.35, "first ink = 6.35");
        closeTo(s[1].s0, 12.7, "last ink start");
        closeTo(s[1].s1, 19.05, "last ink end = L");
    }

    // --- DASHED, L=10 < P=19.05: continuous ---
    beginCase("DASHED, L=10 too short -> continuous");
    {
        auto s = run("12.7,-6.35", 10.0, 1.0);
        check(s.size() == 1 && !s[0].dot, "single full ink segment");
        closeTo(s[0].s1, 10.0, "covers whole line");
    }

    // --- BORDER (12.7,-6.35,0,-6.35,0,-6.35), L=20 < P=31.75: continuous ---
    beginCase("BORDER, L=20 too short -> continuous");
    {
        auto s = run("12.7,-6.35,0,-6.35,0,-6.35", 20.0, 1.0);
        check(s.size() == 1 && !s[0].dot, "single full ink segment");
        closeTo(s[0].s1, 20.0, "covers whole line");
    }

    // --- DOT (0,-6.35): dot-leading -> plain cut fallback ---
    beginCase("DOT, L=100 (plain cut fallback)");
    {
        auto s = run("0,-6.35", 100.0, 1.0);
        checkInvariants(s, 100.0);
        check(s.size() == 16, "16 dots (15 full periods + remainder)");
        for (std::size_t i = 0; i < s.size(); ++i) {
            check(s[i].dot, "all segments are dots");
            closeTo(s[i].s0, 6.35 * (double)i, "dot at period offset");
        }
    }

    // --- ZIGZAG (tiny first element): must stay valid either way ---
    beginCase("ZIGZAG, L=100 (tiny e1)");
    {
        auto s = run("0.00254,-0.00254,0.00254,-0.00254,0.00254,-0.00254,"
                     "0.00254,-0.00254,0.00254,-0.00254,0.00254,-0.00254,"
                     "0.00254",
                     100.0, 1.0);
        checkInvariants(s, 100.0);
        check(!s.empty(), "non-empty output");
    }

    // --- BORDER2 (6.35,-3.175,6.35,-3.175,0,-3.175), P=22.225, L=100:
    // x=8.725 > e1=6.35 and x2=-2.39 < 0 -> first dash must be lengthened
    // (NOT plain-cut fallback).  9 ink + 4 dots = 13 segments.
    beginCase("BORDER2, L=100 (end dash > e1)");
    {
        auto s = run("6.35,-3.175,6.35,-3.175,0,-3.175", 100.0, 1.0);
        checkInvariants(s, 100.0);
        check(s.size() == 13, "13 segments (9 ink + 4 dots)");
        closeTo(s[0].s1, 8.725, "first ink = 8.725 (lengthened)");
        closeTo(s[1].s1 - s[1].s0, 6.35, "dash 1 = 6.35");
        closeTo(s[2].s0, 21.425, "dot 1");
        closeTo(s[3].s1 - s[3].s0, 6.35, "dash 2 = 6.35");
        closeTo(s[4].s1 - s[4].s0, 6.35, "dash 3 = 6.35");
        closeTo(s[5].s0, 43.65, "dot 2");
        closeTo(s[6].s1 - s[6].s0, 6.35, "dash 4 = 6.35");
        closeTo(s[7].s1 - s[7].s0, 6.35, "dash 5 = 6.35");
        closeTo(s[8].s0, 65.875, "dot 3");
        closeTo(s[9].s1 - s[9].s0, 6.35, "dash 6 = 6.35");
        closeTo(s[10].s1 - s[10].s0, 6.35, "dash 7 = 6.35");
        closeTo(s[11].s0, 88.1, "dot 4");
        closeTo(s[12].s1 - s[12].s0, 8.725, "last ink = 8.725 (lengthened)");
        closeTo(s[12].s1, 100.0, "last ink end = L");
    }

    // --- ISO05W100 with scale=2 (LTSCALE=2): pattern doubles ---
    beginCase("ISO05W100, L=100, scale=2");
    {
        auto s = run("24,-3,0,-3,0,-3", 100.0, 2.0);
        checkInvariants(s, 100.0);
        closeTo(s[0].s1, 41.0, "first ink = 41");
        closeTo(s[1].s0, 47.0, "dot 1");
        closeTo(s[2].s0, 53.0, "dot 2");
        closeTo(s[3].s0, 59.0, "last ink start");
        closeTo(s[3].s1, 100.0, "last ink end = L");
    }

    // --- Degenerate pattern (all gaps) -> continuous ---
    beginCase("all-space pattern -> continuous");
    {
        auto s = run("-1,-2,-3", 50.0, 1.0);
        check(s.size() == 1 && !s[0].dot, "single full ink segment");
        closeTo(s[0].s1, 50.0, "covers whole line");
    }

    // --- ACAD_ISO06W100 (12,-4.2,-0.5,0,-0.5,0,-4.2), P=21.4, L=100:
    // m=3, x=13.2 > e1=12 (end dash grows) ---
    beginCase("ACAD_ISO06W100, L=100 (long-dash double-dot)");
    {
        auto s = run("12,-4.2,-0.5,0,-0.5,0,-4.2", 100.0, 1.0);
        checkInvariants(s, 100.0);
        check(s.size() == 13, "13 segments (5 ink + 8 dots)");
        closeTo(s[0].s1, 13.2, "first ink = 13.2 (lengthened)");
        closeTo(s[1].s0, 17.9, "dot 1");
        closeTo(s[2].s0, 18.4, "dot 2");
        closeTo(s[3].s1 - s[3].s0, 12.0, "dash 1 = 12");
        closeTo(s[4].s0, 39.3, "dot 3");
        closeTo(s[5].s0, 39.8, "dot 4");
        closeTo(s[6].s1 - s[6].s0, 12.0, "dash 2 = 12");
        closeTo(s[7].s0, 60.7, "dot 5");
        closeTo(s[8].s0, 61.2, "dot 6");
        closeTo(s[9].s1 - s[9].s0, 12.0, "dash 3 = 12");
        closeTo(s[10].s0, 82.1, "dot 7");
        closeTo(s[11].s0, 82.6, "dot 8");
        closeTo(s[12].s0, 86.8, "last ink start");
        closeTo(s[12].s1, 100.0, "last ink end = L");
        closeTo(s[12].s1 - s[12].s0, 13.2, "last ink = 13.2 (lengthened)");
    }

    // --- BORDER2 (9.525,-3.175,3.175,-3.175,0,-3.175), P=22.225, L=50:
    // x=7.5375, dash-short-dot-dash layout ---
    beginCase("BORDER2, L=50 (dash dot)");
    {
        auto s = run("9.525,-3.175,3.175,-3.175,0,-3.175", 50.0, 1.0);
        checkInvariants(s, 50.0);
        check(s.size() == 7, "7 segments (6 ink + 1 dot pair)");
        closeTo(s[0].s1, 7.5375, "first ink = 7.5375");
        closeTo(s[1].s1 - s[1].s0, 3.175, "short dash = 3.175");
        closeTo(s[2].s0, 17.0625, "dot 1");
        closeTo(s[3].s1 - s[3].s0, 9.525, "dash = 9.525");
        closeTo(s[4].s1 - s[4].s0, 3.175, "short dash = 3.175");
        closeTo(s[5].s0, 39.2875, "dot 2");
        closeTo(s[6].s1, 50.0, "last ink end = L");
        closeTo(s[6].s1 - s[6].s0, 7.5375, "last ink = 7.5375");
    }

    // --- GAS_LINE (12.7,-5.08,[GAS,S=2.54],-5.08), L=100 ---
    // The complex "GAS" element has no explicit length in the LINETYPE text,
    // so its layout width is the symbol width (0.6 * S * 3 chars = 4.572).
    // P = 12.7+5.08+4.572+5.08 = 27.432; m=2; x=15.202.
    // Symbol insertion points sit at the centre of each complex run:
    // 22.568 | 50.0 | 77.432.
    beginCase("GAS_LINE complex text, L=100");
    {
        std::vector<LineTypeDash> gas;
        LineTypeDash d1;
        d1.length = 12.7;
        gas.push_back(d1);
        LineTypeDash gap;
        gap.length = -5.08;
        gas.push_back(gap);
        LineTypeDash txt;  // ["GAS",STANDARD,S=2.54,X=-2.54,Y=-1.27]
        txt.length = 0.0;
        txt.shapeFlag = 4;
        txt.text = "GAS";
        txt.scale = 2.54;
        txt.xOffset = -2.54;
        txt.yOffset = -1.27;
        gas.push_back(txt);
        gas.push_back(gap);

        auto s = computeInkSegments(100.0, gas, 1.0);
        std::printf("    L=100 scale=1.00 -> %s\n", fmt(s).c_str());
        checkInvariants(s, 100.0);
        check(s.size() == 7, "7 segments (4 ink + 3 text symbols)");
        closeTo(s[0].s1, 15.202, "first ink = x = 15.202");
        check(s[1].complex, "segment 1 is a complex symbol");
        closeTo(s[1].sCenter, 24.854, "first GAS inserted at run end");
        check(s[1].shapeFlag == 4, "text symbol flag");
        check(s[1].text == "GAS", "embedded text preserved");
        closeTo(s[1].symScale, 2.54, "S scale preserved");
        closeTo(s[1].xOffset, -2.54, "X offset preserved");
        closeTo(s[2].s1 - s[2].s0, 12.7, "period dash = 12.7");
        closeTo(s[2].s0, 29.934, "period dash start");
        check(s[3].complex, "second period symbol");
        closeTo(s[3].sCenter, 52.286, "second GAS inserted at run end");
        closeTo(s[4].s0, 57.366, "third period dash start");
        check(s[5].complex, "third period symbol");
        closeTo(s[5].sCenter, 79.718, "third GAS inserted at run end");
        closeTo(s[6].s1, 100.0, "last ink reaches line end");
        closeTo(s[6].s1 - s[6].s0, 15.202, "last ink = x = 15.202");
    }

    // --- TRACKS (0.15,[TRACK1,S=0.5],-0.25), L=1.8 ---
    // Zero-length shape element: layout width = S = 0.5 (no .shx resolved, so
    // symWidth stays 0 and the width falls back to S); P = 0.9; m=1; x=0.075.
    // Shape insertion at the run END (linetype definition vertex): markers at
    // 0.575 and 1.475.
    beginCase("TRACKS complex shape, L=1.8");
    {
        std::vector<LineTypeDash> trk;
        LineTypeDash d1;
        d1.length = 0.15;
        trk.push_back(d1);
        LineTypeDash shp;  // [TRACK1,ltypeshp.shx,S=0.5]
        shp.length = 0.0;
        shp.shapeFlag = 2;
        shp.complexShapecode = 5;
        shp.scale = 0.5;
        shp.shapefile = "ltypeshp.shx";
        trk.push_back(shp);
        LineTypeDash gap;
        gap.length = -0.25;
        trk.push_back(gap);

        auto s = computeInkSegments(1.8, trk, 1.0);
        std::printf("    L=1.8 scale=1.00 -> %s\n", fmt(s).c_str());
        checkInvariants(s, 1.8);
        check(s.size() == 5, "5 segments (3 ink + 2 shape symbols)");
        closeTo(s[0].s1, 0.075, "first ink = x = 0.075");
        check(s[1].complex, "segment 1 is a shape symbol");
        check(s[1].shapeFlag == 2, "shape flag");
        check(s[1].shapefile == "ltypeshp.shx", "shapefile preserved");
        closeTo(s[1].sCenter, 0.575, "first shape inserted at run end");
        closeTo(s[1].symScale, 0.5, "shape S");
        closeTo(s[2].s1 - s[2].s0, 0.15, "period dash = 0.15");
        check(s[3].complex, "second shape symbol");
        closeTo(s[3].sCenter, 1.475, "second shape inserted at run end");
        closeTo(s[4].s1, 1.8, "last ink reaches line end");
    }

    // --- ZIGZAG: shape run width = X span x S, insertion at run end --------
    // A, [ZIG,ltypeshp.shx,S=0.2,X=-0.2], -0.4,
    //    [ZIG,ltypeshp.shx,R=180,S=0.2,X=0.2], -0.4
    // ZIG X span = 2 shape units (real ltypeshp.shx bytes 12 1E 00) so each
    // shape run is 2 * 0.2 = 0.4 wide; P = 0.4*4 = 1.6; L=8; m=4; x=0.2.
    // After the leading x the layout continues from e2: the first symbol is
    // the rotated sh2 (X=+0.2), then full periods alternate e1 (X=-0.2) and
    // e3.  Run ends (definition vertices) at 1.0 / 1.8 / 2.6 / 3.4 / 4.2 /
    // 5.0 / 5.8 / 6.6 / 7.4; the X offsets are preserved for the draw stage.
    beginCase("ZIGZAG shape run width + run-end insertion, L=8");
    {
        std::vector<LineTypeDash> zig;
        LineTypeDash sh1;  // [ZIG,ltypeshp.shx,S=0.2,X=-0.2]
        sh1.length = 0.0;
        sh1.shapeFlag = 2;
        sh1.complexShapecode = 131;
        sh1.scale = 0.2;
        sh1.symWidth = 2.0;  // ZIG X span from the real ltypeshp.shx
        sh1.xOffset = -0.2;
        sh1.shapefile = "ltypeshp.shx";
        zig.push_back(sh1);
        LineTypeDash g1;
        g1.length = -0.4;
        zig.push_back(g1);
        LineTypeDash sh2;  // [ZIG,ltypeshp.shx,R=180,S=0.2,X=0.2]
        sh2.length = 0.0;
        sh2.shapeFlag = 2;
        sh2.complexShapecode = 131;
        sh2.scale = 0.2;
        sh2.symWidth = 2.0;
        sh2.rotation = PI;
        sh2.xOffset = 0.2;
        sh2.shapefile = "ltypeshp.shx";
        zig.push_back(sh2);
        LineTypeDash g2;
        g2.length = -0.4;
        zig.push_back(g2);

        auto s = computeInkSegments(8.0, zig, 1.0);
        std::printf("    L=8 scale=1.00 -> %s\n", fmt(s).c_str());
        checkInvariants(s, 8.0);
        check(s.size() == 11, "11 segments (6 ink + 5 shape symbols)");
        closeTo(s[0].s1, 0.2, "first ink = x = 0.2");
        check(s[1].complex, "first shape symbol");
        closeTo(s[1].s1 - s[1].s0, 0.4, "shape run = X span x S");
        closeTo(s[1].s0, 0.6, "first shape run start");
        closeTo(s[1].sCenter, 1.0, "first shape at run end (def vertex)");
        closeTo(s[1].symWidth, 0.4, "run width carried to the draw stage");
        closeTo(s[1].xOffset, 0.2, "leading symbol is the rotated sh2 (X=+0.2)");
        closeTo(s[1].symRotation, PI, "180 deg rotation preserved");
        closeTo(s[1].symScale, 0.2, "shape S");
        closeTo(s[2].sCenter, 1.8, "full-period e1 at run end");
        closeTo(s[2].xOffset, -0.2, "e1 X offset preserved");
        closeTo(s[3].s0, 2.2, "second shape run start");
        closeTo(s[3].sCenter, 2.6, "second shape at run end");
        closeTo(s[3].xOffset, 0.2, "rotated shape X offset preserved");
        closeTo(s[3].symRotation, PI, "180 deg rotation preserved");
        closeTo(s[4].sCenter, 3.4, "e1 of the next period");
        closeTo(s[5].sCenter, 4.2, "third shape at run end");
        closeTo(s[7].sCenter, 5.8, "fourth shape at run end");
        closeTo(s[9].sCenter, 7.4, "fifth shape at run end");
        closeTo(s[10].s1, 8.0, "last ink reaches line end");
    }

    std::printf("\n%d case(s), %d failure(s)\n", g_case, g_fail);
    return g_fail ? 1 : 0;
}
