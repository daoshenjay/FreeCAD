#include "PreCompiled.h"

#include "DWGSurfaceMesh.h"

#include <vector>

using namespace SWDWG;

PROPERTY_SOURCE(SWDWG::DWGSurfaceMesh, App::GeoFeature)

DWGSurfaceMesh::DWGSurfaceMesh()
{
    ADD_PROPERTY_TYPE(
        Points,
        (),
        "DWG",
        App::Prop_None,
        "Shared vertices of the quad surface (model units)");
    ADD_PROPERTY_TYPE(
        FaceIndex,
        (),
        "DWG",
        App::Prop_None,
        "Flat quad index list: 4 Points indices per 3DFACE");
    ADD_PROPERTY_TYPE(Layer,
                      ("0"),
                      "DWG",
                      App::Prop_None,
                      "DWG layer name the entity lives on");
    ADD_PROPERTY_TYPE(EntityKind,
                      ("SURFACEMESH"),
                      "DWG",
                      App::Prop_ReadOnly,
                      "Imported entity kind: SURFACEMESH (tree grouping key)");
    ADD_PROPERTY_TYPE(MeshColor,
                      (1.0f, 1.0f, 1.0f),
                      "DWG",
                      App::Prop_None,
                      "Resolved face colour from the DWG entity/layer");
}

DWGSurfaceMesh::~DWGSurfaceMesh() = default;
