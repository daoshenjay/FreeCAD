#include "PreCompiled.h"

#include <cctype>
#include <cmath>
#include <cstring>

#include <Inventor/nodes/SoAsciiText.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoFont.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoNormal.h>
#include <Inventor/nodes/SoNormalBinding.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoTransform.h>

#include <App/Document.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Base/Console.h>
#include <Base/Vector3D.h>

#include <Gui/Selection/SoFCSelection.h>

#include "ViewProviderDWGDimension.h"
#include "../App/DWGDimension.h"
#include "../App/TextShxRenderer.h"

using namespace SWDWG;

namespace
{

//! True when a font / big-font file name is a classic AutoCAD .shx.
bool isShxFontName(const std::string& s)
{
    const std::string::size_type dot = s.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = s.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower((unsigned char)c));
    }
    return ext == "shx";
}

}  // namespace

PROPERTY_SOURCE(SWDWG::ViewProviderDWGDimension, Gui::ViewProviderDocumentObject)

ViewProviderDWGDimension::ViewProviderDWGDimension()
{
    pDimColor = new SoBaseColor();
    pDimColor->ref();
    pLineGroup = new SoSeparator();
    pLineGroup->ref();
    pDimCoords = new SoCoordinate3();
    pDimCoords->ref();
    pDimLines = new SoIndexedLineSet();
    pDimLines->ref();
    pArrowCoords = new SoCoordinate3();
    pArrowCoords->ref();
    pArrowNormalBinding = new SoNormalBinding();
    pArrowNormalBinding->value = SoNormalBinding::PER_FACE;
    pArrowNormalBinding->ref();
    pArrows = new SoIndexedFaceSet();
    pArrows->ref();
    pArrowNormals = new SoNormal();
    pArrowNormals->ref();
    pLineLightModel = new SoLightModel();
    pLineLightModel->model = SoLightModel::BASE_COLOR;
    pLineLightModel->ref();
    pExtColor = new SoBaseColor();
    pExtColor->ref();
    pExtCoords = new SoCoordinate3();
    pExtCoords->ref();
    pExtLines = new SoIndexedLineSet();
    pExtLines->ref();
    pTextColor = new SoBaseColor();
    pTextColor->ref();
    pFont = new SoFont();
    pFont->ref();
    pTextTransform = new SoTransform();
    pTextTransform->ref();
    pText = new SoAsciiText();
    pText->ref();
    pShxTextGroup = new SoSeparator();
    pShxTextGroup->ref();

    sPixmap = "Tree_Annotation";
}

ViewProviderDWGDimension::~ViewProviderDWGDimension()
{
    pDimColor->unref();
    pLineGroup->unref();
    pDimCoords->unref();
    pDimLines->unref();
    pArrowCoords->unref();
    pArrowNormalBinding->unref();
    pArrows->unref();
    pArrowNormals->unref();
    pLineLightModel->unref();
    pExtColor->unref();
    pExtCoords->unref();
    pExtLines->unref();
    pTextColor->unref();
    pFont->unref();
    pTextTransform->unref();
    pText->unref();
    pShxTextGroup->unref();
}

