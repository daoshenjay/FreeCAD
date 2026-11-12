//! Classic AutoCAD .shx text rendering service.
//!
//! DWG TEXT/MTEXT entities whose STYLE uses a single-line CAD font
//! (txt.shx / romans.shx) or a double-byte big font (gbcbig.shx /
//! fsdb.shx) cannot be shown by Coin's TrueType text nodes.  This service
//! turns one UTF-8 text line into the native single-line strokes of the
//! referenced shape file(s), scaled so the glyph height equals the DWG text
//! height, exactly like AutoCAD's plot.
//!
//! The ViewProvider (Gui) calls renderShxLine() to build its scene geometry;
//! the unit test (tests/test_shxtext.cpp) validates the mapping + scaling.

#pragma once

#include <string>
#include <vector>

#include "DWGShapeFile.h"
#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! Result of rendering one text line with the .shx engines.
struct ShxLineRender
{
    //! Per-character glyph strokes (model units, left-baseline origin,
    //! y up).  One entry per character that produced an outline; characters
    //! with no glyph (whitespace, unmapped) are skipped.
    std::vector<std::vector<ShapeSeg>> glyphs;
    //! Total advance width of the line (model units).
    double width = 0.0;
    //! Ascender (max top above baseline) / descender (max bottom below) in
    //! model units, y-up.
    double ascender = 0.0;
    double descender = 0.0;
    //! True when at least one glyph was produced.
    bool ok = false;
};

//! Render one UTF-8 text line with the AutoCAD single-line shape-file fonts.
//!
//! \param fontFile     STYLE font file, e.g. "txt.shx" (ASCII / classic
//!                     shape font) or a resolved path.
//! \param bigFontFile  STYLE big-font file, e.g. "gbcbig.shx" / "fsdb.shx";
//!                     empty when the style has no CJK font.
//! \param codepage     DWG codepage (e.g. 936 = GBK) used to map UTF-8
//!                     characters back to the font's character codes.
//! \param utf8Text     One line of UTF-8 text.
//! \param height       Text height in model units (DWG text height).
//! \param widthFactor  Horizontal stretch (1.0 = normal).
//! \param extraShxPaths Paths searched before the standard AutoCAD locations.
//!
//! \return The strokes.  ok=false when no usable shape file was found or no
//!         glyph could be decoded.
SWDWGExport ShxLineRender renderShxLine(const std::string& fontFile,
                                        const std::string& bigFontFile,
                                        int codepage,
                                        const std::string& utf8Text,
                                        double height,
                                        double widthFactor = 1.0,
                                        const std::vector<std::string>&
                                            extraShxPaths = {});

//! True when a STYLE referencing this font (and optional big font) can be
//! rendered by the .shx engines, i.e. the font file is a .shx that exists.
SWDWGExport bool isShxFont(const std::string& fontFile,
                           const std::string& bigFontFile = {},
                           const std::vector<std::string>& extraShxPaths =
                               {});

//! Map one UTF-8 string to the character codes the AutoCAD font expects
//! under \a codepage (single bytes for ASCII, two-byte codes for CJK).
//! Returns a flat byte/code list usable as .shx shape numbers.
SWDWGExport std::vector<int> utf8ToFontCodes(const std::string& utf8,
                                             int codepage);

}  // namespace SWDWG
