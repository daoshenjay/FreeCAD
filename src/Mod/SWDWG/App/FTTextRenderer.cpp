// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FTTextRenderer.h"
#include "PathUtil.h"

#ifdef FCUseFreeType

# include <ft2build.h>
# include FT_FREETYPE_H
# include FT_OUTLINE_H

# include <algorithm>
# include <cctype>
# include <cstdint>
# include <filesystem>
# include <fstream>
# include <map>
# include <mutex>
# include <string>
# include <vector>

namespace SWDWG
{

namespace
{

// ---------------------------------------------------------------------------
// FreeType library singleton
// ---------------------------------------------------------------------------
FT_Library& ftLib()
{
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        FT_Init_FreeType(&l);
        return l;
    }();
    return lib;
}

//! UTF-8 decode: return the next code point and its byte length.
std::uint32_t utf8Next(const std::string& s, std::size_t& i)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    std::size_t extra = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
    }
    else if ((c & 0xF0) == 0xE0) {
        extra = 2;
    }
    else if ((c & 0xF8) == 0xF0) {
        extra = 3;
    }
    std::uint32_t cp = c & (0xFF >> (extra + 1));
    for (std::size_t k = 1; k <= extra && i + k < s.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += extra + 1;
    return cp;
}

// ---------------------------------------------------------------------------
// De Casteljau sampling of quadratic / cubic Bezier segments
// ---------------------------------------------------------------------------
inline void sampleQuadratic(double x0, double y0, double x1, double y1,
                            double x2, double y2, int steps,
                            std::vector<std::pair<double, double>>& pts)
{
    for (int k = 1; k <= steps; ++k) {
        const double t = static_cast<double>(k) / steps;
        const double u = 1.0 - t;
        const double x = u * u * x0 + 2.0 * u * t * x1 + t * t * x2;
        const double y = u * u * y0 + 2.0 * u * t * y1 + t * t * y2;
        pts.emplace_back(x, y);
    }
}

inline void sampleCubic(double x0, double y0, double x1, double y1,
                        double x2, double y2, double x3, double y3, int steps,
                        std::vector<std::pair<double, double>>& pts)
{
    for (int k = 1; k <= steps; ++k) {
        const double t = static_cast<double>(k) / steps;
        const double u = 1.0 - t;
        const double x = u * u * u * x0 + 3.0 * u * u * t * x1
            + 3.0 * u * t * t * x2 + t * t * t * x3;
        const double y = u * u * u * y0 + 3.0 * u * u * t * y1
            + 3.0 * u * t * t * y2 + t * t * t * y3;
        pts.emplace_back(x, y);
    }
}

//! FreeType outline-decomposition context: accumulates one polyline per
//! contour in unscaled 26.6 font units (divided by 64.0 later).
struct OutlineCtx
{
    //! One contour being built (points in font units, y-up).
    std::vector<std::pair<double, double>> cur;
    //! Finished contours (as point rings).
    std::vector<TTFPolyline> contours;
    //! Current pen position within the glyph (font units).
    double penX = 0.0;
    double penY = 0.0;
    //! Width factor / oblique shear applied after scaling.
    double widthFactor = 1.0;
    double oblique = 0.0;
    //! Accumulated contour count (for the ok flag).
    int contourCount = 0;

