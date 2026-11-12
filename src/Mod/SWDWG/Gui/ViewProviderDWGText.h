#pragma once

#include <vector>

#include <Gui/ViewProviderDocumentObject.h>

#include "../SWDWGGlobal.h"

class SoFont;
class SoAsciiText;
class SoBaseColor;
class SoCoordinate3;
class SoIndexedLineSet;
class SoIndexedFaceSet;
class SoTransform;
class SoMatrixTransform;
class SoSeparator;
class SoGroup;

namespace SWDWG
{
struct ShxLineRender;
}

namespace SWDWG
{

//! View provider for SWDWG::DWGText.
//!
//! Renders every text line with a dedicated SoAsciiText node so that the
//! glyphs keep their world-unit size (the DWG text height) when the view is
//! zoomed, exactly like text in AutoCAD.  The scene graph mirrors the DWG
//! semantics:
//!
//!   pTransform  anchor point (Position) + rotation about the anchor
//!   per line    local offset (left baseline relative to the anchor)
//!               -> shared width-factor/mirror scale
//!               -> shared oblique shear
//!               -> glyph geometry
//!
//! Glyph geometry is produced from the *real* font outline so the rendered
//! result matches the measured layout exactly:
//!   - TrueType families (Arial, STFangsong, ...) are decomposed through
//!     FreeType (FTTextRenderer) into closed rings and drawn as filled
//!     SoIndexedFaceSet faces;
//!   - AutoCAD .shx files (txt.shx, romans.shx, big fonts) are drawn with
//!     their native single-line strokes as SoIndexedLineSet edges.
//! Each line keeps its own SoAsciiText fallback (font scaling) while the
//! outline geometry, when available, overrides it for AutoCAD-faithful look.
class SWDWGGuiExport ViewProviderDWGText: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::ViewProviderDWGText);

public:
    /// Constructor
    ViewProviderDWGText();
    ~ViewProviderDWGText() override;

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;
    /// Called by the GUI once the document restore finished: rebuilds the
    /// glyph scene graph exactly once (updateData was deferred while the
    /// object was being restored).
    void finishRestoring() override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* ModeName) override;

protected:
    void onChanged(const App::Property* prop) override;

private:
    //! True while a bulk import or a document restore is in progress.  Heavy
    //! SHX / FreeType rebuilds are then deferred (to the post-import touch or
    //! to finishRestoring) instead of running once per property write.
    bool deferHeavyRebuild();
    //! Rebuild the per-line glyph nodes from LabelText / LabelOffsets.
    void rebuildLines();
    //! Push WidthFactor / Mirror into every per-line scale node.
    void updateScale();
    //! Push ObliqueAngle into every per-line shear matrix.
    void updateShear();

    //! Build one text line with the classic .shx single-line fonts.  Returns
    //! true and appends SoCoordinate3 + SoIndexedLineSet nodes to \a group
    //! (the line's own separator, so the strokes stay inside that line's
    //! offset/scale/shear) when the object's STYLE is a CAD shape font and a
    //! usable file was found; false leaves the caller to fall back to
    //! SoAsciiText.
    //! \a lineOff is the SoTransform already carrying the LabelOffsets value
    //! from DWGText::recomputeLayout.  For a single-line TEXT object the
    //! function recomputes that placement from the ACTUAL stroked geometry
    //! (renderShxLine centres every big-font cell on the insertion point, so
    //! the TrueType-estimated LabelOffsets dy would double-shift it).
    bool addShxLine(const std::string& line, SoTransform* lineOff,
                    SoGroup* group);

    //! Build vertical (top-to-bottom) text, one character per column, with
    //! the classic .shx fonts.  \a chars are the individual UTF-8 characters
    //! in reading order (first at the top).  Every glyph is drawn under the
    //! caller's offset/scale/shear with the character step = font height, into
    //! \a group (the column's own separator).
    //! Returns false when no usable .shx font is available so the caller can
    //! fall back to the multi-line SoAsciiText path.
    bool addShxColumn(const std::vector<std::string>& chars, SoGroup* group);

    SoFont* pFont;
    SoBaseColor* pColor;
    SoTransform* pTransform;   //!< anchor + rotation (DWG rotation centre)
    SoGroup* pLineGroup;       //!< one offset+glyph sub-tree per line
    SoSeparator* pRoot = nullptr;
    //! Per-line width-factor / mirror scale nodes.  A separate node per line
    //! is required because Coin3D removes a node from its previous parent
    //! when it is added to another group, so a single shared node would only
    //! apply to the last line.
    std::vector<SoTransform*> m_lineScales;
    //! Per-line oblique shear matrices (see m_lineScales).
    std::vector<SoMatrixTransform*> m_lineShears;
};

}  // namespace SWDWG
