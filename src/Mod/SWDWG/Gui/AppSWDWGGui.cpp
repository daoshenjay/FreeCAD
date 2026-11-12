#include <Python.h>


#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <string>

#include <QCoreApplication>
#include <QProgressBar>
#include <QStatusBar>

#include <fastsignals/connection.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/PropertyStandard.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>
#include <Gui/Application.h>
#include <Gui/MainWindow.h>

#include "ViewProviderDWGLineGroup.h"
#include "ViewProviderDWGText.h"
#include "ViewProviderDWGSurfaceMesh.h"
#include "ViewProviderDWGDimension.h"
#include "ViewProviderMultiInstanceTest.h"

#include <CXX/Extensions.hxx>
#include <CXX/Objects.hxx>

// use a different name to CreateCommand()
void CreateSWDWGCommands(void);

// Defined in Command.cpp: apply the DWG appearance (color / linetype / line
// weight) stored on the imported Part::Features to their view providers.
void applyDWGStyles(App::Document* doc);

// Defined in ViewProviderDWGText.cpp: restore-time VP glyph-build timers.
void swdwg_resetOpenTimers();
double swdwg_vpRebuildSec();
double swdwg_vpShxSec();
int swdwg_vpRebuilds();
double swdwg_vpAttachSec();
int swdwg_vpAttaches();

//! Every recompute re-applies the DWG styles.  This is what makes editing a
//! shared DWGLineType definition (its onChanged touches the linked groups and
//! recomputes the document) update all lines using that linetype uniformly.
static void onDocumentRecomputed(const App::Document& doc)
{
    applyDWGStyles(const_cast<App::Document*>(&doc));
}

// ---------------------------------------------------------------------
// Progress bar shown in the main window's status bar while a saved .fcstd
// document (one that contains SWDWG objects) is re-opened with File->Open.
// FreeCAD drives the restore on the GUI thread; the document emits
// signalFinishRestoreObject once per restored object, which lets us advance
// a determinate progress bar.
//
// The bar only becomes visible when the restored document actually contains
// SWDWG objects (first object whose type name starts with "SWDWG::"), so
// opening a plain FreeCAD document stays silent.
// ---------------------------------------------------------------------
namespace
{

QProgressBar* s_openProgress = nullptr;  //!< status-bar progress (hidden until SWDWG found)
App::Document* s_openDocument = nullptr;          //!< document being restored
bool s_hasSwdwgObjects = false;                   //!< saw a SWDWG object yet?
int s_restoredObjects = 0;                        //!< objects finished so far
std::chrono::steady_clock::time_point s_openStart;  //!< restore start (timing)
std::chrono::steady_clock::time_point s_lastUi;   //!< last progress repaint
fastsignals::connection s_openObjectConn;          //!< per-document connection

//! True when the object is one of the SWDWG document types.
bool isSwdwgObject(const App::DocumentObject& obj)
{
    const std::string_view n = obj.getTypeId().getName();
    return n.size() >= 7 && n.compare(0, 7, "SWDWG::") == 0;
}

//! Per-type stats collected while a document is being restored (see
//! onRestoreObjectFinished).  A key like "DWGText-shx" lets us see whether the
//! restore seconds go into the stroked SHX glyph build, the TTF text path or
//! the generic FreeCAD object restore.
struct TypeStat
{
    int count = 0;
    double seconds = 0.0;
};
std::map<std::string, TypeStat> s_typeStats;
std::chrono::steady_clock::time_point s_lastObj;  // previous per-object tick

//! The slowest individual restore slices, so the per-type totals can be pinned
//! down to concrete objects (name + type) instead of just a type bucket.
struct SlowSlice
{
    double seconds = 0.0;
    std::string key;
    std::string name;
    std::string label;
};
std::vector<SlowSlice> s_slowSlices;

std::string restoreStatKey(const App::DocumentObject& obj)
{
    const std::string_view n = obj.getTypeId().getName();
    std::string name(n);
    if (name.compare(0, 7, "SWDWG::") == 0) {
        std::string t = name.substr(7);
        if (t == "DWGText") {
            if (const App::Property* p = obj.getPropertyByName("FontFile")) {
                if (p->isDerivedFrom(App::PropertyString::getClassTypeId())) {
                    std::string ff = static_cast<const App::PropertyString*>(
                                         p)
                                         ->getValue();
                    if (ff.size() >= 4) {
                        std::string ext = ff.substr(ff.size() - 4);
                        for (char& c : ext) {
                            c = static_cast<char>(
                                std::tolower(static_cast<unsigned char>(c)));
                        }
                        if (ext == ".shx") {
                            return "DWGText-shx";
                        }
                        return "DWGText-ttf";
                    }
                }
            }
            return "DWGText-other";
        }
        return t;  // DWGLineGroup / DWGLineType / DWGDimension / ...
    }
    return name;  // generic FreeCAD object (groups, Part features, ...)
}

//! Advanced by Document::signalFinishRestoreObject for every restored object.
void onRestoreObjectFinished(const App::DocumentObject& obj)
{
    if (!s_openProgress || !s_openDocument
        || obj.getDocument() != s_openDocument) {
        return;
    }
    // Per-object wall-clock attribution: each slice covers everything between
    // two finishRestoreObject signals (App property restore + any VP work that
    // happened in between).
    const auto nowTick = std::chrono::steady_clock::now();
    TypeStat& st = s_typeStats[restoreStatKey(obj)];
    ++st.count;
    const double slice =
        std::chrono::duration<double>(nowTick - s_lastObj).count();
    st.seconds += slice;
    s_lastObj = nowTick;
    if (slice > 0.02) {
        s_slowSlices.push_back({slice, restoreStatKey(obj),
                                obj.getNameInDocument() ? obj.getNameInDocument()
                                                        : "?",
                                obj.Label.getValue()});
    }

    if (!s_hasSwdwgObjects) {
        if (!isSwdwgObject(obj)) {
            return;  // wait until the document proves it is an SWDWG drawing
        }
        s_hasSwdwgObjects = true;
        if (QMainWindow* mw = Gui::getMainWindow()) {
            mw->setEnabled(false);  // freeze the UI while SWDWG objects restore
        }
        s_openProgress->show();
    }
    ++s_restoredObjects;
    const int denom = s_openDocument->getObjects().size();
    if (denom <= 0) {
        return;
    }
    // Throttle the repaint: one signal fires per restored object, and each
    // repaint + event pump measurably slows the restore down.  Refresh at
    // most every ~250 ms; the final update happens in onFinishRestoreDocument.
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - s_lastUi).count() < 0.25) {
        return;
    }
    s_lastUi = now;
    int pct = (int)(s_restoredObjects * 100.0 / denom);
    if (pct > 99) {
        pct = 99;  // stay under 100% until the restore is finished
    }
    s_openProgress->setValue(pct);
    s_openProgress->setFormat(
        QObject::tr("Opening FCStd: %p%  (%1 of %2 objects)")
            .arg(s_restoredObjects)
            .arg(denom));
    QCoreApplication::processEvents();
}

