//! Demo view provider for SoMultipleCopy instancing - implementation.

#include "PreCompiled.h"

#include <cstring>
#include <cmath>
#include <vector>

#include <Inventor/SbMatrix.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoMultipleCopy.h>
#include <Inventor/nodes/SoSeparator.h>

#include <App/PropertyStandard.h>

#include "ViewProviderMultiInstanceTest.h"
#include "../App/MultiInstanceTest.h"

using namespace SWDWG;

// SoMultipleInstance (the faster subclass) is NOT present in every Coin3D /
// Inventor build; SoMultipleCopy is always available and exposes the same
// "matrix" field API that this demo feeds.  Use only SoMultipleCopy here so
// the file compiles everywhere FreeCAD builds.

PROPERTY_SOURCE(SWDWG::ViewProviderMultiInstanceTest,
                Gui::ViewProviderDocumentObject)

ViewProviderMultiInstanceTest::ViewProviderMultiInstanceTest()
{
    sPixmap = "Tree_Annotation";
}

ViewProviderMultiInstanceTest::~ViewProviderMultiInstanceTest()
{
    if (pInstances) {
        pInstances->unref();
    }
    if (pRoot) {
        pRoot->unref();
    }
}

void ViewProviderMultiInstanceTest::attach(App::DocumentObject* f)
{
    ViewProviderDocumentObject::attach(f);

    // Scene:  BASE_COLOR light -> color -> ONE SoMultipleCopy whose single
    // child is the (unit-size) template geometry.  The instance matrices are
    // pushed into the node below, so the GPU / Coin draws the template once
    // per matrix without per-instance scene-graph branches.
    auto* light = new SoLightModel();
    light->model = SoLightModel::BASE_COLOR;
    auto* color = new SoBaseColor();
    color->rgb.setValue(0.9f, 0.9f, 0.2f);

    pInstances = new SoMultipleCopy();
    pInstances->ref();
    // SoMultipleCopy draws its children once per matrix entry.
    auto* templateSep = new SoSeparator();
    auto* coords = new SoCoordinate3();
    auto* lines = new SoIndexedLineSet();
    templateSep->addChild(coords);
    templateSep->addChild(lines);
    pInstances->addChild(templateSep);

    pRoot = new SoSeparator();
    pRoot->ref();
    pRoot->addChild(light);
    pRoot->addChild(color);
    pRoot->addChild(pInstances);
    pRoot->renderCaching = SoSeparator::ON;
    pRoot->boundingBoxCaching = SoSeparator::ON;
    addDisplayMaskMode(pRoot, "3D");

    rebuildTemplate();
    rebuildMatrices();
}

void ViewProviderMultiInstanceTest::rebuildTemplate()
{
    auto* obj = dynamic_cast<MultiInstanceTest*>(pcObject);
    if (!obj || !pInstances || pInstances->getNumChildren() < 1) {
        return;
    }
    auto* templateSep = static_cast<SoSeparator*>(pInstances->getChild(0));
    SoCoordinate3* coords = nullptr;
    SoIndexedLineSet* lines = nullptr;
    for (int i = 0; i < templateSep->getNumChildren(); ++i) {
        SoNode* n = templateSep->getChild(i);
        if (n->getTypeId() == SoCoordinate3::getClassTypeId()) {
            coords = static_cast<SoCoordinate3*>(n);
        }
        else if (n->getTypeId() == SoIndexedLineSet::getClassTypeId()) {
            lines = static_cast<SoIndexedLineSet*>(n);
        }
    }
    if (!coords || !lines) {
        return;
    }

    const int kind = obj->Template.getValue();

    // Unit-size outline templates centred at the origin (y-up).
    std::vector<SbVec3f> pts;
    std::vector<int32_t> idx;
    auto close = [&](int first) {
        idx.push_back(first);
        idx.push_back(-1);
    };

    if (kind == 1) {  // plus sign: horizontal + vertical stroke
        pts.emplace_back(-1.0f, 0.0f, 0.0f);
        pts.emplace_back(1.0f, 0.0f, 0.0f);
        pts.emplace_back(0.0f, -1.0f, 0.0f);
        pts.emplace_back(0.0f, 1.0f, 0.0f);
        idx = {0, 1, -1, 2, 3, -1};
    }
    else if (kind == 2) {  // octagon disc outline
        static const int N = 8;
        for (int k = 0; k < N; ++k) {
            double a = 2.0 * M_PI * k / N;
            pts.emplace_back(static_cast<float>(std::cos(a)),
                             static_cast<float>(std::sin(a)), 0.0f);
        }
        for (int k = 0; k < N; ++k) {
            idx.push_back(k);
            idx.push_back((k + 1) % N);
            idx.push_back(-1);
        }
    }
    else {  // diamond outline (default)
        pts.emplace_back(0.0f, 1.0f, 0.0f);
        pts.emplace_back(1.0f, 0.0f, 0.0f);
        pts.emplace_back(0.0f, -1.0f, 0.0f);
        pts.emplace_back(-1.0f, 0.0f, 0.0f);
        idx.push_back(0);
        idx.push_back(1);
        idx.push_back(-1);
        idx.push_back(1);
        idx.push_back(2);
        idx.push_back(-1);
        idx.push_back(2);
        idx.push_back(3);
        idx.push_back(-1);
        idx.push_back(3);
        idx.push_back(0);
        close(0);
    }

    coords->point.setNum(static_cast<int>(pts.size()));
    coords->point.setValues(0, static_cast<int>(pts.size()), pts.data());
    lines->coordIndex.setNum(static_cast<int>(idx.size()));
    lines->coordIndex.setValues(0, static_cast<int>(idx.size()),
                                idx.data());
}

