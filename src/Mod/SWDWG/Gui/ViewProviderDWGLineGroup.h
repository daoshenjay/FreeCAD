#pragma once

#include <vector>

#include <Gui/ViewProviderDocumentObject.h>

#include "../SWDWGGlobal.h"

class SoBaseColor;
class SoSeparator;
class SoGroup;

namespace SWDWG
{

//! Lightweight view provider for SWDWG::DWGLineGroup.
//!
//! The group feature stores its final (linetype-cut) geometry in the
//! Part::Feature "Shape" property, but it must NOT be displayed through
//! PartGui::ViewProviderPart: building that provider's per-shape Coin/SoShapeKit
//! scene on every recompute costs ~0.5 s per group (measured 16.7 s for the 32
//! line groups of a 67k-entity drawing while the geometry executes took ~1 s).
//!
//! This provider renders the Shape itself, once, into a handful of flat Coin
//! nodes (single SoCoordinate3 + SoIndexedLineSet for the edges, a second pair
//! for the triangulated faces when the shape contains any), so a recompute /
//! restore / camera move rebuilds nothing through the Part display pipeline.
class SWDWGGuiExport ViewProviderDWGLineGroup
    : public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::ViewProviderDWGLineGroup);

public:
    ViewProviderDWGLineGroup();
    ~ViewProviderDWGLineGroup() override;

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;
    /// Called once the document restore finished (updateData is deferred while
    /// the object is restoring): build the display a single time.
    void finishRestoring() override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* ModeName) override;

protected:
    void onChanged(const App::Property* prop) override;

private:
    //! True while the surrounding document is being restored (defer rebuilds).
    bool restoring() const;
    //! True while the importer / bulk-create window is running (defer).
    bool importing() const;
    //! (Re)build the line / face node set from the current Shape.
    void rebuildGeometry();

    SoBaseColor* pColor = nullptr;
    //! Container whose children are replaced on every rebuild.
    SoGroup* pGeom = nullptr;
    SoSeparator* pRoot = nullptr;
};

}  // namespace SWDWG
