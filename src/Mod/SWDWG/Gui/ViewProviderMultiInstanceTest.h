//! Demo view provider for SoMultipleCopy instancing.
//!
//! Renders a small template geometry (a diamond / plus / octagon outline)
//! many times by feeding an array of instance matrices into a single
//! SoMultipleCopy node.  Editing Rows/Cols/Spacing on the App object
//! rebuilds the matrix array (no scene-graph churn); changing Template
//! rebuilds the single shared template.
//!
//! Note: the (faster) SoMultipleInstance subclass is not present in every
//! Coin3D / Inventor build, so this provider sticks to the always-available
//! SoMultipleCopy node, whose matrix field it uses identically.

#pragma once

#include <vector>

#include <Gui/ViewProviderDocumentObject.h>

#include "../SWDWGGlobal.h"

class SoCoordinate3;
class SoIndexedLineSet;
class SoLightModel;
class SoMultipleCopy;
class SoSeparator;

namespace SWDWG
{

class SWDWGGuiExport ViewProviderMultiInstanceTest
    : public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::ViewProviderMultiInstanceTest);

public:
    ViewProviderMultiInstanceTest();
    ~ViewProviderMultiInstanceTest() override;

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* ModeName) override;

protected:
    void onChanged(const App::Property* prop) override;

private:
    //! Rebuild the Rows x Cols instance matrices from the App properties.
    void rebuildMatrices();
    //! Rebuild the shared template geometry (per Template property).
    void rebuildTemplate();

    SoMultipleCopy* pInstances = nullptr;
    SoSeparator* pRoot = nullptr;
};

}  // namespace SWDWG
