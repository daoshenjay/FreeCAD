#include "PreCompiled.h"

#include "ViewProviderDWGSurfaceMesh.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoVertexProperty.h>

#include <App/Document.h>

#include <Gui/Selection/SoFCSelection.h>

#include "../App/DWGSurfaceMesh.h"

using namespace SWDWG;

PROPERTY_SOURCE(SWDWG::ViewProviderDWGSurfaceMesh,
                Gui::ViewProviderDocumentObject)

ViewProviderDWGSurfaceMesh::ViewProviderDWGSurfaceMesh()
{
    sPixmap = "Tree_Annotation";
}

ViewProviderDWGSurfaceMesh::~ViewProviderDWGSurfaceMesh() = default;

bool ViewProviderDWGSurfaceMesh::restoring()
{
    if (pcObject && pcObject->isRestoring()) {
        return true;
    }
    return App::Document::isAnyRestoring();
}

bool ViewProviderDWGSurfaceMesh::importing()
{
    // While the importer is bulk-creating objects the GUI deliberately does
    // not receive per-property updates; the importer triggers one explicit
    // touch() after the Importing lock is released instead.
    return pcObject && pcObject->getDocument()
        && pcObject->getDocument()->testStatus(App::Document::Status::Importing);
}

void ViewProviderDWGSurfaceMesh::attach(App::DocumentObject* f)
{
    ViewProviderDocumentObject::attach(f);

    auto* sel = new Gui::SoFCSelection();
    sel->objectName = pcObject->getNameInDocument();
    sel->documentName = pcObject->getDocument()->getName();
    sel->subElementName = "Main";

    pMat = new SoMaterial();
    sel->addChild(pMat);

    pFaces = new SoIndexedFaceSet();
    sel->addChild(pFaces);

    pRoot = new SoSeparator();
    pRoot->addChild(sel);
    // Static geometry: freeze it into a display list so camera moves / the
    // selection highlight do not re-tessellate a multi-million-quad surface.
    pRoot->renderCaching = SoSeparator::ON;
    pRoot->boundingBoxCaching = SoSeparator::ON;
    addDisplayMaskMode(pRoot, "3D");

    auto* surf = dynamic_cast<DWGSurfaceMesh*>(f);
    if (surf) {
        const Base::Color& c = surf->MeshColor.getValue();
        pMat->diffuseColor.setValue(c.r, c.g, c.b);
    }
    if (!restoring() && !importing()) {
        rebuildMesh();
    }
}

