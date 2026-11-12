#include "PreCompiled.h"

#include "ViewProviderDWGLineGroup.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoGroup.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoSeparator.h>

#include <App/Application.h>
#include <App/Document.h>

#include <Base/Parameter.h>

#include <Gui/Selection/SoFCSelection.h>

#include <Mod/Part/App/PartFeature.h>

#include "../App/DWGLineGroup.h"
#include "../App/DWGLineType.h"

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Line.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

using namespace SWDWG;

PROPERTY_SOURCE(SWDWG::ViewProviderDWGLineGroup,
                Gui::ViewProviderDocumentObject)

ViewProviderDWGLineGroup::ViewProviderDWGLineGroup()
{
    sPixmap = "Tree_Annotation";
}

ViewProviderDWGLineGroup::~ViewProviderDWGLineGroup()
{
    if (pRoot) {
        pRoot->unref();
    }
}

bool ViewProviderDWGLineGroup::restoring() const
{
    if (pcObject && pcObject->isRestoring()) {
        return true;
    }
    return App::Document::isAnyRestoring();
}

bool ViewProviderDWGLineGroup::importing() const
{
    return pcObject && pcObject->getDocument()
        && pcObject->getDocument()->testStatus(App::Document::Status::Importing);
}

void ViewProviderDWGLineGroup::attach(App::DocumentObject* f)
{
    ViewProviderDocumentObject::attach(f);

    auto* sel = new Gui::SoFCSelection();
    sel->objectName = pcObject->getNameInDocument();
    sel->documentName = pcObject->getDocument()->getName();
    sel->subElementName = "Main";

    // Flat shading: the DWG geometry already carries the resolved colour; we
    // do not want Gouraud lighting to wash it out, and flat BASE_COLOR needs
    // no normals on the face set.
    auto* light = new SoLightModel();
    light->model = SoLightModel::BASE_COLOR;
    sel->addChild(light);

    pColor = new SoBaseColor();
    sel->addChild(pColor);

    pGeom = new SoSeparator();
    sel->addChild(pGeom);

    pRoot = new SoSeparator();
    pRoot->ref();
    pRoot->addChild(sel);
    // Static geometry: freeze into display lists so camera moves / selection
    // highlights do not re-tessellate the tens-of-thousands-edge shapes.
    pRoot->renderCaching = SoSeparator::ON;
    pRoot->boundingBoxCaching = SoSeparator::ON;
    addDisplayMaskMode(pRoot, "3D");

    auto* g = dynamic_cast<DWGLineGroup*>(f);
    if (g && pColor) {
        const Base::Color& c = g->DWGColor.getValue();
        pColor->rgb.setValue(c.r, c.g, c.b);
    }
    if (!restoring() && !importing()) {
        rebuildGeometry();
    }
}