void ViewProviderDWGDimension::attach(App::DocumentObject* f)
{
    ViewProviderDocumentObject::attach(f);

    auto* sel = new Gui::SoFCSelection();
    sel->objectName = pcObject->getNameInDocument();
    sel->documentName = pcObject->getDocument()->getName();
    sel->subElementName = "Main";

    // Dimension line + arrows (DIMCLRD) -> extension lines (DIMCLRE) ->
    // text (DIMCLRT), each in its own colour group.
    sel->addChild(pDimColor);
    // Render dimension lines and arrow heads with flat (unlit) colour so the
    // two arrow heads look identical regardless of their triangle winding.
    pLineGroup->addChild(pLineLightModel);
    pLineGroup->addChild(pDimCoords);
    pLineGroup->addChild(pDimLines);
    pLineGroup->addChild(pArrowNormalBinding);
    pLineGroup->addChild(pArrowCoords);
    pLineGroup->addChild(pArrows);
    sel->addChild(pLineGroup);
    sel->addChild(pExtColor);
    sel->addChild(pExtCoords);
    sel->addChild(pExtLines);
    sel->addChild(pTextColor);
    sel->addChild(pFont);
    sel->addChild(pTextTransform);
    sel->addChild(pShxTextGroup);
    sel->addChild(pText);

    // Freeze the display list like the DWG text provider, otherwise every
    // view interaction re-tessellates the arrow faces and re-harvests the
    // glyphs.
    pRoot = new SoSeparator();
    pRoot->addChild(sel);
    pRoot->renderCaching = SoSeparator::ON;
    pRoot->boundingBoxCaching = SoSeparator::ON;
    addDisplayMaskMode(pRoot, "3D");

    // During a bulk import addObject() runs while the importer already holds
    // the bulk window open; the properties are not filled yet, so the scene
    // graph would be built empty only to be rebuilt 15 times as FreeCAD
    // touches each property.  Skip it - the importer's closing
    // DimText.touch() triggers the one rebuild that carries real data.
    // A .fcstd restore writes the properties back one by one with no bulk
    // window open; those rebuilds are deferred to finishRestoring() as well.
    if (!deferHeavyRebuild()) {
        rebuildGeometry("attach");
    }
}

bool ViewProviderDWGDimension::deferHeavyRebuild()
{
    if (DWGDimension::isBulkCreating()) {
        return true;  // importer window: filled after leaveBulkCreate()
    }
    if (isRestoring()) {  // Gui::isRestoring status bit
        return true;
    }
    const App::DocumentObject* o = pcObject;
    return o && (o->isRestoring() || App::Document::isAnyRestoring());
}

void ViewProviderDWGDimension::finishRestoring()
{
    // All App + view properties are restored now; rebuild the geometry scene
    // graph a single time instead of once per property during updateData.
    if (pcObject) {
        rebuildGeometry("restore");
    }
    ViewProviderDocumentObject::finishRestoring();
}

