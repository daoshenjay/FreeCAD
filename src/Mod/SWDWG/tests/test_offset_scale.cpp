//! Cross-check X/Y offset semantics: scaled-by-LTSCALE (current) vs raw drawing
//! units (AutoCAD convention).  Print the insertion point of the first GAS
//! marker under both interpretations so we can pick the right one.
#include "../App/DWGLineCut.h"
#include "../App/DWGTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SWDWG;

static double insertionRaw(const LineTypeDash& d)
{
    // AutoCAD convention: X offset is in drawing units, not scaled by LTSCALE
    return d.xOffset;
}

static double insertionScaled(const LineTypeDash& d, double scale)
{
    // Current convention: scaled by LTSCALE
    return d.xOffset * scale;
}

int main()
{
    // User DWG GAS_LINE: 12.7 -5.08(complex GAS) -6.35
    std::vector<LineTypeDash> dashes(3);
    dashes[0].length = 12.7;
    dashes[1].length = -5.08;
    dashes[1].shapeFlag = 4;
    dashes[1].text = "GAS";
    dashes[1].scale = 2.54;
    dashes[1].xOffset = -2.54;
    dashes[1].yOffset = -1.27;
    dashes[2].length = -6.35;

    const double LTS = 5.0;
    const double P = 24.13 * LTS;        // world units

    // L = 1 period exactly (matches user's cutEdges=80 case)
    const double L = P + 0.001;        // nudge past L<P float-equal boundary
    auto segs = computeInkSegments(L, dashes, LTS);
    std::printf("L=%.4f (P+ε) -> %zu segs:\n", L, segs.size());
    for (const InkSeg& s : segs) {
        if (s.complex) {
            std::printf("  GAS sCenter=%.4f  ink.xOffset=%.4f\n",
                        s.sCenter, s.xOffset);
        }
        else if (!s.dot) {
            std::printf("  ink[%.4f, %.4f]\n", s.s0, s.s1);
        }
    }

    // Compute insertion point under both conventions.
    for (const InkSeg& s : segs) {
        if (!s.complex) continue;
        const double insScaled = s.sCenter + insertionScaled(dashes[1], LTS);
        const double insRaw = s.sCenter + insertionRaw(dashes[1]);
        std::printf("\nGAS insertion:\n");
        std::printf("  current (scaled): s=%.4f -> arc=%.4f (%.2f%% of P)\n",
                    s.sCenter + s.xOffset, insScaled, 100.0 * insScaled / P);
        std::printf("  raw (AutoCAD):    s=%.4f -> arc=%.4f (%.2f%% of P)\n",
                    s.sCenter + s.xOffset, insRaw, 100.0 * insRaw / P);
        // Gap midpoint between first and last ink
        std::printf("  gap midpoint: %.4f (%.2f%% of P) -- GAS should sit just left of this\n",
                    P * 0.5, 50.0);
    }
    return 0;
}