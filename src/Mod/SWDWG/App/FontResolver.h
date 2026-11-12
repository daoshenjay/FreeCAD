//! Font engine selection for DWG text rendering.
//!
//! AutoCAD text styles reference three kinds of glyph source:
//!   - TrueType/OpenType fonts (arial.ttf), resolved by family name;
//!   - classic single-byte .shx shape files (txt.shx / romans.shx);
//!   - double-byte big-font .shx files (fsdb.shx / gbcbig.shx) for CJK.
//!
//! Given a STYLE record (font file + optional big font) and the text run
//! itself, FontResolver decides which engine to use and resolves the concrete
//! file / family.  It deliberately has no FreeCAD / OCCT / GUI dependency so
//! it can be shared by the App importer, the Gui view provider and the unit
//! tests.  It re-uses the .shx parser (DWGShapeFile.h) and its file lookup.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "DWGShapeFile.h"
#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! Rendering engine chosen for one text run.
enum class TextFontEngine
{
    Ttf,       //!< TrueType family (resolveFontFile / SoFont)
    Shx,       //!< classic single-byte .shx shape file
    BigFont,   //!< double-byte .shx big font (CJK)
};

//! Resolved glyph source for one text style + run.
struct TextFontChoice
{
    TextFontEngine engine = TextFontEngine::Ttf;
    //! TrueType family name to fall back to when no SHX file is installed
    //! (also the family for engine == Ttf).
    std::string ttfFamily;
    //! True when the run contains non-ASCII characters (CJK / fullwidth).
    bool hasCjk = false;
    //! STYLE font file as stored in the DWG (e.g. "txt.shx"), may be a bare
    //! .ttf file name.  Empty for the built-in Standard style.
    std::string fontFile;
    //! STYLE big-font file (e.g. "gbcbig.shx"); empty when the style has no
    //! separate CJK font.
    std::string bigFontFile;
    //! Absolute path of the located .shx (engine Shx/BigFont), or empty.
    std::string shxPath;
    //! Absolute path of the located big-font .shx (engine BigFont), or empty.
    std::string bigFontPath;
    //! True when the requested SHX file could not be found and the caller
    //! should fall back to ttfFamily.
    bool shxMissing = false;
};

//! True when a UTF-8 string contains any non-ASCII byte.
SWDWGExport bool textHasCjk(const std::string& utf8Text);

//! Classify a DWG STYLE font file name: TrueType family candidate, classic
//! .shx, or big-font .shx.  \a text is the UTF-8 run the font will display.
//! \a extraShxPaths are searched before the standard AutoCAD locations.
SWDWGExport TextFontChoice resolveTextFont(const std::string& fontFile,
                                           const std::string& bigFontFile,
                                           const std::string& utf8Text,
                                           const std::vector<std::string>&
                                               extraShxPaths = {});

//! Cached .shx parser handle.  Parsing a full shape file (especially a
//! 10k-glyph big font such as fsdb.shx) is expensive, so resolved files are
//! kept in a small LRU cache.  Callers use this instead of parseShapeFile()
//! directly when they may touch the same file repeatedly.
//!
//! This is the single shared parse cache for .shx files: the DWG line-group
//! L2 path (SWDWG::loadShapeFile) and the text SHX path both go through it,
//! so the same font / ltypeshp.shx is decoded at most once per process.
//! The shared_ptr keeps a file alive for any caller even if the LRU later
//! evicts it from the cache.
class SWDWGExport ShapeFileCache
{
public:
    //! Returns the parsed shape file for \a path, loading + caching on first
    //! use.  Returns nullptr when the file cannot be parsed (not cached).
    std::shared_ptr<const ShapeFile> get(const std::string& path);

    //! Global shared instance.
    static ShapeFileCache& instance();

private:
    std::map<std::string, std::shared_ptr<const ShapeFile>> m_files;
    std::vector<std::string> m_lru;  // insertion order ~ LRU
    static constexpr std::size_t kMaxEntries = 32;
};

}  // namespace SWDWG