    void beginContour(double x, double y)
    {
        cur.clear();
        cur.emplace_back(x, y);
        penX = x;
        penY = y;
    }
    void lineTo(double x, double y)
    {
        cur.emplace_back(x, y);
        penX = x;
        penY = y;
    }
    void finishContour()
    {
        if (cur.size() >= 2) {
            TTFPolyline poly;
            poly.pts.reserve(cur.size());
            // Keep 26.6 font-unit coordinates here; the caller maps them to
            // model units with scale = height / size->metrics.height.
            for (const auto& p : cur) {
                double x = p.first * widthFactor;
                double y = p.second;
                if (oblique != 0.0) {
                    x += y * oblique;
                }
                poly.pts.emplace_back(x, y);
            }
            contours.push_back(std::move(poly));
            ++contourCount;
        }
        cur.clear();
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// FreeType callbacks
// ---------------------------------------------------------------------------
namespace
{

int ftMoveTo(const FT_Vector* to, void* user)
{
    auto* ctx = static_cast<OutlineCtx*>(user);
    ctx->finishContour();
    ctx->beginContour(to->x, to->y);
    return 0;
}

int ftLineTo(const FT_Vector* to, void* user)
{
    auto* ctx = static_cast<OutlineCtx*>(user);
    ctx->lineTo(to->x, to->y);
    return 0;
}

int ftConicTo(const FT_Vector* c, const FT_Vector* to, void* user)
{
    auto* ctx = static_cast<OutlineCtx*>(user);
    const int steps = 16;  // 2^4 subdivisions per Bezier segment
    sampleQuadratic(ctx->penX, ctx->penY, c->x, c->y, to->x, to->y, steps,
                    ctx->cur);
    ctx->penX = to->x;
    ctx->penY = to->y;
    return 0;
}

int ftCubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to,
              void* user)
{
    auto* ctx = static_cast<OutlineCtx*>(user);
    const int steps = 16;
    sampleCubic(ctx->penX, ctx->penY, c1->x, c1->y, c2->x, c2->y, to->x,
                to->y, steps, ctx->cur);
    ctx->penX = to->x;
    ctx->penY = to->y;
    return 0;
}

FT_Outline_Funcs outlineFuncs()
{
    FT_Outline_Funcs f;
    f.move_to = ftMoveTo;
    f.line_to = ftLineTo;
    f.conic_to = ftConicTo;
    f.cubic_to = ftCubicTo;
    f.shift = 0;
    f.delta = 0;
    return f;
}

}  // namespace

// ---------------------------------------------------------------------------
// Font file resolution
// ---------------------------------------------------------------------------
namespace
{

//! Font family name of a face, as a normalized (lowercase) string.
std::string faceFamily(FT_Face face)
{
    std::string fam = face && face->family_name ? face->family_name : "";
    for (char& c : fam) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return fam;
}

//! Lowercase copy of \a s (ASCII only).
std::string lowerAscii(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

//! True when \a lower (already lowercased) ends in a font file extension.
bool hasFontExtension(const std::string& lower)
{
    if (lower.size() < 4) {
        return false;
    }
    const std::size_t n = lower.size() - 4;
    return lower.compare(n, 4, ".ttf") == 0 || lower.compare(n, 4, ".otf") == 0
        || lower.compare(n, 4, ".ttc") == 0 || lower.compare(n, 4, ".otc") == 0;
}

//! Family name of face \a index inside \a path, or "" when it cannot be opened.
//!
//! FT_New_Face() is used with the *path* rather than FT_New_Memory_Face(): with
//! a path FreeType keeps its own stream and reads only the tables it needs (the
//! sfnt directory and the name table), so probing a font costs a few KB of I/O.
//! The previous code read every font file of the search directories into a
//! std::vector first - 475 files on a stock Windows installation, up to 16 MB
//! each - and paid that again for every family the drawing had not asked for
//! yet.
std::string faceFamilyAt(FT_Library lib, const std::string& path, FT_Long index)
{
    FT_Face face = nullptr;
    if (FT_New_Face(lib, toLocalPath(path).c_str(), index, &face) != 0
        || !face) {
        return {};
    }
    std::string fam = faceFamily(face);
    FT_Done_Face(face);
    return fam;
}

//! Record every family provided by one font file in \a index.  A .ttc / .otc
//! collection can hold several faces, so all of them are probed.  First hit
//! wins (emplace), which keeps the mapping stable across runs.
void indexFontFile(FT_Library lib,
                   const std::string& path,
                   std::map<std::string, std::string>& index)
{
    FT_Face probe = nullptr;
    if (FT_New_Face(lib, toLocalPath(path).c_str(), 0, &probe) != 0 || !probe) {
        return;
    }
    const FT_Long faces = probe->num_faces;
    const std::string first = faceFamily(probe);
    FT_Done_Face(probe);
    if (!first.empty()) {
        index.emplace(first, path);
    }
    for (FT_Long idx = 1; idx < faces; ++idx) {
        const std::string fam = faceFamilyAt(lib, path, idx);
        if (!fam.empty()) {
            index.emplace(fam, path);
        }
    }
}

std::vector<std::string>& fontSearchDirs()
{
    static std::vector<std::string> dirs;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string win = std::getenv("WINDIR") ? std::getenv("WINDIR") : "";
        if (!win.empty()) {
            dirs.push_back(win + "/Fonts");
        }
        dirs.push_back("C:/Program Files/Autodesk");
        // The resolved AutoCAD font dir may be reported via the importer;
        // add a couple of common ones for the standalone test path too.
        dirs.push_back("C:/Program Files/Autodesk/AutoCAD 2016/Fonts");
    });
    return dirs;
}

//! Family (lowercase) -> font file index of the font directories, built once on
//! first use and reused for every subsequent family lookup.
//!
//! This is what makes the resolver viable: the lookup is inherently "open every
//! font file and read its family name", so doing it once for the whole run
//! costs a fraction of a second, whereas doing it once per unresolved family -
//! which is what the previous implementation did - costs that again for every
//! font the drawing mentions.
std::map<std::string, std::string>& fontFamilyIndex()
{
    static std::map<std::string, std::string> index;
    static std::once_flag once;
    std::call_once(once, [] {
        FT_Library lib = ftLib();
        if (!lib) {
            return;
        }
        for (const std::string& dir : fontSearchDirs()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) {
                continue;
            }
            std::filesystem::recursive_directory_iterator it(
                dir,
                std::filesystem::directory_options::skip_permission_denied,
                ec);
            const std::filesystem::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const std::string ext =
                    lowerAscii(it->path().extension().string());
                if (ext != ".ttf" && ext != ".otf" && ext != ".ttc"
                    && ext != ".otc") {
                    continue;
                }
                indexFontFile(lib, it->path().string(), index);
            }
        }
    });
    return index;
}

}  // namespace

