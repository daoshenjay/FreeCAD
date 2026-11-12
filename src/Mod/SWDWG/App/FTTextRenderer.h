// SPDX-License-Identifier: LGPL-2.1-or-later
//! FreeType-backed TrueType text outline extractor.
//!
//! Complex linetypes can embed text (GAS_LINE's "GAS", HOT_WATER_SUPPLY's
//! "HW") whose STYLE font is a TrueType file (arial.ttf).  The legacy
//! addTextGlyphs() only understood AutoCAD .shx fonts and fell back to a
//! placeholder box for TTF.  This renderer extracts the real glyph outlines
//! with FreeType and returns them as closed polylines in model units
//! (baseline at y = 0), which the caller turns into OCCT edges.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! One closed outline ring of a glyph (2D, y-up, model units after scaling).
struct TTFPolyline
{
    std::vector<std::pair<double, double>> pts;
};

//! Outline + metrics of one rendered text line.
struct TTFRenderResult
{
    //! All glyph outlines concatenated (per-contour point rings).
    std::vector<TTFPolyline> contours;
    //! Pen advance width (model units).
    double width = 0.0;
    //! Font ascender / descender (model units, y-up; descender < 0).
    double ascender = 0.0;
    double descender = 0.0;
    //! True when at least one glyph produced an outline.
    bool ok = false;
};

//! Resolve a font family name (e.g. "Arial", "STFangsong", "SimSun") to a
//! font file path by scanning the system and AutoCAD font directories with
//! FreeType and matching the family name.  Supports .ttf / .otf / .ttc
//! collections.  Results are cached.
//!
//! \return empty string when no matching font file is found.
SWDWGExport std::string resolveFontFile(const std::string& family);

//! Measure one UTF-8 text line with a font family (not a file path).
//! Resolves the family through resolveFontFile() and reuses the cached
//! renderTextLine() result, so a later draw with the same arguments hits the
//! same cache entry.  The returned TTFRenderResult.width / ascender /
//! descender are in model units (baseline at y = 0).
//!
//! \return an empty (ok=false) result when the family cannot be resolved.
SWDWGExport TTFRenderResult measureTextLine(const std::string& family,
                                            const std::string& text,
                                            double height,
                                            double widthFactor = 1.0,
                                            double oblique = 0.0);

//! Measure the rendered width of one UTF-8 text line in model units using
//! the mapped TrueType family (as stored in DWGText::FontName).  WidthFactor
//! (DXF 41, >0) is already applied inside renderTextLine(); pass the same
//! value used when the text is drawn so the layout and the glyphs agree.
//!
//! \return the advance width in model units, or a negative value when the
//!         family could not be resolved (caller decides the fallback).
SWDWGExport double measureTextWidth(const std::string& family,
                                    const std::string& text,
                                    double height,
                                    double widthFactor = 1.0);

//! Render one UTF-8 text line into glyph outlines.
//!
//! \param text        UTF-8 text (may contain CJK / fullwidth characters).
//! \param fontFile    Path returned by resolveFontFile().
//! \param height      Desired text height in model units (the complex-linetype
//!                    scale S).  1 font unit maps to height / face->height.
//! \param widthFactor Horizontal stretch (1.0 = normal); applied to X only.
//! \param oblique     Shear tangent (tan of the oblique angle); x += oblique*y.
//! \param out         Receives the scaled outlines (baseline at y = 0).
TTFRenderResult renderTextLine(const std::string& text,
                               const std::string& fontFile,
                               double height,
                               double widthFactor = 1.0,
                               double oblique = 0.0);

}  // namespace SWDWG
