#pragma once

#include <App/Annotation.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include <string>
#include <vector>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! AutoCAD's single line spacing for TEXT/MTEXT: 5/3 of the character height
//! (its "3-on-5" rule).
//!
//! The DWG stores only the *multiple* (DWGText::LineSpacing / linespace_factor,
//! DXF 44) and the spacing *style* (DXF 73); the single value itself is a
//! convention, so it has to live in code - here as one named constant instead
//! of a bare 1.66.
//!
//! It is not taken on faith: the extents boxes the drawing stores for its own
//! MTEXTs invert to this number.  Solving
//!     extents_height = (rows-1) * k * (height * factor) + height
//! for every MTEXT whose row count and box are known gives k = 1.6667 exactly
//! (minimum = median = maximum) for 6 of the 14 candidates - among them the
//! reported 浒光运河 label, whose stored box is
//!     20.000 = 3 * (5/3 * 4 * 0.8) + 4.
//! (The remaining ones are explained in the probe: two are single-line labels
//! the importer wrongly wraps into two rows, six have boxes that fit neither
//! reading - see docs / probe derive_spacing.py.)
inline constexpr double kSingleLineSpacing = 5.0 / 3.0;

//! Split a UTF-8 string into one entry per Unicode character.
//! Declared here (instead of the LibreDWG-dependent DWGInternal.h) so that
//! the Gui module can use it without pulling in the libredwg headers.
//! Exported so that SWDWGGui can link against the SWDWG App DLL.
SWDWGExport std::vector<std::string> splitUtf8Chars(const std::string& text);

//! A TEXT/MTEXT entity of a DWG drawing.
//!
//! Unlike App::Annotation (whose view provider renders with SoText2 /
//! SoAsciiText, i.e. fixed-size or ASCII-only text), this type carries the
//! full text geometry attributes of a DWG text entity (height in world
//! units, rotation, color, font) so that its view provider can render the
//! text with SoText3 at true model-space size.
class SWDWGExport DWGText: public App::Annotation
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::DWGText);

public:
    /// Constructor
    DWGText();
    ~DWGText() override;

    /// Text height in world units (mm), from the DWG TEXT/MTEXT height.
    App::PropertyFloat FontHeight;
    /// Rotation about the Z axis, in radians.  The rotation always happens
    /// around the anchor point (Position), exactly like AutoCAD.
    App::PropertyAngle Rotation;
    /// Text color (resolved from the entity / layer color).
    App::PropertyColor TextColor;
    /// TrueType font name used for rendering (mapped from the DWG text
    /// style / shx font).
    App::PropertyString FontName;
    /// DWG layer name the entity lives on.
    App::PropertyString Layer;
    /// Imported entity kind ("TEXT" / "MTEXT", read-only, tree grouping key).
    App::PropertyString EntityKind;

    // --- DWG text style attributes (added for full AutoCAD mapping) -------

    /// STYLE table entry this entity references (e.g. "Standard").
    App::PropertyString StyleName;
    /// Horizontal stretch factor (width/height ratio); 1.0 = normal.
    App::PropertyFloat WidthFactor;
    /// Oblique angle in radians (AutoCAD stores it in degrees).
    App::PropertyAngle ObliqueAngle;
    /// Text generation flags: 0 = normal, 1 = upside-down, 2 = backward.
    App::PropertyInteger Mirror;
    /// TEXT horizontal alignment: 0 left, 1 center, 2 right, 3 aligned,
    /// 4 middle, 5 fit.
    App::PropertyEnumeration HorAlign;
    /// TEXT vertical alignment: 0 baseline, 1 bottom, 2 middle, 3 top.
    App::PropertyEnumeration VertAlign;
    /// MTEXT attachment point: 1 top-left ... 9 bottom-right.
    App::PropertyInteger Attachment;
    /// MTEXT line spacing factor (0.25 .. 4.0); 1.0 = AutoCAD default.
    App::PropertyFloat LineSpacing;
    /// MTEXT line spacing style: 1 = at least, 2 = exactly.
    App::PropertyInteger LineSpacingStyle;
    /// TEXT Aligned(3) / Fit(5): second baseline point (world units).
    App::PropertyVector AlignPt2;
    /// True when the entity is a DWG MTEXT (drives the per-line layout path).
    App::PropertyBool IsMText;
    /// True when the text style has vertical direction (STYLE DXF 70 bit 1).
    App::PropertyBool IsVertical;
    /// Per-line local offsets (left-baseline origin relative to the anchor,
    /// in the un-rotated text plane, y up).  One entry per rendered line;
    /// TEXT always has exactly one.
    App::PropertyVectorList LabelOffsets;
    /// STYLE font file name (e.g. "txt.shx"), informational.
    App::PropertyString FontFile;
    /// STYLE big-font (CJK) file name (e.g. "fsdb.shx"), empty when none.
    App::PropertyString BigFontFile;
    // ===================== 导出功能 BEGIN =====================
    /// 原始插入点（DXF 组码 10）。导出按左/基线回写，位置与源图一致。
    App::PropertyVector InsPt;
    // ===================== 导出功能 END =======================
    /// DWG codepage (0x1F4 = 500 ANSI, 0x3A8 = 936 GBK, ...) of the drawing.
    App::PropertyInteger Codepage;
    /// Rendering engine override: 0 auto, 1 force TTF, 2 force SHX.
    App::PropertyInteger RenderEngine;

    //! Recompute LabelOffsets (and, for Aligned/Fit TEXT, Rotation and
    //! FontHeight) from the current property values.  Called automatically
    //! whenever any alignment-affecting property changes.
    void recomputeLayout();

    //! Run recomputeLayout() guarded against recursive re-entry (its
    //! LabelOffsets / FontHeight writes would otherwise trigger onChanged and
    //! re-enter).  The importer calls this once after a bulk-create window.
    void recomputeLayoutOnce();

    //! Enter / leave a bulk-create window (used while the importer fills the
    //! properties of a fresh text object).  Inside the window onChanged skips
    //! the automatic re-layout and the Gui view provider skips glyph rebuilds,
    //! so importing a drawing with thousands of text entities does not run the
    //! FreeType measurement / SHX decoding once per property.  Nested use is
    //! counted.
    static void enterBulkCreate();
    static void leaveBulkCreate();
    static bool isBulkCreating();

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "SWDWG::ViewProviderDWGText";
    }

protected:
    void onChanged(const App::Property* prop) override;

private:
    bool m_bRecomputing = false;
};

}  // namespace SWDWG