void ViewProviderDWGLineGroup::rebuildGeometry()
{
    if (!pGeom || !pColor) {
        return;
    }
    auto* g = dynamic_cast<DWGLineGroup*>(pcObject);
    if (!g) {
        return;
    }
    const Base::Color& c = g->DWGColor.getValue();
    pColor->rgb.setValue(c.r, c.g, c.b);

    while (pGeom->getNumChildren() > 0) {
        pGeom->removeChild(pGeom->getChild(0));
    }

    // Direction-2 display: the group's persisted Shape is the RAW source
    // geometry (Dir2RawShape).  Dashes are produced here on the fly from
    // DWGSourceShape + the linked linetype definition, so the cut geometry is
    // never materialised into Shape / the file / PartGui.
    TopoDS_Shape shape = g->DWGSourceShape.getValue();
    if (shape.IsNull()) {
        shape = g->Shape.getValue();
    }
    const TopoDS_Shape rawShape = shape;
    if (shape.IsNull()) {
        return;
    }

    // LOD budget (P2-lite): when a dash pattern explodes into an impractical
    // number of segments (very long edges vs. a short pattern), rendering
    // every dash is slow no matter how the nodes are built, so such groups are
    // drawn with the raw (solid) shape.
    //
    // The budget MUST be checked before cutting, and so must the result: the
    // previous order cut first and discarded afterwards, which meant a group
    // already far past the budget (hundreds of thousands of edges, e.g. a
    // terrain layer) paid for the full, very expensive expansion only for the
    // result to be thrown away again.
    static const int capEdges = []() {
        const Base::Reference<ParameterGrp> hGrp =
            App::GetApplication().GetParameterGroupByPath(
                "User parameter:BaseApp/Preferences/Mod/SWDWG");
        return hGrp ? hGrp->GetInt("Dir2DashEdgeCap", 200000) : 200000;
    }();
    auto withinBudget = [](const TopoDS_Shape& s) {
        int n = 0;
        for (TopExp_Explorer ce(s, TopAbs_EDGE); ce.More(); ce.Next()) {
            if (++n > capEdges) {
                return false;
            }
        }
        return true;
    };

    if (withinBudget(rawShape)) {
        if (App::DocumentObject* ref = g->DWGLineTypeRef.getValue()) {
            if (auto* lt = dynamic_cast<SWDWG::DWGLineType*>(ref)) {
                if (!lt->dashes().empty()) {
                    // The definition supplies the pattern; the scale belongs to
                    // the line.  DWGLineScale is this group's own import-time
                    // scale (LTSCALE x this entity's linetype scale) and the
                    // definition contributes only a shared multiplier, so
                    // groups sharing a linetype keep their own dash spacing.
                    const double scale = effectiveLineScale(
                        g->DWGLineScale.getValue(),
                        lt->ScaleOverride.getValue());
                    try {
                        TopoDS_Shape cut = expandShapeByPattern(
                            rawShape, lt->dashes(), scale, lt->patternLen());
                        if (!cut.IsNull() && withinBudget(cut)) {
                            shape = cut;
                        }
                    }
                    catch (const Standard_Failure&) {
                        // keep the raw shape on cutter failure
                    }
                }
            }
        }
    }

    // --- edges as a single polyline set --------------------------------
    std::vector<SbVec3f> epts;
    std::vector<int32_t> eidx;
    TopExp_Explorer ex(shape, TopAbs_EDGE);
    for (; ex.More(); ex.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
        Standard_Real first = 0.0;
        Standard_Real last = 0.0;
        Handle(Geom_Curve) crv = BRep_Tool::Curve(edge, first, last);
        if (crv.IsNull()) {
            continue;
        }
        const bool linear = crv->IsKind(STANDARD_TYPE(Geom_Line));
        const int n = linear ? 2 : 25;  // arc / spline samples
        const int base = static_cast<int>(epts.size());
        for (int i = 0; i < n; ++i) {
            const double t = first + (last - first) * i / (n - 1);
            gp_Pnt p;
            crv->D0(t, p);
            epts.emplace_back(static_cast<float>(p.X()),
                              static_cast<float>(p.Y()),
                              static_cast<float>(p.Z()));
        }
        for (int i = 0; i < n; ++i) {
            eidx.push_back(base + i);
        }
        eidx.push_back(-1);
    }
    if (!epts.empty()) {
        auto* coords = new SoCoordinate3();
        auto* lines = new SoIndexedLineSet();
        coords->point.setNum(static_cast<int>(epts.size()));
        coords->point.setValues(0, static_cast<int>(epts.size()), epts.data());
        lines->coordIndex.setNum(static_cast<int>(eidx.size()));
        lines->coordIndex.setValues(0, static_cast<int>(eidx.size()),
                                    eidx.data());
        pGeom->addChild(coords);
        pGeom->addChild(lines);
    }

    // --- faces (triangulated) when the group contains surfaces ---------
    int faceCount = 0;
    for (TopExp_Explorer fe(shape, TopAbs_FACE); fe.More(); fe.Next()) {
        ++faceCount;
    }
    if (faceCount == 0) {
        return;
    }
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    double diag = 0.0;
    if (!box.IsVoid()) {
        const gp_Pnt mn = box.CornerMin();
        const gp_Pnt mx = box.CornerMax();
        diag = mn.Distance(mx);
    }
    const double deflection =
        diag > 0.0 ? std::max(1e-3, diag * 1e-3) : 0.1;
    BRepMesh_IncrementalMesh mesher(shape, deflection);

    std::vector<SbVec3f> fpts;
    std::vector<int32_t> fidx;
    TopExp_Explorer fe2(shape, TopAbs_FACE);
    for (; fe2.More(); fe2.Next()) {
        const TopoDS_Face& face = TopoDS::Face(fe2.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull() || tri->NbNodes() < 3) {
            continue;
        }
        const int offset = static_cast<int>(fpts.size());
        for (Standard_Integer i = 1; i <= tri->NbNodes(); ++i) {
            gp_Pnt p = tri->Node(i);
            p.Transform(loc.Transformation());
            fpts.emplace_back(static_cast<float>(p.X()),
                              static_cast<float>(p.Y()),
                              static_cast<float>(p.Z()));
        }
        for (Standard_Integer t = 1; t <= tri->NbTriangles(); ++t) {
            Standard_Integer n1 = 0, n2 = 0, n3 = 0;
            tri->Triangle(t).Get(n1, n2, n3);
            fidx.push_back(offset + n1 - 1);
            fidx.push_back(offset + n2 - 1);
            fidx.push_back(offset + n3 - 1);
            fidx.push_back(-1);
        }
    }
    if (!fpts.empty() && !fidx.empty()) {
        auto* coords = new SoCoordinate3();
        auto* faces = new SoIndexedFaceSet();
        coords->point.setNum(static_cast<int>(fpts.size()));
        coords->point.setValues(0, static_cast<int>(fpts.size()), fpts.data());
        faces->coordIndex.setNum(static_cast<int>(fidx.size()));
        faces->coordIndex.setValues(0, static_cast<int>(fidx.size()),
                                    fidx.data());
        pGeom->addChild(coords);
        pGeom->addChild(faces);
    }
}

void ViewProviderDWGLineGroup::updateData(const App::Property* prop)
{
    if (prop && !restoring() && !importing()) {
        const char* n = prop->getName();
        if (n && (std::strcmp(n, "Shape") == 0
                  || std::strcmp(n, "DWGColor") == 0
                  || std::strcmp(n, "DWGSourceShape") == 0
                  || std::strcmp(n, "DWGLineTypeRef") == 0
                  || std::strcmp(n, "DWGLineScale") == 0)) {
            rebuildGeometry();
        }
    }
    if (pRoot) {
        pRoot->touch();
    }
    ViewProviderDocumentObject::updateData(prop);
}

void ViewProviderDWGLineGroup::finishRestoring()
{
    if (!importing()) {
        rebuildGeometry();
    }
    ViewProviderDocumentObject::finishRestoring();
}

void ViewProviderDWGLineGroup::onChanged(const App::Property* prop)
{
    ViewProviderDocumentObject::onChanged(prop);
}

std::vector<std::string> ViewProviderDWGLineGroup::getDisplayModes() const
{
    return {"3D"};
}

void ViewProviderDWGLineGroup::setDisplayMode(const char* ModeName)
{
    if (strcmp(ModeName, "3D") == 0) {
        setDisplayMaskMode("3D");
    }
    ViewProviderDocumentObject::setDisplayMode(ModeName);
}
