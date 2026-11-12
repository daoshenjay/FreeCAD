//! Lightweight quad-surface holder for very large 3DFACE imports.
//!
//! Importing a terrain-like drawing where hundreds of thousands / millions of
//! 3DFACE entities all live on one layer as individual BRep faces is both slow
//! (per-face OCC + per-face Part display nodes) and memory hungry.  This
//! feature stores the same faces as ONE compact quad mesh (a shared vertex
//! list + a flat index list, 4 indices per quad).  Its custom view provider
//! renders the whole surface with a single SoIndexedFaceSet node, so the
//! import/open time and memory no longer scale with the face count.
//!
//! Geometry is read-only after import (edit by rebuilding the source DWG).
//! Scripts can still read every quad through Points / FaceIndex.

#pragma once

#include "DWGTypes.h"

#include <App/GeoFeature.h>
#include <App/PropertyGeo.h>
#include <App/PropertyStandard.h>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

class SWDWGExport DWGSurfaceMesh : public App::GeoFeature
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::DWGSurfaceMesh);

public:
    DWGSurfaceMesh();
    ~DWGSurfaceMesh() override;

    /// Map this App object to its GUI view provider.
    const char* getViewProviderName() const override
    {
        return "SWDWG::ViewProviderDWGSurfaceMesh";
    }

    /// Shared vertices (model units).  Adjacent quads share vertices, so a
    /// grid terrain of N quads stores ~N vertices instead of 4N.
    App::PropertyVectorList Points;
    /// Flat quad index list: every 4 entries are the Points indices of one
    /// 3DFACE corner (0..3, all four corners of a quad; tri-only faces repeat
    /// the last corner).
    App::PropertyIntegerList FaceIndex;
    /// DWG layer name.
    App::PropertyString Layer;
    /// Imported DWG entity kind (read-only, tree grouping key).
    App::PropertyString EntityKind;
    /// Resolved colour (entity > layer).
    App::PropertyColor MeshColor;
};

}  // namespace SWDWG