std::string resolveFontFile(const std::string& family)
{
    if (family.empty()) {
        return {};
    }
    static std::mutex mtx;
    static std::map<std::string, std::string> cache;
    const std::string key = lowerAscii(family);
    std::lock_guard<std::mutex> guard(mtx);
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    // An explicit .ttf / .otf / .ttc file name (e.g. "arial.ttf") is looked up
    // by name in the font directories.
    if (hasFontExtension(key)) {
        for (const std::string& dir : fontSearchDirs()) {
            std::filesystem::path p(dir);
            p /= family;
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec)) {
                const std::string fp = p.string();
                cache[key] = fp;
                return fp;
            }
        }
    }
    // Family lookup through the index of the font directories.
    const auto& index = fontFamilyIndex();
    if (auto it = index.find(key); it != index.end()) {
        cache[key] = it->second;
        return it->second;
    }
    // Some styles name a font by its file base name ("simsun") rather than by
    // its family name; accept that spelling as a last resort.
    if (!hasFontExtension(key)) {
        for (const std::string& dir : fontSearchDirs()) {
            for (const char* ext : {".ttf", ".otf", ".ttc", ".otc"}) {
                std::filesystem::path p(dir);
                p /= family + ext;
                std::error_code ec;
                if (std::filesystem::is_regular_file(p, ec)) {
                    const std::string fp = p.string();
                    cache[key] = fp;
                    return fp;
                }
            }
        }
    }
    cache[key] = {};
    return {};
}

// ---------------------------------------------------------------------------
// Text rendering
// ---------------------------------------------------------------------------

//! Render cache: complex-linetype text is stamped once per pattern period
//! (a 100-unit GAS_LINE line draws ~20 GAS runs), and every run used to
//! re-read the font file and re-decompose the glyphs, which visibly stalls
//! the 3D view.  The outlines are already in model units (scaled by
//! height/cap-height), so a cache hit returns a deep copy of the previous
//! result and the caller only transforms it to the insertion point.
namespace
{
struct TTFKey
{
    std::string fontFile;
    std::string text;
    double height = 0.0;
    double widthFactor = 1.0;
    double oblique = 0.0;
    bool operator==(const TTFKey& o) const
    {
        return fontFile == o.fontFile && text == o.text && height == o.height
            && widthFactor == o.widthFactor && oblique == o.oblique;
    }
};
struct TTFKeyLess
{
    bool operator()(const TTFKey& a, const TTFKey& b) const
    {
        if (a.fontFile != b.fontFile) {
            return a.fontFile < b.fontFile;
        }
        if (a.text != b.text) {
            return a.text < b.text;
        }
        if (a.height != b.height) {
            return a.height < b.height;
        }
        if (a.widthFactor != b.widthFactor) {
            return a.widthFactor < b.widthFactor;
        }
        return a.oblique < b.oblique;
    }
};
std::mutex& ttfCacheMutex()
{
    static std::mutex m;
    return m;
}

//! Upper bound on cached renders.  One entry is a single text run's outlines
//! (a few KB for a short run, more for a long CJK one), so a few hundred
//! entries stay in the low MB.
constexpr std::size_t ttfCacheLimit = 512;

//! A cached render plus the stamp that orders it for eviction.
struct TTFCacheEntry
{
    TTFRenderResult result;
    std::uint64_t lastUse = 0;
};

//! Recency clock for the cache; only ever touched under ttfCacheMutex().
std::uint64_t& ttfCacheClock()
{
    static std::uint64_t c = 0;
    return c;
}

std::map<TTFKey, TTFCacheEntry, TTFKeyLess>& ttfRenderCache()
{
    // Bounded with least-recently-used eviction.  It used to be emptied
    // completely as soon as it grew past 64 entries, which did not just cap the
    // memory - it made the cache useless, and turned every stamp back into a
    // full glyph decomposition, once a drawing carried more than 64 distinct
    // (text, height, width factor) combinations.  A complex linetype stamps one
    // entry per distinct embedded text and symbol scale, so that is reachable
    // on drawings with several such linetypes.
    static std::map<TTFKey, TTFCacheEntry, TTFKeyLess> c;
    return c;
}
}  // namespace

