#pragma once

#include <vector>

#include <Gui/ViewProviderDocumentObject.h>

#include "../SWDWGGlobal.h"

class SoIndexedFaceSet;
class SoMaterial;
class SoLightModel;
class SoSeparator;

namespace SWDWG
{

//! View provider for SWDWG::DWGSurfaceMesh (single-node quad rendering).
//!
//! A large 3DFACE import is stored as one object with a shared vertex list +
//! flat index list; this provider renders the WHOLE surface with a single
//! SoIndexedFaceSet instead of thousands/millions of per-face Coin nodes.
class SWDWGGuiExport ViewProviderDWGSurfaceMesh
    : public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::ViewProviderDWGSurfaceMesh);

public:
    ViewProviderDWGSurfaceMesh();
    ~ViewProviderDWGSurfaceMesh() override;

    void attach(App::DocumentObject*) override;
    void updateData(const App::Property*) override;
    void finishRestoring() override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* ModeName) override;

private:
    //! True while the surrounding document is being restored (the importer
    //! writes Points then FaceIndex while the object is created normally).
    bool restoring();
    //! True while the document is inside the importer's bulk-create window
    //! (GUI property updates are suppressed; the importer touches afterwards).
    bool importing();
    //! Rebuild the single SoIndexedFaceSet from Points / FaceIndex.
    void rebuildMesh();

    SoSeparator* pRoot = nullptr;
    SoMaterial* pMat = nullptr;
    SoIndexedFaceSet* pFaces = nullptr;
};

}  // namespace SWDWG