void ViewProviderMultiInstanceTest::rebuildMatrices()
{
    auto* obj = dynamic_cast<MultiInstanceTest*>(pcObject);
    if (!obj || !pInstances) {
        return;
    }
    int rows = obj->Rows.getValue();
    int cols = obj->Cols.getValue();
    float spacing = obj->Spacing.getValue();
    if (rows < 1) {
        rows = 1;
    }
    if (cols < 1) {
        cols = 1;
    }
    if (!(spacing > 0.0f)) {
        spacing = 10.0f;
    }
    // Clamp the demo so a runaway edit cannot build a gigantic array.
    const int maxInstances = 100000;
    long total = static_cast<long>(rows) * cols;
    if (total > maxInstances) {
        total = maxInstances;
    }

    std::vector<SbMatrix> mats;
    mats.reserve(static_cast<size_t>(total));
    // Marker ~35 % of the grid pitch.
    const float marker = spacing * 0.35f;
    // Centre the grid about the origin.
    const double offX = 0.5 * (cols - 1);
    const double offY = 0.5 * (rows - 1);

    int placed = 0;
    for (int r = 0; r < rows && placed < maxInstances; ++r) {
        for (int c = 0; c < cols && placed < maxInstances; ++c, ++placed) {
            SbMatrix s;
            s.setScale(SbVec3f(marker, marker, marker));
            SbMatrix t;
            t.setTranslate(SbVec3f(
                static_cast<float>((c - offX) * spacing),
                static_cast<float>((r - offY) * spacing), 0.0f));
            SbMatrix m = t;  // p' = T * (S * p)
            m.multRight(s);
            mats.push_back(m);
        }
    }

    pInstances->matrix.setNum(static_cast<int>(mats.size()));
    if (!mats.empty()) {
        pInstances->matrix.setValues(0, static_cast<int>(mats.size()),
                                     mats.data());
    }
}

void ViewProviderMultiInstanceTest::updateData(const App::Property* prop)
{
    if (prop) {
        const char* n = prop->getName();
        if (std::strcmp(n, "Template") == 0) {
            rebuildTemplate();
        }
        else if (std::strcmp(n, "Rows") == 0 || std::strcmp(n, "Cols") == 0
                 || std::strcmp(n, "Spacing") == 0) {
            rebuildMatrices();
        }
    }
    if (pRoot) {
        pRoot->touch();
    }
    ViewProviderDocumentObject::updateData(prop);
}

void ViewProviderMultiInstanceTest::onChanged(const App::Property* prop)
{
    ViewProviderDocumentObject::onChanged(prop);
}

std::vector<std::string> ViewProviderMultiInstanceTest::getDisplayModes()
    const
{
    return {"3D"};
}

void ViewProviderMultiInstanceTest::setDisplayMode(const char* ModeName)
{
    if (std::strcmp(ModeName, "3D") == 0) {
        setDisplayMaskMode("3D");
    }
    ViewProviderDocumentObject::setDisplayMode(ModeName);
}
