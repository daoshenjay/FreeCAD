//! Pure L2 linetype cutting algorithms (no OCCT / FreeCAD dependency).
//!
//! AutoCAD's 'A' alignment is reproduced here: every line must begin and end
//! with a pen-down element.  Autodesk's rule is that "the first dash begins
//! and ends the line": half of the first dash is drawn at the line start and
//! half at the line end, with full pattern periods in between.  When the line
//! length is not an exact multiple of the pattern period the two halves are
//! lengthened equally, keeping the pattern symmetric (e.g. ISO05W100 on a
//! 100 mm line renders 12.5, dots, 24, dots, 24, dots, 12.5 - not
//! 24, dots, 24, dots, 24, dots, 1).  Lines shorter than one pattern cycle
//! are rendered continuous (AutoCAD behaviour for too-short lines).
//! Dot-leading patterns (DOT, ISO07) fall back to a plain cyclic cut.
//!
//! The functions only consume the dependency-free DWGTypes.h model, so they
//! are compiled into the importer and into the standalone unit test
//! (tests/test_align.cpp) as identical code.

#pragma once

#include "DWGTypes.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace SWDWG
{

//! One ink (visible) segment of a cut line, in arc-length coordinates.
//!
//! A complex element (embedded text / shape, 18.10.6) replaces its ink run
//! with the symbol itself: the segment still occupies its layout width but
//! carries the symbol description so the caller can draw the text box / shape
//! marker at sCenter instead of a plain dash.
struct InkSeg
{
    double s0 = 0.0;   //!< arc length from the edge start
    double s1 = 0.0;
    bool dot = false;  //!< zero-length marker (dot element)
    // Complex element ---------------------------------------------------
    bool complex = false;    //!< this run is an embedded text / shape symbol
    double sCenter = 0.0;    //!< symbol insertion point (arc length)
    int shapeFlag = 0;       //!< 2 = shape, 4 = text (mirrors LineTypeDash)
    int complexShapecode = 0;  //!< shape number (shx file), 0 for text
    std::string text;        //!< embedded text run (e.g. "GAS")
    std::string shapefile;   //!< .shx file holding the shape (e.g. "ltypeshp.shx")
    double symScale = 1.0;   //!< S: symbol / text height in drawing units
    double symRotation = 0.0;  //!< extra rotation about the line (radians)
    double symWidth = 0.0;   //!< layout width reserved by this complex dash
    double xOffset = 0.0;    //!< offset along the line direction
    double yOffset = 0.0;    //!< offset along the (counter-clockwise) normal
    double widthFactor = 1.0;  //!< STYLE width factor (text horizontal stretch)
};

//! Effective layout width of one dash element (drawing units, before the
//! LTSCALE factor).  Plain elements use |length|; a zero-length complex
//! element (e.g. "[GAS,...]" or "[TRACK1,...]" in the LINETYPE text) carries
//! no explicit length, so AutoCAD sizes it from the symbol itself: the text
//! width for embedded text, and the shape's bounding-box X span times S for
//! embedded shapes (the run a shape occupies along the line equals its width
//! in the .shx scaled by S).  When the shape file could not be resolved the
//! importer leaves symWidth == 0 and we fall back to the scale S.
inline double dashLayoutWidth(const LineTypeDash& d)
{
    double w = std::fabs(d.length);
    if (w < 1e-12 && d.shapeFlag != 0) {
        const double h = d.scale > 1e-9 ? d.scale : 1.0;
        if (d.shapeFlag == 2 && d.symWidth > 0.0) {
            w = d.symWidth * h;  // shape X span x S
        }
        else {
            const double wf = (d.widthFactor > 0.0) ? d.widthFactor : 1.0;
            w = (d.shapeFlag == 4)
                ? std::max(0.6 * h * (double)d.text.size(), h) * wf
                : h;
        }
    }
    return w;
}

//! Fill the complex-element fields of an ink segment from a dash (used by the
//! cutting routines; keeps the symbol description next to its phase).
//!
//! AutoCAD stamps embedded shapes/text at the END of the layout run the
//! complex element occupies - the "linetype definition vertex" (the point
//! where the element finishes and the next one begins) - then applies the
//! linetype-definition X/Y offsets from that point: the Customization Guide
//! states the X shift is "computed from the end of the linetype definition
//! vertex".  The symbol scale S, X and Y offsets are all multiplied by the
//! effective linetype scale factor.
inline void setComplex(InkSeg& ink, const LineTypeDash& d, double pos,
                       double width, double scale)
{
    ink.complex = true;
    ink.sCenter = pos + width;         // run end = definition vertex
    ink.shapeFlag = d.shapeFlag;
    ink.complexShapecode = d.complexShapecode;
    ink.text = d.text;
    ink.shapefile = d.shapefile;
    ink.symScale = d.scale * scale;    // S * LTSCALE
    ink.symRotation = d.rotation;
    ink.symWidth = width;
    ink.widthFactor = d.widthFactor;
    // X / Y offsets are part of the linetype pattern, so like every other
    // pattern value they are in drawing units and scale with LTSCALE.
    ink.xOffset = d.xOffset * scale;
    ink.yOffset = d.yOffset * scale;
}

//! Build a shape-only marker (no ink segment) for a complex dash whose
//! length is negative: AutoCAD stores such a dash as "leading gap, then
//! shape at the run end", and the line is still broken (the shape's visual
//! extent does not consume layout width).  Used by both computeInkSegments
//! and computePlainCut when they encounter a gap-shaped complex element.
inline InkSeg makeShapeOnlyMarker(const LineTypeDash& d, double pos,
                                  double width, double scale)
{
    InkSeg marker;
    marker.s0 = pos;
    marker.s1 = pos;                    // zero-length run: no ink
    marker.dot = false;
    setComplex(marker, d, pos, width, scale);
    return marker;
}

//! Plain cyclic cut (fallback for dot-leading patterns such as DOT / ISO07):
//! full periods from the line start plus a truncated remainder.  The line
//! still ends on ink by lengthening the preceding dash when the remainder
//! exhausts inside a gap.
inline std::vector<InkSeg> computePlainCut(double L,
                                           const std::vector<LineTypeDash>& dashes,
                                           const std::vector<double>& w,
                                           double P,
                                           double scale)
{
    std::vector<InkSeg> out;
    const long long nFull = static_cast<long long>(std::floor(L / P));
    const double R = L - (double)nFull * P;

    // One pattern period (phase 0..P).
    std::vector<InkSeg> base;
    {
        double pos = 0.0;
        for (std::size_t i = 0; i < dashes.size(); ++i) {
            const LineTypeDash& d = dashes[i];
            if (d.length > 0.0) {
                // Positive-length ink dash. May also carry a shape stamped at
                // the run end (rare: leading ink plus symbol).
                base.push_back({pos, pos + w[i], false});
                if (d.shapeFlag != 0) {
                    setComplex(base.back(), d, pos, w[i], scale);
                }
                pos += w[i];
            }
            else if (d.length == 0.0) {
                if (d.shapeFlag != 0) {
                    // Zero-length complex dash: layout width = shape size
                    // (dashLayoutWidth fallback).  Always ink + stamp.
                    base.push_back({pos, pos + w[i], false});
                    setComplex(base.back(), d, pos, w[i], scale);
                    pos += w[i];
                }
                else {
                    base.push_back({pos, pos, true});  // dot: no phase advance
                }
            }
            else {  // length < 0: gap
                pos += w[i];
                if (d.shapeFlag != 0) {
                    // Negative-length complex dash: "leading gap of |length|
                    // then shape at the run end".  No ink consumed - the
                    // shape is a marker the caller draws at the run end.
                    base.push_back(makeShapeOnlyMarker(d, pos - w[i], w[i], scale));
                }
            }
        }
    }
    // Full periods.
    for (long long k = 0; k < nFull; ++k) {
        for (const InkSeg& s : base) {
            InkSeg t = s;
            t.s0 += (double)k * P;
            t.s1 += (double)k * P;
            // Complex elements carry their phase position in sCenter; it must
            // advance with the period too or every period after the first
            // stamps its shape at the first period's location.
            if (t.complex) {
                t.sCenter += (double)k * P;
            }
            out.push_back(t);
        }
    }
    // Line ends exactly on a period boundary: if the pattern tail is a gap,
    // pull the last ink up to the line end so the line still ends on ink.
    if (R <= 1e-9 && !base.empty() && !base.back().dot
        && base.back().s1 < P - 1e-9 && !out.empty()) {
        out.back().s1 = L;
    }
    // Remainder: line must end on ink.
    if (R > 1e-9) {
        double pos = 0.0;
        std::size_t lastOnIdx = std::size_t(-1);
        const double base0 = (double)nFull * P;
        for (std::size_t i = 0; i < dashes.size(); ++i) {
            const LineTypeDash& d = dashes[i];
            if (d.length > 0.0) {
                double avail = R - pos;
                if (avail <= 1e-9) {
                    break;
                }
                double len = std::min(w[i], avail);
                out.push_back({base0 + pos, base0 + pos + len, false});
                if (d.shapeFlag != 0) {
                    // Symbol too squeezed for the remaining space: AutoCAD
                    // omits it; we keep the (possibly truncated) marker so
                    // the phase is not lost.
                    setComplex(out.back(), d, base0 + pos, len, scale);
                }
                lastOnIdx = out.size() - 1;
                pos += len;
            }
            else if (d.length == 0.0) {
                if (d.shapeFlag != 0) {
                    // Zero-length complex element: reserves layout width but
                    // draws no ink, so it contributes only its marker (see the
                    // 1161 note in the A-alignment branch above).  Kept in sync
                    // with that branch.
                    double avail = R - pos;
                    if (avail <= 1e-9) {
                        break;
                    }
                    double len = std::min(w[i], avail);
                    out.push_back(makeShapeOnlyMarker(d, base0 + pos, len,
                                                      scale));
                    pos += len;
                }
                else if (R - pos > -1e-9) {
                    out.push_back({base0 + pos, base0 + pos, true});
                }
            }
            else {  // length < 0: gap
                double avail = R - pos;
                if (avail <= 1e-9) {
                    break;
                }
                if (w[i] >= avail - 1e-9) {
                    // Remainder exhausts inside this gap: lengthen the
                    // preceding dash up to the line end ('A' alignment).
                    if (lastOnIdx != std::size_t(-1)) {
                        out.resize(lastOnIdx + 1);  // drop trailing dots
                        out[lastOnIdx].s1 = base0 + R;
                    }
                    pos = R;
                    break;
                }
                pos += w[i];
                if (d.shapeFlag != 0) {
                    // Stamp the shape at the end of this gap; no ink segment.
                    if (base0 + pos <= L + 1e-9) {
                        out.push_back(makeShapeOnlyMarker(d, base0 + pos - w[i],
                                                         w[i], scale));
                    }
                }
            }
        }
    }
    return out;
}

//! Compute the visible arc-length intervals of a line of total length L laid
//! out with dashes x scale, following AutoCAD 'A' alignment.
//!
//! AutoCAD's 'A' alignment (see Autodesk "About Simple Custom Linetypes"):
//! the pattern must begin and end with a pen-down element, and "the first
//! dash begins and ends the line" - half of the first dash is drawn at the
//! line start and half at the line end, with full pattern periods in
//! between.  When the line length is not an exact multiple of the period the
//! two halves are lengthened equally to absorb the excess, so the pattern is
//! symmetric.  Lines shorter than one cycle are drawn continuous.
//!
//! This is why, e.g. for ACAD_ISO05W100 (24,-3,0,-3,0,-3, period 33) on a
//! 100 mm line AutoCAD shows  12.5, dots, 24, dots, 24, dots, 12.5  (first
//! dash 24 split as (24+1)/2 at both ends) instead of 24, dots, 24, dots,
//! 24, dots, 1.
inline std::vector<InkSeg> computeInkSegments(double L,
                                              const std::vector<LineTypeDash>& dashes,
                                              double scale,
                                              double patternLen = 0.0)
{
    std::vector<InkSeg> out;
    if (L <= 1e-9) {
        return out;
    }
    // Element widths in world units and pattern period.  A complex element
    // (shape or embedded text) is not a plain dash: the symbol is stamped at
    // the end of the run it reserves, the linetype definition vertex
    // (dashLayoutWidth gives that width, using the symbol size when the
    // definition carries no explicit length).
    std::vector<double> w;
    w.reserve(dashes.size());
    double P = 0.0;
    bool anyInk = false;
    for (const LineTypeDash& d : dashes) {
        double wi = dashLayoutWidth(d) * scale;
        w.push_back(wi);
        P += wi;
        // Ink candidates: any dash whose length is non-negative (positive-length
        // ink dash or zero-length dot / complex).  Negative length means
        // gap, even when it carries a complex shape: a negative-length
        // complex dash stores the leading gap width, the shape is stamped
        // at the run end, and the pattern consumes no ink in the gap.
        if (d.length >= 0.0) {
            anyInk = true;
        }
    }
    // Adopt the DWG-stored pattern length when it is close to the raw dash
    // sum.  AutoCAD sizes the pattern period from that stored field, and it
    // can differ from the |dash| sum by a few ulps (e.g. BATTING 10.1625 vs
    // 10.16254).  Using it keeps a line of exactly N x pattern_len a whole
    // number of periods - otherwise float noise makes floor(L/P) come out
    // one period short and every complex element is misplaced (a 2-period
    // ZIGZAG line was the symptom).
    if (patternLen > 0.0 && P > 1e-12) {
        const double pWorld = patternLen * scale;
        // AutoCAD sizes the period from the stored pattern_len even when it
        // differs from the raw dash sum by more than rounding (e.g. GAS_LINE
        // stores 22.86 while 12.7+5.08+6.35 = 24.13).  Reject only clearly
        // inconsistent values (off by more than 50%).
        if (pWorld >= 0.5 * P && pWorld <= 2.0 * P) {
            const double k = pWorld / P;
            for (double& wi : w) {
                wi *= k;
            }
            P = pWorld;
        }
    }
    // Degenerate pattern -> continuous line.
    if (P <= 1e-12 || !anyInk) {
        out.push_back({0.0, L, false});
        return out;
    }
    // Snap a line length that is (numerically) an exact multiple of the
    // pattern period to that value.  Without this, float noise just below
    // N*P makes floor(L/P) come out one period too small, so the A-alignment
    // end halves grow from e1/2 to (P+e1)/2 and every complex element lands
    // at the wrong phase (ZIGZAG / BATTING on a line of exactly N periods).
    {
        const long long nPer = static_cast<long long>(std::floor(L / P + 1e-9));
        if (nPer >= 1) {
            const double snap = (double)nPer * P;
            if (std::fabs(L - snap) < 1e-9) {
                L = snap;
            }
        }
    }
    // Shorter than one cycle -> continuous (AutoCAD too-short behaviour).
    if (L < P) {
        out.push_back({0.0, L, false});
        return out;
    }

    const double e1 = w[0];
    if (e1 <= 1e-9) {
        // Dot-leading pattern (DOT, ISO07, ...): the first element has no
        // length, so "half a dash at each end" degenerates; fall back to the
        // plain cyclic cut (equidistant dots, end truncated inside a gap).
        return computePlainCut(L, dashes, w, P, scale);
    }

    // ---- AutoCAD 'A' alignment ----
    // Layout:  x | e2..en | (e1..en) x m | x   with  L = 2*x + (m+1)*P - e1,
    // i.e. the line begins and ends with the first dash (length x visible at
    // each end) and (m+1) full periods sit in between.
    const std::size_t n = dashes.size();
    // Full pattern periods between the two end halves.
    long long m = static_cast<long long>(std::floor(L / P)) - 1;
    if (m < 0) {
        m = 0;
    }
    // End half = (L - (m+1)*P + e1) / 2.  It is *not* capped at e1: AutoCAD
    // keeps m = floor(L/P)-1 full periods and simply lets the first dash
    // grow.  E.g. CENTER (31.75,-6.35,6.35,-6.35) on a 100 mm line renders
    //   40.475 | 6.35 | 40.475   (m=0, end dashes longer than e1=31.75),
    // and ISO06W100 (24,-3,0,-3,0,-3,0,-3) renders
    //   26 | 3 dots | 24 | 3 dots | 26   (m=1, end dashes 26 > 24).
    // x is always >= 0 because (m+1)*P <= L holds whenever L >= P.
    double x = (L - (double)(m + 1) * P + e1) * 0.5;

    auto emit = [&](std::size_t i, double& pos) {
        const LineTypeDash& d = dashes[i];
        if (d.length > 0.0) {
            // Positive-length ink dash. May also carry a shape stamped at the
            // run end (rare: leading ink plus symbol).
            out.push_back({pos, pos + w[i], false});
            if (d.shapeFlag != 0) {
                setComplex(out.back(), d, pos, w[i], scale);
            }
            pos += w[i];
        }
        else if (d.length == 0.0) {
            if (d.shapeFlag != 0) {
                // Zero-length complex element: it reserves a layout run - the
                // shape's bounding-box width x S, or the text width, resolved
                // by dashLayoutWidth() - but AutoCAD draws NO ink for it, only
                // the symbol.  Drawing the run as ink put a short bar next to
                // every marker: the DXT survey's linetype 1161 is exactly a
                // zero-length shape element (AAA.shx code 149, S = 0.15)
                // followed by a -1.6 gap, and every 1.6-unit period came out
                // with a 137 mm stray bar (symptom: "1161 produces extra short
                // lines", visible as a row of marks).  Treated like the
                // negative-length complex dash below: marker only, no ink.  The
                // phase still advances by the reserved width - that part IS
                // AutoCAD's layout rule.
                out.push_back(makeShapeOnlyMarker(d, pos, w[i], scale));
                pos += w[i];
            }
            else {
                out.push_back({pos, pos, true});  // dot: no phase advance
            }
        }
        else {  // length < 0: gap
            pos += w[i];
            if (d.shapeFlag != 0) {
                // Negative-length complex dash: gap of |length|, then shape
                // stamped at the run end.  No ink segment - the marker
                // carries just the symbol description for the renderer.
                out.push_back(makeShapeOnlyMarker(d, pos - w[i], w[i], scale));
            }
        }
    };

    // Layout:  x | e2..en | (e1..en) x m | x
    double pos = 0.0;
    if (x > 1e-12) {
        // A-alignment splits the first dash in half at both ends; when that
        // dash is a complex element the symbol stamps at the split point
        // (its definition vertex).  TRACKS "3.81,[TRACK1],3.81" resolves to
        // a 2-dash pattern [shape(3.81), plain(3.81)]; on a 100-unit line
        // the first TRACK1 must sit at x = 2.375, not at the end of the
        // first full run (9.995) - that one-period shift was the user's
        // "first horizontal segment length differs" report.
        //
        // The half run is ink ONLY when the first element really draws ink.
        // A zero-length complex element (a shape / text marker) reserves
        // layout width but no pen-down run, so drawing that half as ink put a
        // ~650 mm bar at both ends of every line using the DXT survey's
        // linetype 1161 (shape AAA.shx 149, S = 0.15, length 0, then a -1.6
        // gap; 395 such bars in the layer group).  Such an element gets its
        // marker only - the split point still has to be kept, it is where
        // AutoCAD places the symbol.
        if (dashes[0].length > 0.0) {
            out.push_back({0.0, x, false});
            if (dashes[0].shapeFlag != 0) {
                setComplex(out.back(), dashes[0], 0.0, x, scale);
            }
        }
        else if (dashes[0].shapeFlag != 0) {
            out.push_back(makeShapeOnlyMarker(dashes[0], 0.0, x, scale));
        }
    }
    pos = x;
    for (std::size_t i = 1; i < n; ++i) {
        emit(i, pos);
    }
    for (long long k = 0; k < m; ++k) {
        for (std::size_t i = 0; i < n; ++i) {
            emit(i, pos);
        }
    }
    if (x > 1e-12 && dashes[0].length > 0.0) {
        // End half: plain ink only.  AutoCAD draws the last period's tail as
        // e2(3.81) + x(2.375) without stamping the symbol again at the very
        // end of the line (TRACKS shows the last TRACK1 at 93.815, then a
        // 6.185 tail with no sleeper).
        //
        // Only when the first element draws ink: with a zero-length complex
        // element there is no first dash, so there is no tail to draw either.
        // That tail was the last of the "extra short lines": one bar of
        // 200..740 mm at the END of every line using the DXT survey's linetype
        // 1161 (199 of them in the layer group; the matching bar at the START
        // is handled above).
        out.push_back({pos, pos + x, false});
    }
    pos += x;

    return out;
}

}  // namespace SWDWG
