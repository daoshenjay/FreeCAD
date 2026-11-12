#pragma once

#include <vector>

#include <Gui/ViewProviderDocumentObject.h>

#include "../SWDWGGlobal.h"

class SoAsciiText;
class SoBaseColor;
class SoCoordinate3;
class SoFont;
class SoIndexedFaceSet;
class SoIndexedLineSet;
class SoLightModel;
class SoNormal;
class SoNormalBinding;
class SoSeparator;
class SoTransform;

namespace SWDWG
{

//! View provider for SWDWG::DWGDimension.
//!
//! The import pipeline resolves the DIMENSION geometry once into world-space
//! point lists (see buildDimensionGeometry in DWGDimension.cpp); this
//! provider renders exactly those points:
//!
//!   dimension line / arc / leader segments + arrow triangles  in DIMCLRD
//!   extension lines                                           in DIMCLRE
//!   measurement text (SoAsciiText)                            in DIMCLRT
//!
//! Text is rendered with SoAsciiText so the glyphs keep their world-unit
//! height (DIMTXT x DIMSCALE) when the view is zoomed, exactly like the DWG
//! text provider.

class SWDWGGuiExport ViewProviderDWGDimension: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::ViewProviderDWGDimension);

public:
    /// Constructor
    ViewProviderDWGDimension();
    ~ViewProviderDWGDimension() override;

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;
    /// Called by the GUI once the document restore finished: rebuilds the
    /// geometry scene graph exactly once (updateData was deferred while the
    /// object was being restored).
    void finishRestoring() override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* ModeName) override;

protected:
    void onChanged(const App::Property* prop) override;

private:
    //! True while a bulk import or a document restore is in progress.  Heavy
    //! geometry / SHX-text rebuilds are then deferred (to the post-import
    //! touch or to finishRestoring) instead of running once per property.
    bool deferHeavyRebuild();
    //! Rebuild every scene-graph leaf from the DWGDimension properties.
    void rebuildGeometry(const char* why = "");

    // Dimension line / arc / leader + arrow heads (DIMCLRD).
    SoBaseColor* pDimColor = nullptr;
    SoSeparator* pLineGroup = nullptr;
    SoCoordinate3* pDimCoords = nullptr;
    SoIndexedLineSet* pDimLines = nullptr;
    SoCoordinate3* pArrowCoords = nullptr;
    SoNormalBinding* pArrowNormalBinding = nullptr;
    SoIndexedFaceSet* pArrows = nullptr;
    SoNormal* pArrowNormals = nullptr;
    SoLightModel* pLineLightModel = nullptr;
    // Extension lines (DIMCLRE).
    SoBaseColor* pExtColor = nullptr;
    SoCoordinate3* pExtCoords = nullptr;
    SoIndexedLineSet* pExtLines = nullptr;
    // Dimension text (DIMCLRT), anchored at TextPosition, rotated about Z.
    SoBaseColor* pTextColor = nullptr;
    SoFont* pFont = nullptr;
    SoTransform* pTextTransform = nullptr;
    SoAsciiText* pText = nullptr;
    // SHX single-line strokes of the dimension text (rebuilt per update).
    SoSeparator* pShxTextGroup = nullptr;
    SoSeparator* pRoot = nullptr;
};

}  // namespace SWDWG