TTFRenderResult renderTextLine(const std::string& text,
                               const std::string& fontFile,
                               double height,
                               double widthFactor,
                               double oblique)
{
    TTFRenderResult result;
    if (fontFile.empty() || text.empty() || height <= 0.0) {
        return result;
    }
    const TTFKey key{fontFile, text, height, widthFactor, oblique};
    {
        std::lock_guard<std::mutex> lock(ttfCacheMutex());
        auto& cache = ttfRenderCache();
        const auto it = cache.find(key);
        if (it != cache.end()) {
            it->second.lastUse = ++ttfCacheClock();
            return it->second.result;  // deep copy: TTFRenderResult is copyable
        }
    }
    FT_Library lib = ftLib();
    if (!lib) {
        return result;
    }

    // Open the font by path: FreeType keeps its own stream and reads only the
    // tables it needs, so a call costs a few KB of I/O instead of a full copy
    // of the font.  The render cache misses for every distinct text / height /
    // width factor, and a CJK collection is 15-20 MB, so that copy used to be
    // the dominant part of this function.
    const std::string localFile = toLocalPath(fontFile);
    FT_Face face = nullptr;
    // TTC collections: pick the first face whose family matches the file
    // base name (best effort); otherwise face 0.
    const FT_Long faceCount = FT_New_Face(lib, localFile.c_str(), 0, &face) == 0
        ? face->num_faces
        : 0;
    if (!face) {
        return result;
    }
    if (faceCount > 1) {
        // Prefer a face matching the file base name (e.g. "simsun" in
        // simsun.ttc); keep face 0 otherwise.
        std::string want = fontFile;
        const std::size_t slash = want.find_last_of("/\\");
        if (slash != std::string::npos) {
            want = want.substr(slash + 1);
        }
        const std::size_t dot = want.find_last_of('.');
        if (dot != std::string::npos) {
            want = want.substr(0, dot);
        }
        want = lowerAscii(want);
        for (FT_Long idx = 1; idx < faceCount; ++idx) {
            FT_Face f2 = nullptr;
            if (FT_New_Face(lib, localFile.c_str(), idx, &f2) != 0) {
                continue;
            }
            if (faceFamily(f2) == want) {
                FT_Done_Face(face);
                face = f2;
                break;
            }
            FT_Done_Face(f2);
        }
    }

    // 48 pt x 10 (keeps detail), matching the Part module's FT2FC approach.
    if (FT_Set_Char_Size(face, 0, 48 * 64 * 10, 72, 72) != 0) {
        FT_Done_Face(face);
        return result;
    }

    // AutoCAD's complex-linetype text height S is the CAP height - the
    // visible height of an uppercase letter (e.g. S=2.54 for GAS_LINE gives
    // ~2.54 visually).  Scale against the 'H' glyph's outline bbox instead of
    // the em box so the rendered run matches AutoCAD: the em box then scales
    // proportionally, which stretches the advance widths to their AutoCAD
    // values (e.g. "GAS" renders ~7.04 wide instead of ~4.66 at S=2.54).
    double capPx = static_cast<double>(face->size->metrics.height) * 0.716;
    {
        const FT_UInt hidx = FT_Get_Char_Index(face, 0x48);  // 'H'
        if (hidx != 0
            && FT_Load_Glyph(face, hidx,
                             FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP)
                == 0
            && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE
            && face->glyph->metrics.height > 0) {
            capPx = static_cast<double>(face->glyph->metrics.height);
        }
    }
    const double scale = height / capPx;
    result.ascender = static_cast<double>(face->size->metrics.ascender) * scale;
    result.descender = static_cast<double>(face->size->metrics.descender) * scale;

    OutlineCtx ctx;
    ctx.widthFactor = widthFactor;
    ctx.oblique = oblique;
    FT_Outline_Funcs funcs = outlineFuncs();
    FT_UInt prev = 0;
    double penX = 0.0;

    constexpr FT_Int loadFlags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::uint32_t cp = utf8Next(text, pos);
        const FT_UInt idx = FT_Get_Char_Index(face, cp);
        if (idx != 0) {
            FT_Error err = FT_Load_Glyph(face, idx, loadFlags);
            if (err == 0 && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
                OutlineCtx gctx;
                gctx.widthFactor = widthFactor;
                gctx.oblique = oblique;
                // Translate by the current pen position.
                gctx.penX = penX;
                // FreeType outlines are relative to the glyph origin; the
                // pen advance is in 26.6 font units.
                FT_Outline_Translate(&face->glyph->outline,
                                     static_cast<FT_Pos>(penX),
                                     0);
                if (FT_Outline_Decompose(&face->glyph->outline, &funcs,
                                         &gctx)
                    == 0) {
                    gctx.finishContour();
                    for (TTFPolyline& poly : gctx.contours) {
                        for (auto& p : poly.pts) {
                            p.first *= scale;
                            p.second *= scale;
                        }
                        result.contours.push_back(std::move(poly));
                    }
                    if (gctx.contourCount > 0) {
                        result.ok = true;
                    }
                }
            }
            if (prev && face->face_flags & FT_FACE_FLAG_KERNING) {
                FT_Vector kern{};
                if (FT_Get_Kerning(face, prev, idx, FT_KERNING_DEFAULT,
                                   &kern)
                    == 0) {
                    penX += kern.x;
                }
            }
            penX += face->glyph->advance.x;
            prev = idx;
        }
        else {
            // Missing glyph: advance by the face's default width (0.5 em).
            penX += face->height / 2;
            prev = 0;
        }
    }

    result.width = penX * scale * widthFactor;
    FT_Done_Face(face);
    {
        std::lock_guard<std::mutex> lock(ttfCacheMutex());
        auto& cache = ttfRenderCache();
        cache[key] = TTFCacheEntry{result, ++ttfCacheClock()};
        while (cache.size() > ttfCacheLimit) {
            // Drop the least recently used entry instead of the whole cache.
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (it->second.lastUse < oldest->second.lastUse) {
                    oldest = it;
                }
            }
            cache.erase(oldest);
        }
    }
    return result;
}