//! Called by Application::signalStartRestoreDocument (File->Open, restore).
void onStartRestoreDocument(const App::Document& doc)
{
    if (s_openProgress) {
        return;  // nested restore: keep the running bar
    }
    s_openDocument = const_cast<App::Document*>(&doc);
    s_hasSwdwgObjects = false;
    s_restoredObjects = 0;
    s_openStart = std::chrono::steady_clock::now();
    s_lastUi = s_openStart;
    s_typeStats.clear();
    s_slowSlices.clear();
    s_lastObj = s_openStart;
    swdwg_resetOpenTimers();
    // A lightweight non-modal progress bar parked in the status bar (no
    // pop-up dialog).  It stays hidden until the restored document proves to
    // contain SWDWG objects, so opening a plain FCStd file is silent.
    QMainWindow* mw = Gui::getMainWindow();
    if (!mw) {
        return;  // console start: no status bar to attach to
    }
    s_openProgress = new QProgressBar(mw->statusBar());
    s_openProgress->setRange(0, 100);
    s_openProgress->setValue(0);
    s_openProgress->setTextVisible(true);
    s_openProgress->setMinimumWidth(200);
    s_openProgress->hide();  // shown on the first SWDWG object
    mw->statusBar()->addWidget(s_openProgress, 1);
    s_openObjectConn = s_openDocument->signalFinishRestoreObject.connect(
        &onRestoreObjectFinished);
}