void ViewProviderDWGDimension::rebuildGeometry(const char* why)
{
    auto* dim = dynamic_cast<DWGDimension*>(pcObject);
    if (!dim) {
        return;
    }

    // ---- dimension line / arc / leader segments -------------------------
    const std::vector<Base::Vector3d>& dimLines = dim->DimLines.getValues();
    std::vector<SbVec3f> dimPts;
    std::vector<int32_t> dimIdx;
    dimPts.reserve(dimLines.size());
    dimIdx.reserve(dimLines.size() + dimLines.size() / 2);
    for (size_t i = 0; i + 1 < dimLines.size(); i += 2) {
        int32_t a = static_cast<int32_t>(dimPts.size());
        dimPts.emplace_back(static_cast<float>(dimLines[i].x),
                            static_cast<float>(dimLines[i].y),
                            static_cast<float>(dimLines[i].z));
        dimPts.emplace_back(static_cast<float>(dimLines[i + 1].x),
                            static_cast<float>(dimLines[i + 1].y),
                            static_cast<float>(dimLines[i + 1].z));
        dimIdx.push_back(a);
        dimIdx.push_back(a + 1);
        dimIdx.push_back(-1);
    }
    pDimCoords->point.setNum(static_cast<int>(dimPts.size()));
    if (!dimPts.empty()) {
        pDimCoords->point.setValues(0, static_cast<int>(dimPts.size()),
                                    dimPts.data());
    }
    pDimLines->coordIndex.setNum(static_cast<int>(dimIdx.size()));
    if (!dimIdx.empty()) {
        pDimLines->coordIndex.setValues(0, static_cast<int>(dimIdx.size()),
                                        dimIdx.data());
    }

    // ---- arrow head triangles -------------------------------------------
    const std::vector<Base::Vector3d>& arrows = dim->ArrowTriangles.getValues();
    std::vector<SbVec3f> arrowPts;
    std::vector<int32_t> faceIdx;
    std::vector<SbVec3f> faceNormals;
    arrowPts.reserve(arrows.size());
    faceIdx.reserve(arrows.size() + arrows.size() / 3);
    faceNormals.reserve(arrows.size() / 3);
    for (size_t i = 0; i + 2 < arrows.size(); i += 3) {
        SbVec3f p0(static_cast<float>(arrows[i].x),
                   static_cast<float>(arrows[i].y),
                   static_cast<float>(arrows[i].z));
        SbVec3f p1(static_cast<float>(arrows[i + 1].x),
                   static_cast<float>(arrows[i + 1].y),
                   static_cast<float>(arrows[i + 1].z));
        SbVec3f p2(static_cast<float>(arrows[i + 2].x),
                   static_cast<float>(arrows[i + 2].y),
                   static_cast<float>(arrows[i + 2].z));
        SbVec3f n = (p1 - p0).cross(p2 - p0);
        if (n.length() > 1e-6f) {
            n.normalize();
        }
        else {
            n.setValue(0.0f, 0.0f, 1.0f);
        }
        faceNormals.push_back(n);
        int32_t base = static_cast<int32_t>(arrowPts.size());
        arrowPts.push_back(p0);
        arrowPts.push_back(p1);
        arrowPts.push_back(p2);
        faceIdx.push_back(base);
        faceIdx.push_back(base + 1);
        faceIdx.push_back(base + 2);
        faceIdx.push_back(-1);
    }
    pArrowCoords->point.setNum(static_cast<int>(arrowPts.size()));
    if (!arrowPts.empty()) {
        pArrowCoords->point.setValues(0, static_cast<int>(arrowPts.size()),
                                      arrowPts.data());
    }
    pArrows->coordIndex.setNum(static_cast<int>(faceIdx.size()));
    if (!faceIdx.empty()) {
        pArrows->coordIndex.setValues(0, static_cast<int>(faceIdx.size()),
                                      faceIdx.data());
    }
    pArrowNormals->vector.setNum(static_cast<int>(faceNormals.size()));
    if (!faceNormals.empty()) {
        pArrowNormals->vector.setValues(0, static_cast<int>(faceNormals.size()),
                                        faceNormals.data());
    }

    // ---- extension lines -------------------------------------------------
    const std::vector<Base::Vector3d>& extLines = dim->ExtLines.getValues();
    std::vector<SbVec3f> extPts;
    std::vector<int32_t> extIdx;
    extPts.reserve(extLines.size());
    extIdx.reserve(extLines.size() + extLines.size() / 2);
    for (size_t i = 0; i + 1 < extLines.size(); i += 2) {
        int32_t a = static_cast<int32_t>(extPts.size());
        extPts.emplace_back(static_cast<float>(extLines[i].x),
                            static_cast<float>(extLines[i].y),
                            static_cast<float>(extLines[i].z));
        extPts.emplace_back(static_cast<float>(extLines[i + 1].x),
                            static_cast<float>(extLines[i + 1].y),
                            static_cast<float>(extLines[i + 1].z));
        extIdx.push_back(a);
        extIdx.push_back(a + 1);
        extIdx.push_back(-1);
    }
    pExtCoords->point.setNum(static_cast<int>(extPts.size()));
    if (!extPts.empty()) {
        pExtCoords->point.setValues(0, static_cast<int>(extPts.size()),
                                    extPts.data());
    }
    pExtLines->coordIndex.setNum(static_cast<int>(extIdx.size()));
    if (!extIdx.empty()) {
        pExtLines->coordIndex.setValues(0, static_cast<int>(extIdx.size()),
                                        extIdx.data());
    }

    // ---- text ------------------------------------------------------------
    const Base::Vector3d tp = dim->TextPosition.getValue();
    const float rot = static_cast<float>(dim->TextRotation.getValue());
    const float th = static_cast<float>(dim->TextHeight.getValue());
    const float wf = dim->TextWidthFactor.getValue() > 0.0f
        ? dim->TextWidthFactor.getValue() : 1.0f;
    const std::string& str = dim->DimText.getValue();
    // SoAsciiText anchors the string at its baseline, but TextPosition is
    // AutoCAD's "middle point of dimension text" (the vertical centre of the
    // text box).  Shift the baseline down by 0.35 * textHeight along the
    // text-local up direction so the glyph centre lands on TextPosition.
    const float off = 0.35f * th;
    pTextTransform->translation.setValue(static_cast<float>(tp.x)
                                             + off * std::sin(rot),
                                         static_cast<float>(tp.y)
                                             - off * std::cos(rot),
                                         static_cast<float>(tp.z));
    pTextTransform->rotation.setValue(SbVec3f(0.0f, 0.0f, 1.0f), rot);
    pTextTransform->scaleFactor.setValue(wf, 1.0f, 1.0f);
    pFont->size = th;
    pText->justification = SoAsciiText::CENTER;

    // Classic .shx dimension text is stroked with the native single-line
    // glyphs (AutoCAD look); TrueType text keeps SoAsciiText.  The two text
    // nodes share pTextTransform, so only one is populated at a time.
    auto* dimObj = dynamic_cast<DWGDimension*>(pcObject);
    const std::string& fontFile = dimObj ? dimObj->FontFile.getValue()
                                         : std::string();
    const std::string& bigFontFile = dimObj ? dimObj->BigFontFile.getValue()
                                            : std::string();
    const int cp = dimObj ? dimObj->Codepage.getValue() : 0;
    bool shxOk = false;
    SWDWG_LOG(
        "[SWDWG DIMVP] '%s' text='%s' font='%s' bigfont='%s' cp=%d h=%.3f "
        "wf=%.3f why=%s\n",
        dimObj ? dimObj->Label.getValue() : "?", str.c_str(),
        fontFile.c_str(), bigFontFile.c_str(), cp, th, wf,
        why ? why : "-");
    if (!str.empty()
        && (isShxFontName(fontFile) || isShxFontName(bigFontFile))) {
        ShxLineRender r =
            renderShxLine(fontFile, bigFontFile, cp, str, th, wf);
        SWDWG_LOG(
            "[SWDWG DIMVP] renderShxLine ok=%d glyphs=%zu width=%.3f\n",
            (int)r.ok, r.glyphs.size(), r.width);
        if (r.ok && !r.glyphs.empty()) {
            // Centre the whole run on the text anchor: renderShxLine starts
            // at x=0, so shift everything left by half the run width (the
            // vertical 0.35*th baseline offset is already in pTextTransform).
            std::vector<SbVec3f> pts;
            std::vector<int32_t> idx;
            pts.reserve(r.glyphs.size() * 8);
            idx.reserve(r.glyphs.size() * 16);
            const double half = 0.5 * r.width;
            for (const auto& glyph : r.glyphs) {
                for (const ShapeSeg& s : glyph) {
                    if ((s.x1 - s.x0) * (s.x1 - s.x0)
                            + (s.y1 - s.y0) * (s.y1 - s.y0)
                        < 1e-12) {
                        continue;
                    }
                    const int32_t base = static_cast<int32_t>(pts.size());
                    pts.emplace_back(static_cast<float>(s.x0 - half),
                                     static_cast<float>(s.y0), 0.0f);
                    pts.emplace_back(static_cast<float>(s.x1 - half),
                                     static_cast<float>(s.y1), 0.0f);
                    idx.push_back(base);
                    idx.push_back(base + 1);
                    idx.push_back(-1);
                }
            }
            if (!pts.empty()) {
                while (pShxTextGroup->getNumChildren() > 0) {
                    pShxTextGroup->removeChild(
                        pShxTextGroup->getChild(0));
                }
                auto* light = new SoLightModel();
                light->model = SoLightModel::BASE_COLOR;
                auto* coords = new SoCoordinate3();
                auto* lines = new SoIndexedLineSet();
                coords->point.setNum(static_cast<int>(pts.size()));
                coords->point.setValues(0, static_cast<int>(pts.size()),
                                        pts.data());
                lines->coordIndex.setNum(static_cast<int>(idx.size()));
                lines->coordIndex.setValues(0, static_cast<int>(idx.size()),
                                            idx.data());
                pShxTextGroup->addChild(light);
                pShxTextGroup->addChild(coords);
                pShxTextGroup->addChild(lines);
                shxOk = true;
            }
        }
    }
    if (shxOk) {
        pText->string.setNum(0);  // hide the TrueType glyphs
    }
    else {
        while (pShxTextGroup->getNumChildren() > 0) {
            pShxTextGroup->removeChild(pShxTextGroup->getChild(0));
        }
        pText->string.setNum(1);
        pText->string.set1Value(0,
                                SbString(str.empty() ? " " : str.c_str()));
    }

    // ---- colours ----------------------------------------------------------
    const Base::Color& dc = dim->DimColor.getValue();
    pDimColor->rgb.setValue(dc.r, dc.g, dc.b);
    const Base::Color& ec = dim->ExtColor.getValue();
    pExtColor->rgb.setValue(ec.r, ec.g, ec.b);
    const Base::Color& tc = dim->TextColor.getValue();
    pTextColor->rgb.setValue(tc.r, tc.g, tc.b);
}