TTFRenderResult measureTextLine(const std::string& family,
                                const std::string& text,
                                double height,
                                double widthFactor,
                                double oblique)
{
    const std::string fontFile = resolveFontFile(family);
    if (fontFile.empty()) {
        return {};
    }
    // Delegates to the cached renderer so a subsequent draw with identical
    // arguments reuses this same outline result.
    return renderTextLine(text, fontFile, height, widthFactor, oblique);
}

namespace
{

//! Width-only measurement used to re-create a face (re-reading + parsing the
//! whole font file) for every single text object, which dominated import time
//! on text-heavy DWGs.  One long-lived FT_Face per font file fixes that; the
//! faces are opened by path, so no font bytes have to be kept alive - an
//! FT_New_Memory_Face would pin 15-20 MB per CJK collection instead.
std::map<std::string, FT_Face>& faceFileCache()
{
    static std::map<std::string, FT_Face> c;
    return c;
}

//! Lower-cased file base name without extension (used to pick the matching
//! face inside a .ttc collection, mirroring renderTextLine).
std::string fileBaseLower(const std::string& fontFile)
{
    std::string b = fontFile;
    const std::size_t slash = b.find_last_of("/\\");
    if (slash != std::string::npos) {
        b = b.substr(slash + 1);
    }
    const std::size_t dot = b.find_last_of('.');
    if (dot != std::string::npos) {
        b = b.substr(0, dot);
    }
    for (char& c : b) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return b;
}

//! Long-lived FT_Face for a font file (caller must hold ttfCacheMutex()).
//!
//! Opened by path, so FreeType reads just the tables it needs and no font bytes
//! have to be held on to afterwards.
FT_Face cachedFace(const std::string& fontFile)
{
    auto& c = faceFileCache();
    auto it = c.find(fontFile);
    if (it != c.end()) {
        return it->second;
    }
    FT_Library lib = ftLib();
    if (!lib) {
        return nullptr;
    }
    const std::string local = toLocalPath(fontFile);
    FT_Face face = nullptr;
    const FT_Long faces = FT_New_Face(lib, local.c_str(), 0, &face) == 0
        ? face->num_faces
        : 0;
    if (!face) {
        return nullptr;
    }
    if (faces > 1) {
        const std::string want = fileBaseLower(fontFile);
        for (FT_Long idx = 1; idx < faces; ++idx) {
            FT_Face f2 = nullptr;
            if (FT_New_Face(lib, local.c_str(), idx, &f2) != 0) {
                continue;
            }
            if (faceFamily(f2) == want) {
                FT_Done_Face(face);
                face = f2;
                break;
            }
            FT_Done_Face(f2);
        }
    }
    c.emplace(fontFile, face);
    return face;
}

//! Advance-only line width with exactly the value renderTextLine() computes,
//! but without decomposing any glyph outline (no polyline contours are
//! produced, so a layout/measure call no longer costs a full rasterisation).
double measureWidthCached(FT_Face face, const std::string& text,
                          double height, double widthFactor)
{
    if (!face || text.empty() || height <= 0.0) {
        return -1.0;
    }
    if (FT_Set_Char_Size(face, 0, 48 * 64 * 10, 72, 72) != 0) {
        return -1.0;
    }
    double capPx = static_cast<double>(face->size->metrics.height) * 0.716;
    {
        const FT_UInt hidx = FT_Get_Char_Index(face, 0x48);  // 'H'
        if (hidx != 0
            && FT_Load_Glyph(face, hidx, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP)
                == 0
            && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE
            && face->glyph->metrics.height > 0) {
            capPx = static_cast<double>(face->glyph->metrics.height);
        }
    }
    if (capPx <= 0.0) {
        return -1.0;
    }
    const double scale = height / capPx;
    FT_Pos pen = 0;
    FT_UInt prev = 0;
    constexpr FT_Int loadFlags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::uint32_t cp = utf8Next(text, pos);
        const FT_UInt idx = FT_Get_Char_Index(face, cp);
        if (idx != 0) {
            FT_Load_Glyph(face, idx, loadFlags);
            if (prev && (face->face_flags & FT_FACE_FLAG_KERNING)) {
                FT_Vector kern{};
                if (FT_Get_Kerning(face, prev, idx, FT_KERNING_DEFAULT,
                                   &kern)
                    == 0) {
                    pen += kern.x;
                }
            }
            pen += face->glyph->advance.x;
            prev = idx;
        }
        else {
            pen += face->height / 2;  // missing glyph: default half-em advance
            prev = 0;
        }
    }
    return static_cast<double>(pen) * scale * widthFactor;
}

}  // namespace

double measureTextWidth(const std::string& family, const std::string& text,
                        double height, double widthFactor)
{
    if (family.empty() || text.empty() || height <= 0.0) {
        return -1.0;
    }
    const std::string fontFile = resolveFontFile(family);
    if (fontFile.empty()) {
        return -1.0;
    }
    std::lock_guard<std::mutex> lock(ttfCacheMutex());
    return measureWidthCached(cachedFace(fontFile), text, height, widthFactor);
}

}  // namespace SWDWG

#else  // !FCUseFreeType

namespace SWDWG
{

std::string resolveFontFile(const std::string&)
{
    return {};
}

TTFRenderResult renderTextLine(const std::string&, const std::string&,
                               double, double, double)
{
    return {};
}

TTFRenderResult measureTextLine(const std::string&, const std::string&,
                                double, double, double)
{
    return {};
}

double measureTextWidth(const std::string&, const std::string&,
                        double, double)
{
    return -1.0;
}

}  // namespace SWDWG

#endif  // FCUseFreeType
