//! Font engine selection - implementation.

// Note: this file deliberately does NOT include PreCompiled.h so the unit
// test can compile it standalone (it has no FreeCAD / OCCT / Xerces deps).

#include "FontResolver.h"

#include <algorithm>
#include <cctype>
#include <mutex>

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

//! Lowercase extension of a file name ("" when none).
std::string fontExt(const std::string& base)
{
    const std::size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? std::string()
                                    : base.substr(dot + 1);
}

//! True when the file name is a classic / big-font SHX.
bool isShxName(const std::string& base)
{
    return fontExt(base) == "shx";
}

//! AutoCAD .shx family -> TrueType look-alike family (same table as the
//! legacy DWGExtract.cpp mapper, kept here so the resolver is self-contained).
std::string shxToTtfFamily(const std::string& base, bool hasCjk,
                           const std::string& bigFontBase)
{
    // 1) Big font (large / CJK font).
    if (hasCjk && !bigFontBase.empty()) {
        if (bigFontBase.find("gbcbig") != std::string::npos
            || bigFontBase.find("gbbig") != std::string::npos
            || bigFontBase.find("gb2312") != std::string::npos) {
            return "SimSun";  // 宋体 (present on every Chinese Windows)
        }
        if (bigFontBase.find("hztxt") != std::string::npos) {
            return "SimHei";
        }
        return "SimSun";
    }

    // 2) Style font file.
    if (base.find("txt") != std::string::npos
        || base.find("simplex") != std::string::npos) {
        return "Arial";
    }
    if (base.find("romans") != std::string::npos
        || base.find("romand") != std::string::npos
        || base.find("romant") != std::string::npos) {
        return "Times New Roman";
    }
    if (base.find("isocp") != std::string::npos
        || base.find("isoc") != std::string::npos
        || base.find("iso") != std::string::npos) {
        return "Arial Narrow";
    }
    if (base.find("gothice") != std::string::npos) {
        return "KaiTi";  // 楷体 for gothice.shx
    }
    if (base.find("gothicg") != std::string::npos) {
        return "SimHei";  // 黑体 for gothicg.shx
    }
    if (base.find("gothici") != std::string::npos) {
        return "SimHei";  // 黑体 for gothici.shx
    }
    if (base.find("mono") != std::string::npos
        || base.find("complex") != std::string::npos) {
        return "Courier New";
    }
    // 3) Fallback: SimSun (宋体) is present on every Chinese Windows and is
    // the de-facto replacement for an unknown CJK drawing font.
    return hasCjk ? "SimSun" : "Arial";
}

}  // namespace

namespace SWDWG
{

bool textHasCjk(const std::string& utf8Text)
{
    for (const unsigned char c : utf8Text) {
        if (c >= 0x80) {
            return true;
        }
    }
    return false;
}

TextFontChoice resolveTextFont(const std::string& fontFile,
                               const std::string& bigFontFile,
                               const std::string& utf8Text,
                               const std::vector<std::string>& extraShxPaths)
{
    TextFontChoice c;
    c.fontFile = fontFile;
    c.bigFontFile = bigFontFile;
    c.hasCjk = textHasCjk(utf8Text);

    const std::string fBase = fontBaseName(fontFile);
    const std::string bfBase = fontBaseName(bigFontFile);
    const bool fIsShx = isShxName(fBase);
    const bool bfIsShx = isShxName(bfBase);

    // Default TTF family candidate (also used as the "missing SHX" fallback).
    c.ttfFamily = shxToTtfFamily(fBase, c.hasCjk, bfBase);

    // A double-byte big font for CJK text takes precedence when the style
    // declares one and the file is found.
    if (c.hasCjk && bfIsShx && !bigFontFile.empty()) {
        const std::string bigPath =
            findShapeFile(bigFontFile, extraShxPaths);
        if (!bigPath.empty()) {
            c.bigFontPath = bigPath;
            c.shxPath = bigPath;
            c.engine = TextFontEngine::BigFont;
            return c;
        }
    }

    // Classic single-byte SHX (or big-font used without CJK) -> Shx engine.
    if (fIsShx && !fontFile.empty()) {
        const std::string shxPath = findShapeFile(fontFile, extraShxPaths);
        if (!shxPath.empty()) {
            c.shxPath = shxPath;
            // If a big font was declared too and both files exist, the SHX
            // engine renders ASCII from the base file; CJK still comes from
            // the big font when hasCjk.  Prefer the big-font path for CJK.
            if (c.hasCjk && bfIsShx) {
                const std::string bigPath =
                    findShapeFile(bigFontFile, extraShxPaths);
                if (!bigPath.empty()) {
                    c.bigFontPath = bigPath;
                    c.engine = TextFontEngine::BigFont;
                    return c;
                }
            }
            c.engine = TextFontEngine::Shx;
            return c;
        }
        c.shxMissing = true;
        c.engine = TextFontEngine::Ttf;  // fall back to the family mapping
        return c;
    }

    // Explicit TrueType / OpenType file (e.g. "arial.ttf") or bare family.
    if (!fontFile.empty() && !fIsShx) {
        const std::string ext = fontExt(fBase);
        if (ext == "ttf" || ext == "otf" || ext == "ttc") {
            c.engine = TextFontEngine::Ttf;
            c.ttfFamily = fBase.substr(0, fBase.size() - ext.size() - 1);
            return c;
        }
        // Bare family name (rare) - use as-is.
        c.ttfFamily = fontFile;
        c.engine = TextFontEngine::Ttf;
        return c;
    }

    // No font file: built-in style, pick a default.
    c.engine = TextFontEngine::Ttf;
    return c;
}

// ---------------------------------------------------------------------------
// ShapeFileCache
// ---------------------------------------------------------------------------

namespace
{

std::mutex& shapeCacheMutex()
{
    static std::mutex m;
    return m;
}

}  // namespace

std::shared_ptr<const ShapeFile> ShapeFileCache::get(const std::string& path)
{
    if (path.empty()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(shapeCacheMutex());

    auto it = m_files.find(path);
    if (it != m_files.end()) {
        // Refresh LRU position.
        const auto lru = std::find(m_lru.begin(), m_lru.end(), path);
        if (lru != m_lru.end()) {
            m_lru.erase(lru);
        }
        m_lru.push_back(path);
        return it->second;
    }

    ShapeFile sf = parseShapeFile(path);
    if (sf.empty()) {
        return nullptr;  // do not cache failures
    }
    auto sp = std::make_shared<ShapeFile>(std::move(sf));
    m_files[path] = sp;
    m_lru.push_back(path);

    // Evict the least recently used entry beyond the cap.  Callers keep a
    // shared_ptr, so eviction never invalidates an in-use file.
    while (m_files.size() > kMaxEntries) {
        const std::string victim = m_lru.front();
        m_lru.erase(m_lru.begin());
        m_files.erase(victim);
    }
    return sp;
}

ShapeFileCache& ShapeFileCache::instance()
{
    static ShapeFileCache cache;
    return cache;
}

}  // namespace SWDWG
