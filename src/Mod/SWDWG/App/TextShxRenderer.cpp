//! Classic AutoCAD .shx text rendering service - implementation.

// Note: this file deliberately does NOT include PreCompiled.h so the unit
// test can compile it standalone (it has no FreeCAD / OCCT / Xerces deps).

#include "TextShxRenderer.h"

#include "FontResolver.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

using namespace SWDWG;

namespace
{

//! Lowercase and strip a font / shape file path down to its base name.
std::string fontBaseName(const std::string& file)
{
    std::string name = file;
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

//! Bounds of a glyph outline in shape units.
struct GlyphBox
{
    double xmin = 0.0;
    double xmax = 0.0;
    double ymin = 0.0;
    double ymax = 0.0;
    bool valid = false;
};

GlyphBox glyphBox(const ShapeDef& sd)
{
    GlyphBox b;
    for (const ShapeSeg& s : sd.segs) {
        if (!b.valid) {
            b.xmin = std::min(s.x0, s.x1);
            b.xmax = std::max(s.x0, s.x1);
            b.ymin = std::min(s.y0, s.y1);
            b.ymax = std::max(s.y0, s.y1);
            b.valid = true;
        }
        else {
            b.xmin = std::min(b.xmin, std::min(s.x0, s.x1));
            b.xmax = std::max(b.xmax, std::max(s.x0, s.x1));
            b.ymin = std::min(b.ymin, std::min(s.y0, s.y1));
            b.ymax = std::max(b.ymax, std::max(s.y0, s.y1));
        }
    }
    return b;
}

//! Cap-height reference of the classic AutoCAD single-line fonts
//! (txt.shx / romans.shx).  Measured on the shipped txt.shx: the cap top of
//! 'H'/'A' is at shape y = 24 and the baseline at y = 3, so the cap height
//! is 21 shape units.  The DWG text height maps to this cap height
//! (scale = height / 21).  Descenders go down to about y = -4.
constexpr double kBaselineY = 3.0;  //!< baseline in shape units (txt.shx)
constexpr double kCapUnits = 21.0;  //!< cap height in shape units

//! Full-width cell of a CJK big font relative to the text height.
//! AutoCAD advances a big-font run by one full-width cell per character,
//! and that cell is marginally WIDER than the nominal text height: measured
//! on "测试中心线" at h=2.5 the per-glyph centre spacing is ~2.5-2.6
//! (average ~2.525), while a pure h-wide cell renders FSDB text a little
//! tight in FreeCAD.  The glyph strokes themselves keep their size; only the
//! advance cell grows by this factor.
constexpr double kBigCellScale = 1.0;   //!< CJK cell = height * this

}  // namespace

namespace SWDWG
{

bool isShxFont(const std::string& fontFile, const std::string& bigFontFile,
               const std::vector<std::string>& extraShxPaths)
{
    const std::string fBase = fontBaseName(fontFile);
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    };
    const bool fShx = lower(fBase).size() >= 4
        && lower(fBase).compare(lower(fBase).size() - 4, 4, ".shx") == 0;
    const bool bfShx = !bigFontFile.empty()
        && lower(fontBaseName(bigFontFile)).size() >= 4
        && lower(fontBaseName(bigFontFile)).compare(
               lower(fontBaseName(bigFontFile)).size() - 4, 4,
               ".shx") == 0;
    if (!fShx && !bfShx) {
        return false;
    }
    if (fShx && !findShapeFile(fontFile, extraShxPaths).empty()) {
        return true;
    }
    if (bfShx && !findShapeFile(bigFontFile, extraShxPaths).empty()) {
        return true;
    }
    return false;
}

std::vector<int> utf8ToFontCodes(const std::string& utf8, int codepage)
{
    std::vector<int> codes;
    if (utf8.empty()) {
        return codes;
    }

    // A CJK drawing usually declares its codepage in the DWG header (936 =
    // GBK).  When it does not (codepage 0) or declares UTF-8, the .shx big
    // fonts still key their glyphs by the GBK bytes, so fall back to 936.
    int cp = codepage;
    if (cp <= 0 || cp == 65001) {
        cp = 936;
    }
    // Pure ASCII text is identical in every single-byte codepage: keep it.
    bool hasHigh = false;
    for (unsigned char c : utf8) {
        if (c >= 0x80) {
            hasHigh = true;
            break;
        }
    }
    if (!hasHigh) {
        for (unsigned char c : utf8) {
            codes.push_back(static_cast<int>(c));
        }
        return codes;
    }

#ifdef _WIN32
    // Convert the whole UTF-8 run to a wide string, then encode with the
    // effective codepage; the bytes are the shape numbers AutoCAD uses.
    const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         utf8.c_str(), (int)utf8.size(),
                                         nullptr, 0);
    if (wlen > 0) {
        std::wstring wstr(wlen, L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                utf8.c_str(), (int)utf8.size(), wstr.data(),
                                wlen)
            == wlen) {
            int blen = WideCharToMultiByte(cp, 0, wstr.c_str(), wlen, nullptr,
                                           0, nullptr, nullptr);
            if (blen > 0) {
                std::string bytes(blen, '\0');
                WideCharToMultiByte(cp, 0, wstr.c_str(), wlen, bytes.data(),
                                    blen, nullptr, nullptr);
                // Map bytes to shape numbers: ASCII single byte, DBCS
                // lead+trail as one combined code.
                for (size_t i = 0; i < bytes.size(); ++i) {
                    const unsigned char c =
                        static_cast<unsigned char>(bytes[i]);
                    if (c < 0x80) {
                        codes.push_back(c);
                        continue;
                    }
#ifdef _WIN32
                    if (IsDBCSLeadByteEx(cp, c) && i + 1 < bytes.size()) {
                        codes.push_back((c << 8)
                                        | static_cast<unsigned char>(
                                            bytes[i + 1]));
                        ++i;
                        continue;
                    }
#endif
                    codes.push_back(c);
                }
                return codes;
            }
        }
    }
