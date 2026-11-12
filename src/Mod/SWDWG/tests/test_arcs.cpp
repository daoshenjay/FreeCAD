// Verify the ShapeArc records kept by DWGShapeFile match their sampled chord
// segments (start/end endpoints and run continuity) on real font files.
#include "DWGShapeFile.h"
#include <cstdio>
#include <cmath>
#include <string>
using namespace SWDWG;

static constexpr double kPi = 3.14159265358979323846;

static int checkFile(const char* path, const char* name)
{
    const ShapeFile sf = parseShapeFile(path);
    std::printf("== %s shapes=%zu\n", name, sf.size());
    int fails = 0, arcsTotal = 0, chordTotal = 0;
    for (const auto& kv : sf) {
        const ShapeDef& sd = kv.second;
        if (sd.arcs.empty()) {
            if (sd.segs.empty()) continue;
            for (const ShapeSeg& s : sd.segs) {
                if (s.arcChord) {
                    std::printf("  shape #%d has arcChord seg but no arcs\n",
                                sd.number);
                    ++fails;
                    break;
                }
            }
            continue;
        }
        // Ordered arcChord segments; consecutive arcs may be tangent (a run of
        // chords is NOT a reliable grouping), so allocate each arc its own
        // expected chord count and compare endpoint sets in either direction.
        std::vector<const ShapeSeg*> chords;
        for (const ShapeSeg& s : sd.segs) {
            if (s.arcChord) chords.push_back(&s);
        }
        size_t cur = 0;
        for (size_t k = 0; k < sd.arcs.size(); ++k) {
            const ShapeArc& a = sd.arcs[k];
            const int n = std::max(4, (int)std::ceil(a.sweepDeg / 5.0));
            if (cur + (size_t)n > chords.size()) {
                std::printf("  shape #%d arc[%zu] runs past chord list\n",
                            sd.number, k);
                ++fails;
                break;
            }
            const ShapeSeg& first = *chords[cur];
            const ShapeSeg& last = *chords[cur + (size_t)n - 1];
            cur += (size_t)n;
            ++arcsTotal;
            chordTotal += n;
            const double tol = 1e-6 * std::max(1.0, a.r);
            const double pa[2][2] = {
                {a.cx + a.r * std::cos(a.startDeg * kPi / 180.0),
                 a.cy + a.r * std::sin(a.startDeg * kPi / 180.0)},
                {a.cx + a.r * std::cos((a.startDeg + a.sweepDeg) * kPi / 180.0),
                 a.cy + a.r * std::sin((a.startDeg + a.sweepDeg) * kPi / 180.0)}};
            // Each chord endpoint must coincide with one of the two arc ends.
            auto onArc = [&](double x, double y) {
                for (int m = 0; m < 2; ++m) {
                    if (std::fabs(x - pa[m][0]) < tol
                        && std::fabs(y - pa[m][1]) < tol) {
                        return true;
                    }
                }
                return false;
            };
            if (!onArc(first.x0, first.y0) || !onArc(last.x1, last.y1)) {
                std::printf("  shape #%d arc[%zu] endpoints not on arc\n",
                            sd.number, k);
                ++fails;
            }
        }
        if (cur != chords.size()) {
            std::printf("  shape #%d unassigned arc chords (%zu of %zu)\n",
                        sd.number, chords.size() - cur, chords.size());
            ++fails;
        }
    }
    std::printf("  arcs=%d chordSegs=%d fails=%d\n", arcsTotal, chordTotal, fails);
    return fails;
}

int main()
{
    int f = 0;
    f += checkFile("C:/Program Files/Autodesk/AutoCAD 2016/Fonts/AAA.shx",
                   "AAA");
    f += checkFile("C:/Program Files/Autodesk/AutoCAD 2016/Fonts/fsdb.shx",
                   "FSDB");
    std::printf(f ? "ARC TEST FAILED (%d)\n" : "ARC TEST OK\n", f);
    return f ? 1 : 0;
}