void ViewProviderDWGSurfaceMesh::rebuildMesh()
{
    auto* surf = dynamic_cast<DWGSurfaceMesh*>(pcObject);
    if (!surf || !pFaces) {
        return;
    }
    const std::vector<Base::Vector3d>& pts = surf->Points.getValues();
    const std::vector<long>& idx = surf->FaceIndex.getValues();

    // Clear any previous content.
    pFaces->vertexProperty = nullptr;
    pFaces->coordIndex.setNum(0);
    const std::size_t quads = idx.size() / 4;
    if (pts.empty() || quads == 0 || idx.empty()) {
        return;
    }
    const int nVerts = static_cast<int>(pts.size());

    // Smooth per-vertex normals: average the two triangle normals of every
    // quad sharing the vertex (grid terrains look shaded instead of flat).
    std::vector<SbVec3f> normals(static_cast<std::size_t>(nVerts));
    std::vector<int32_t> ci;
    ci.reserve(quads * 8);

    auto addTri = [&](int a, int b, int c) {
        if (a < 0 || b < 0 || c < 0 || a >= nVerts || b >= nVerts
            || c >= nVerts) {
            return;
        }
        const Base::Vector3d& pa = pts[a];
        const Base::Vector3d& pb = pts[b];
        const Base::Vector3d& pc = pts[c];
        const double e1x = pb.x - pa.x;
        const double e1y = pb.y - pa.y;
        const double e1z = pb.z - pa.z;
        const double e2x = pc.x - pa.x;
        const double e2y = pc.y - pa.y;
        const double e2z = pc.z - pa.z;
        // Cross product; drop degenerate triangles (zero area).
        const double nx = e1y * e2z - e1z * e2y;
        const double ny = e1z * e2x - e1x * e2z;
        const double nz = e1x * e2y - e1y * e2x;
        const double len2 = nx * nx + ny * ny + nz * nz;
        if (len2 < 1e-20) {
            return;
        }
        const double inv = 1.0 / std::sqrt(len2);
        const float ux = static_cast<float>(nx * inv);
        const float uy = static_cast<float>(ny * inv);
        const float uz = static_cast<float>(nz * inv);
        normals[a] += SbVec3f(ux, uy, uz);
        normals[b] += SbVec3f(ux, uy, uz);
        normals[c] += SbVec3f(ux, uy, uz);
        ci.push_back(a);
        ci.push_back(b);
        ci.push_back(c);
        ci.push_back(-1);
    };

    for (std::size_t q = 0; q < quads; ++q) {
        const int a = static_cast<int>(idx[q * 4 + 0]);
        const int b = static_cast<int>(idx[q * 4 + 1]);
        const int c = static_cast<int>(idx[q * 4 + 2]);
        const int d = static_cast<int>(idx[q * 4 + 3]);
        addTri(a, b, c);
        // Triangular 3DFACEs repeat the last corner; skip the degenerate quad.
        if (d != c) {
            addTri(a, c, d);
        }
    }

    for (SbVec3f& n : normals) {
        if (n.length() > 1e-6f) {
            n.normalize();
        }
        else {
            n.setValue(0.0f, 0.0f, 1.0f);
        }
    }

    auto* vp = new SoVertexProperty();
    vp->vertex.setNum(nVerts);
    std::vector<SbVec3f> coords(static_cast<std::size_t>(nVerts));
    for (int i = 0; i < nVerts; ++i) {
        coords[static_cast<std::size_t>(i)] =
            SbVec3f(static_cast<float>(pts[i].x), static_cast<float>(pts[i].y),
                    static_cast<float>(pts[i].z));
    }
    vp->vertex.setValues(0, nVerts, coords.data());
    vp->normal.setNum(static_cast<int>(normals.size()));
    vp->normal.setValues(0, static_cast<int>(normals.size()), normals.data());
    vp->normalBinding = SoVertexProperty::PER_VERTEX_INDEXED;

    pFaces->vertexProperty = vp;
    pFaces->coordIndex.setNum(static_cast<int>(ci.size()));
    pFaces->coordIndex.setValues(0, static_cast<int>(ci.size()), ci.data());
}

void ViewProviderDWGSurfaceMesh::updateData(const App::Property* prop)
{
    auto* surf = dynamic_cast<DWGSurfaceMesh*>(pcObject);
    if (surf) {
        if (prop && (prop == &surf->Points || prop == &surf->FaceIndex)) {
            if (!restoring() && !importing()) {
                rebuildMesh();
            }
        }
        else if (prop && prop == &surf->MeshColor) {
            const Base::Color& c = surf->MeshColor.getValue();
            if (pMat) {
                pMat->diffuseColor.setValue(c.r, c.g, c.b);
            }
        }
    }
    ViewProviderDocumentObject::updateData(prop);
}

void ViewProviderDWGSurfaceMesh::finishRestoring()
{
    rebuildMesh();
    ViewProviderDocumentObject::finishRestoring();
}

std::vector<std::string> ViewProviderDWGSurfaceMesh::getDisplayModes() const
{
    return {"3D"};
}

void ViewProviderDWGSurfaceMesh::setDisplayMode(const char* ModeName)
{
    if (strcmp(ModeName, "3D") == 0) {
        setDisplayMaskMode("3D");
    }
    ViewProviderDocumentObject::setDisplayMode(ModeName);
}