#endif

    // Last-resort fallback: emit each UTF-8 code point (only correct for
    // ASCII, which never reaches here).
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            codes.push_back(c);
            ++i;
            continue;
        }
        size_t extra = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        }
        else {
            ++i;
            continue;
        }
        if (i + extra >= utf8.size()) {
            break;
        }
        unsigned int ucp = c & (0xFF >> (extra + 1));
        for (size_t k = 1; k <= extra; ++k) {
            ucp = (ucp << 6)
                | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        }
        codes.push_back(static_cast<int>(ucp));
        i += extra + 1;
    }
    return codes;
}

ShxLineRender renderShxLine(const std::string& fontFile,
                            const std::string& bigFontFile,
                            int codepage,
                            const std::string& utf8Text,
                            double height,
                            double widthFactor,
                            const std::vector<std::string>& extraShxPaths)
{
    ShxLineRender out;
    if (utf8Text.empty() || height <= 0.0) {
        return out;
    }
    if (fontFile.empty() && bigFontFile.empty()) {
        return out;
    }

    // Locate the shape files.  Classic fonts (txt.shx) are single-byte;
    // big fonts (fsdb.shx / gbcbig.shx) store double-byte characters.
    const std::string basePath = fontFile.empty()
        ? std::string()
        : findShapeFile(fontFile, extraShxPaths);
    const std::string bigPath = bigFontFile.empty()
        ? std::string()
        : findShapeFile(bigFontFile, extraShxPaths);

    ShapeFileCache& cache = ShapeFileCache::instance();
    std::shared_ptr<const ShapeFile> base;
    std::shared_ptr<const ShapeFile> big;
    if (!basePath.empty()) {
        base = cache.get(basePath);
    }
    if (!bigPath.empty()) {
        big = cache.get(bigPath);
    }
    if (!base && !big) {
        return out;
    }

    const std::vector<int> codes = utf8ToFontCodes(utf8Text, codepage);
    if (codes.empty()) {
        return out;
    }

    // AutoCAD single-line fonts: the DWG text height maps to the CAP height
    // (measured 21 shape units on txt.shx, baseline at shape y = 3).  Big
    // fonts use their own coordinate grid; each CJK glyph there occupies a
    // full-width cell whose vertical span varies per file (gbcbig ~90,
    // FSDB ~40).  We normalise every big-font glyph to the text height so a
    // Chinese line renders at the nominal size regardless of the grid.
    const double asciiScale = height / kCapUnits;
    const double asciiBaseOff = kBaselineY * asciiScale;

    // First pass over the used codes: find the big-font reference cell height.
    // Big-font (CJK) glyphs of one file all use the same cell, so the first
    // glyph's vertical span is taken as the cell height.  Every glyph is then
    // drawn bottom-aligned to the baseline and centred horizontally inside a
    // full-width cell - AutoCAD lays CJK text out as equally spaced square
    // characters, regardless of where each source font placed the strokes.
    double bigSpan = 0.0;  // shape units of one big-font cell (y)
    for (int code : codes) {
        if (code < 0x80 || !big) {
            continue;
        }
        const auto it = big->find(code);
        if (it == big->end()) {
            continue;
        }
        const GlyphBox b = glyphBox(it->second);
        if (b.valid) {
            bigSpan = b.ymax - b.ymin;
        }
        break;  // CJK glyphs in one file share the same cell height
    }

    double penX = 0.0;  // model units, x of the current glyph origin
    double maxAsc = 0.0;
    double maxDesc = 0.0;

    for (int code : codes) {
        // Look up the shape: ASCII / single byte in the base font, anything
        // >= 0x80 in the big font when one is present.
        const ShapeDef* sd = nullptr;
        const char which = (code >= 0x80 && big) ? 'B' : 'F';
        if (which == 'B') {
            const auto it = big->find(code);
            if (it != big->end()) {
                sd = &it->second;
            }
        }
        else if (base) {
            const auto it = base->find(code);
            if (it != base->end()) {
                sd = &it->second;
            }
        }
        // Characters not present in their font are skipped like a missing
        // glyph (their advance becomes 0; AutoCAD shows the .notdef box).
        if (!sd) {
            continue;
        }

        // Per-engine scale and baseline translation.
        const bool isBig = (which == 'B');
        const double scale = isBig ? (bigSpan > 1e-9 ? height / bigSpan
                                                     : height / kCapUnits)
                                   : asciiScale;
        const GlyphBox gb = glyphBox(*sd);

        // Advance.  Two completely different rules:
        //
        // - ASCII single-byte fonts (txt.shx, and the ASCII part of
        //   fsdb_e.shx): every glyph definition finishes with a pen-up move
        //   to the next character origin, so penEndX is the glyph's advance
        //   and AutoCAD advances by that amount between glyphs.
        //
        // - CJK big fonts (fsdb.shx / gbcbig.shx): the glyph definitions do
        //   NOT encode the advance.  Measured on fsdb.shx, penEndX varies
        //   wildly per glyph (22..60 shape units) and penEndY is non-zero,
        //   i.e. penEnd is just where the decoding pen happened to rest - it
        //   is not the inter-character spacing.  AutoCAD lays a CJK run out
        //   as fixed FULL-WIDTH cells: each character advances by one cell of
        //   ~1.03 x the text height (times the width factor) - AutoCAD's
        //   measured per-glyph centre spacing for "测试中心线" at h=2.5 is
        //   ~2.5-2.6 while its per-glyph stroke widths still vary 1.8..2.4.
        //   Using penEndX here produced uneven gaps (3.2 / 2.4 / 1.9 / 3.1)
        //   that never match AutoCAD.
        double advModel;
        const double glyphOrigin = penX;
        if (isBig) {
            advModel = height * widthFactor * kBigCellScale;
        }
        else {
            double advUnits = sd->penEndX;
            if (gb.valid && advUnits < (gb.xmax - gb.xmin)) {
                advUnits = gb.xmax - gb.xmin;
            }
            if (advUnits < 1e-9) {
                advUnits = 2.0;
            }
            advModel = advUnits * widthFactor * scale;
        }
        penX += advModel;

        if (sd->segs.empty()) {
            continue;  // blank / whitespace glyph: no outline
        }

        // Big-font CJK glyphs: centre the strokes inside their square cell.
        // The cell is `advModel` = one full-width cell (see advance logic
        // above).  FSDB and friends place the pen of each glyph at a slightly
        // different spot relative to the cell, so using raw source
        // coordinates would make both the inter-character gaps and the
        // vertical centres vary from glyph to glyph (some stick left, some
        // padded right).  Centring every glyph in its fixed cell both
        // horizontally and vertically yields the evenly spaced, on-axis
        // characters AutoCAD shows.
        double xOff = 0.0;    // model units added to every x
        double baseOff = asciiBaseOff;
        if (isBig) {
            // Baseline model of AutoCAD big fonts.  The TEXT anchor is the
            // BASELINE of the run (a Justify=center/baseline LIST reports
            // the centre point ON the baseline; for fsdb.shx at h=3.5 the
            // glyphs then sit ~h/2 above that point).  So a CJK cell whose
            // strokes span one text height is drawn from y=0 (baseline) up
            // to y=+h; its visual centre lies at +h/2, NOT on the anchor.
            // Measured: the stroke-box centre of every glyph of a run lands
            // on y = baseline + h/2, whichever shape box height the glyph
            // happens to have.  Translate each glyph so its OWN box centre
            // lands on +h/2 (instead of the previous "centre on y=0", which
            // dropped the whole run by h/2 below the AutoCAD position -
            // verified against AutoCAD by adding +h/2 to the anchor).
            baseOff = gb.valid
                ? ((gb.ymin + gb.ymax) * 0.5 * scale - height * 0.5)
                : 0.0;
            if (gb.valid && bigSpan > 1e-9) {
                const double glyphW =
                    (gb.xmax - gb.xmin) * widthFactor * scale;
                const double cellW = advModel;
                // Centre the glyph in its cell.  gb.xmin MUST be scaled by
                // widthFactor like the stroke coordinates below; forgetting
                // it shifted every asymmetric glyph (xmin != 0) sideways by
                // xmin*scale*(widthFactor-1), so with wf=0.75 characters
                // drifted out of their cells and invaded their neighbours.
                xOff = (cellW - glyphW) * 0.5 - gb.xmin * widthFactor * scale;
            }
        }

        std::vector<ShapeSeg> glyph;
        glyph.reserve(sd->segs.size());
        for (const ShapeSeg& s : sd->segs) {
            ShapeSeg t;
            t.x0 = glyphOrigin + xOff + s.x0 * widthFactor * scale;
            t.y0 = s.y0 * scale - baseOff;
            t.x1 = glyphOrigin + xOff + s.x1 * widthFactor * scale;
            t.y1 = s.y1 * scale - baseOff;
            glyph.push_back(t);
            maxAsc = std::max(maxAsc, std::max(t.y0, t.y1));
            maxDesc = std::min(maxDesc, std::min(t.y0, t.y1));
        }
        if (!glyph.empty()) {
            out.glyphs.push_back(std::move(glyph));
            out.ok = true;
        }
    }

    out.width = penX;
    out.ascender = maxAsc;
    out.descender = maxDesc;
    return out;
}

}  // namespace SWDWG