//! Called by Application::signalFinishRestoreDocument when the restore is done.
void onFinishRestoreDocument(const App::Document& doc)
{
    if (!s_openProgress || &doc != s_openDocument) {
        return;
    }
    // Always report the total wall-clock time of the restore (key operation
    // feedback - intentionally not gated by sw_debug).
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - s_openStart)
                      .count();
    QMainWindow* mw = Gui::getMainWindow();
    if (s_hasSwdwgObjects) {
        // Prefer the .fcstd file name (base name only) so multi-file timing
        // logs are distinguishable; fall back to the document's internal name.
        std::string name = doc.FileName.getValue();
        if (name.empty()) {
            const char* docName = doc.getName();
            name = docName ? docName : "?";
        }
        else {
            const std::size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) {
                name = name.substr(slash + 1);
            }
        }
        s_openProgress->setValue(100);
        s_openProgress->setFormat(QObject::tr("Document opened"));
        QCoreApplication::processEvents();
        Base::Console().message(
            "[SWDWG] FCStd '%s' opened in %.2f s (%d objects)\n", name.c_str(),
            secs, s_restoredObjects);
        for (const auto& kv : s_typeStats) {
            Base::Console().log(
                "[SWDWG]   open %s: %d obj(s), %.2f s\n",
                kv.first.c_str(), kv.second.count, kv.second.seconds);
        }
        Base::Console().log(
            "[SWDWG]   open VP rebuild: %d call(s), %.2f s "
            "(renderShx=%.2f s, other=%.2f s)\n",
            swdwg_vpRebuilds(), swdwg_vpRebuildSec(), swdwg_vpShxSec(),
            swdwg_vpRebuildSec() - swdwg_vpShxSec());
        Base::Console().log(
            "[SWDWG]   open VP attach: %d call(s), %.2f s\n",
            swdwg_vpAttaches(), swdwg_vpAttachSec());
        // Pin the expensive per-type buckets down to concrete objects.
        std::sort(s_slowSlices.begin(), s_slowSlices.end(),
                  [](const SlowSlice& a, const SlowSlice& b) {
                      return a.seconds > b.seconds;
                  });
        Base::Console().log(
            "[SWDWG]   slowest restore slices (>=20 ms): %d of %d objects\n",
            static_cast<int>(s_slowSlices.size()), s_restoredObjects);
        const std::size_t shown =
            std::min<std::size_t>(s_slowSlices.size(), 15);
        for (std::size_t i = 0; i < shown; ++i) {
            Base::Console().log("[SWDWG]     %.3f s  %-16s %-14s '%s'\n",
                                s_slowSlices[i].seconds,
                                s_slowSlices[i].key.c_str(),
                                s_slowSlices[i].name.c_str(),
                                s_slowSlices[i].label.c_str());
        }
        s_openObjectConn.disconnect();
        if (mw) {
            mw->setEnabled(true);  // release the UI we froze at the first SWDWG
            mw->statusBar()->showMessage(
                QObject::tr("Document '%1' opened in %2 s (%3 objects)")
                    .arg(QString::fromUtf8(name.c_str()))
                    .arg(secs, 0, 'f', 2)
                    .arg(s_restoredObjects),
                10000);
            mw->statusBar()->removeWidget(s_openProgress);
        }
        delete s_openProgress;
        s_openProgress = nullptr;
        s_openDocument = nullptr;
        s_hasSwdwgObjects = false;
        s_restoredObjects = 0;
        return;
    }
    s_openObjectConn.disconnect();
    if (mw) {
        mw->statusBar()->removeWidget(s_openProgress);
    }
    delete s_openProgress;
    s_openProgress = nullptr;
    s_openDocument = nullptr;
    s_hasSwdwgObjects = false;
    s_restoredObjects = 0;
}

}  // namespace


namespace SWDWGGui
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("SWDWGGui")
    {
        initialize("This module is the SWDWGGui module.");  // register with Python
    }

    virtual ~Module()
    {}

private:
};

PyObject* initModule()
{
    // Editing a shared DWGLineType definition recomputes the document; on
    // every recompute re-apply the DWG view styles so the whole drawing
    // reflects the change uniformly.
    App::GetApplication().signalRecomputed.connect(&onDocumentRecomputed);
    // Show a status-bar progress bar while a saved SWDWG .fcstd document is
    // re-opened with File->Open (see the anonymous namespace helpers above).
    App::GetApplication().signalStartRestoreDocument.connect(
        &onStartRestoreDocument);
    App::GetApplication().signalFinishRestoreDocument.connect(
        &onFinishRestoreDocument);
    return Base::Interpreter().addModule(new Module);
}

}  // namespace SWDWGGui


/* Python entry */
PyMOD_INIT_FUNC(SWDWGGui)
{
    if (!Gui::Application::Instance) {
        PyErr_SetString(PyExc_ImportError, "Cannot load Gui module in console application.");
        PyMOD_Return(0);
    }

    // instantiating the commands
    CreateSWDWGCommands();
    SWDWG::ViewProviderDWGLineGroup::init();
    SWDWG::ViewProviderDWGText::init();
    SWDWG::ViewProviderDWGSurfaceMesh::init();
    SWDWG::ViewProviderDWGDimension::init();
    SWDWG::ViewProviderMultiInstanceTest::init();

    // ADD YOUR CODE HERE
    //
    //
    PyObject* mod = SWDWGGui::initModule();
    SWDWG_LOG("Loading GUI of SWDWG module... done\n");
    PyMOD_Return(mod);
}