void ViewProviderDWGDimension::updateData(const App::Property* prop)
{
    // While the importer fills a fresh dimension object every property write
    // would otherwise trigger rebuildGeometry() (which also re-strokes the
    // .shx text) - ~15 rebuilds per dimension.  Defer them to the final
    // DimText.touch() the importer issues after closing the bulk window.
    // A .fcstd restore writes the properties back the same way, so the same
    // deferral applies; finishRestoring() performs the single rebuild.
    const bool bulk = deferHeavyRebuild();
    if (!bulk && prop) {
        const char* n = prop->getName();
        if (std::strcmp(n, "DimLines") == 0 || std::strcmp(n, "ExtLines") == 0
            || std::strcmp(n, "ArrowTriangles") == 0
            || std::strcmp(n, "TextPosition") == 0
            || std::strcmp(n, "TextRotation") == 0
            || std::strcmp(n, "DimText") == 0
            || std::strcmp(n, "TextHeight") == 0
            || std::strcmp(n, "TextWidthFactor") == 0
            || std::strcmp(n, "DimColor") == 0
            || std::strcmp(n, "ExtColor") == 0
            || std::strcmp(n, "TextColor") == 0
            // Font engine selection depends on these; a dimension whose style
            // font is set *after* attach() must rebuild to pick up the .shx
            // path (same ordering fix as the DWG text provider).
            || std::strcmp(n, "FontFile") == 0
            || std::strcmp(n, "BigFontFile") == 0
            || std::strcmp(n, "Codepage") == 0) {
            rebuildGeometry(n);
        }
    }
    if (pRoot) {
        pRoot->touch();
    }
    ViewProviderDocumentObject::updateData(prop);
}

std::vector<std::string> ViewProviderDWGDimension::getDisplayModes() const
{
    return {"3D"};
}

void ViewProviderDWGDimension::setDisplayMode(const char* ModeName)
{
    if (std::strcmp(ModeName, "3D") == 0) {
        setDisplayMaskMode("3D");
    }
    ViewProviderDocumentObject::setDisplayMode(ModeName);
}

void ViewProviderDWGDimension::onChanged(const App::Property* prop)
{
    ViewProviderDocumentObject::onChanged(prop);
}
