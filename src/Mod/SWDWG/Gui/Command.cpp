#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QProgressBar>
#include <QStatusBar>

#include <cctype>
#include <chrono>
#include <string>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Base/Color.h>
#include <Base/Console.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/MainWindow.h>
#include <Gui/ViewProvider.h>

#include "../App/DWGImporter.h"
#include "../App/DWGShapeViewer.h"
#include "../App/MultiInstanceTest.h"

// ============================ 导出功能 BEGIN ============================
// DWG 导出：对象 -> Entity -> .dwg（见 App/DWGCompose.h / App/DWGWriter.h）
#include "../App/DWGCompose.h"
#include "../App/DWGWriter.h"

#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionObject.h>

#include <QDir>
#include <set>
// ============================ 导出功能 END ==============================


//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

//! Map a DWG linetype name to the Part view provider's DrawStyle enum.
static const char* mapLineType(const std::string& lt)
{
    std::string u;
    u.reserve(lt.size());
    for (char ch : lt) {
        u += (char)std::toupper((unsigned char)ch);
    }
    if (u.empty() || u.find("CONTINUOUS") != std::string::npos) {
        return "Solid";
    }
    if (u.find("CENTER") != std::string::npos) {
        return "Dashdot";
    }
    if (u.find("HIDDEN") != std::string::npos) {
        return "Dashed";
    }
    if (u.find("PHANTOM") != std::string::npos) {
        return "Dashdot";
    }
    if (u.find("DASH") != std::string::npos) {
        return u.find("DOT") != std::string::npos ? "Dashdot" : "Dashed";
    }
    if (u.find("DOT") != std::string::npos) {
        return "Dotted";
    }
    return "Solid";
}

//! Classify a quantized 16-bit line pattern (mapping level L1) into the
//! Part view provider's DrawStyle enum.  The importer computes the pattern
//! from the DWG LINETYPE table's dash list (see quantizeLinePattern in
//! DWGTypes.h), so this is a "pattern-driven" classification instead of the
//! old name substring guessing.  Runs of consecutive on-bits are analysed:
//! - 0xffff                     -> Solid
//! - all runs are single bits   -> Dotted
//! - single-bit dots + dashes   -> Dashdot
//! - otherwise                  -> Dashed
static const char* patternToStyle(unsigned short pattern)
{
    if (pattern == 0xffff) {
        return "Solid";
    }
    int maxRun = 0;
    int run = 0;
    int onRuns = 0;
    int oneBitRuns = 0;
    for (int i = 15; i >= 0; --i) {
        if (pattern & (1u << i)) {
            ++run;
        }
        else if (run > 0) {
            ++onRuns;
            if (run == 1) {
                ++oneBitRuns;
            }
            if (run > maxRun) {
                maxRun = run;
            }
            run = 0;
        }
    }
    if (run > 0) {  // trailing run (pattern wraps at bit 15/0)
        ++onRuns;
        if (run == 1) {
            ++oneBitRuns;
        }
        if (run > maxRun) {
            maxRun = run;
        }
    }
    if (onRuns == 0) {
        return "Solid";  // degenerate
    }
    if (maxRun == 1) {
        return "Dotted";  // isolated dots only
    }
    if (oneBitRuns > 0 && maxRun >= 2) {
        return "Dashdot";  // dots mixed with longer dashes
    }
    return "Dashed";
}

//! Map a DWG line weight (mm) to a pixel line width for the screen.
static int mapLineWidth(double mm)
{
    if (mm <= 0.25) {
        return 1;
    }
    if (mm <= 0.50) {
        return 2;
    }
    if (mm <= 1.00) {
        return 3;
    }
    if (mm <= 1.40) {
        return 4;
    }
    if (mm <= 2.00) {
        return 5;
    }
    return 6;
}

//! Apply the DWG appearance (color / linetype / line weight) that the
//! importer stored on each Part::Feature to its view provider, so the
//! displayed drawing matches the original DWG.
//!
//! Called after an import and again on every document recompute (connected in
//! AppSWDWGGui.cpp) so that editing a shared DWGLineType definition refreshes
//! all lines that use it at once.
void applyDWGStyles(App::Document* doc)
{
    for (auto* obj : doc->getObjects()) {
        if (!obj) {
            continue;
        }
        App::PropertyColor* colorProp = dynamic_cast<App::PropertyColor*>(
            obj->getPropertyByName("DWGColor"));
        if (!colorProp) {
            continue;
        }
        App::PropertyFloat* lwProp = dynamic_cast<App::PropertyFloat*>(
            obj->getPropertyByName("DWGLineWidth"));
        App::PropertyString* ltProp = dynamic_cast<App::PropertyString*>(
            obj->getPropertyByName("DWGLineType"));
        App::PropertyInteger* lpProp = dynamic_cast<App::PropertyInteger*>(
            obj->getPropertyByName("DWGLinePattern"));

        Gui::ViewProvider* vp = Gui::Application::Instance->getViewProvider(obj);
        if (!vp) {
            continue;
        }

        const Base::Color& c = colorProp->getValue();
        if (auto* p = dynamic_cast<App::PropertyColor*>(
                vp->getPropertyByName("LineColor"))) {
            p->setValue(c);
        }
        if (auto* p = dynamic_cast<App::PropertyColor*>(
                vp->getPropertyByName("PointColor"))) {
            p->setValue(c);
        }
        if (auto* p = dynamic_cast<App::PropertyColor*>(
                vp->getPropertyByName("ShapeColor"))) {
            p->setValue(c);
        }
        double mm = lwProp ? lwProp->getValue() : 0.25;
        if (auto* p = dynamic_cast<App::PropertyFloatConstraint*>(
                vp->getPropertyByName("LineWidth"))) {
            p->setValue(mapLineWidth(mm));
        }
        if (auto* p = dynamic_cast<App::PropertyEnumeration*>(
                vp->getPropertyByName("DrawStyle"))) {
            const std::string ltype = ltProp ? ltProp->getValue() : "";
            const char* styleName = nullptr;
            // L2: the geometry itself is already cut into world-unit ink
            // segments by DWGLineGroup::execute(), so the view must not
            // re-apply a dashed DrawStyle on top of them.  The shared
            // DWGLineTypeRef link marks a group as L2-processed.
            App::PropertyLink* lrProp = dynamic_cast<App::PropertyLink*>(
                obj->getPropertyByName("DWGLineTypeRef"));
            SWDWG_LOG(
                "[SWDWG STYLE] obj='%s' lrProp=%p ref=%p pattern=0x%x ltype='%s'\n",
                obj->getNameInDocument(), static_cast<void*>(lrProp),
                lrProp ? static_cast<void*>(lrProp->getValue()) : nullptr,
                lpProp ? lpProp->getValue() : -1,
                ltProp ? ltProp->getValue() : "");
            if (lrProp && lrProp->getValue()) {
                styleName = "Solid";
            }
            // L1: prefer the pattern quantized from the LINETYPE table.
            // 0xffff means CONTINUOUS (or a missing table entry), in which
            // case fall back to the name substring matching.
            else if (lpProp && lpProp->getValue() != 0xffff) {
                styleName = patternToStyle(
                    (unsigned short)lpProp->getValue());
            }
            if (!styleName) {
                styleName = mapLineType(ltype);
            }
            p->setValue(styleName);
        }
    }
    // Setting the view properties above marks the objects modified and
    // triggers a view update; no document touch is required.
}

//===========================================================================
// CmdSWDWGImport - import a DWG file
//===========================================================================
DEF_STD_CMD(CmdSWDWGImport)

CmdSWDWGImport::CmdSWDWGImport()
    : Command("SWDWG_Import")
{
    sAppModule = "SWDWG";
    sGroup = QT_TR_NOOP("SWDWG");
    sMenuText = QT_TR_NOOP("&Import DWG...");
    sToolTipText = QT_TR_NOOP(
        "Import a DWG file into a new document named after the DWG file");
    sWhatsThis = "SWDWG_Import";
    sStatusTip = QT_TR_NOOP(
        "Import a DWG file into a new document named after the DWG file");
    sPixmap = "SWDWGWorkbench";
    sAccel = "";
    // No document restriction: the import always creates its own document.
    eType = (CmdType)0;
}

void CmdSWDWGImport::activated(int)
{
    QString fn = QFileDialog::getOpenFileName(
        Gui::getMainWindow(), QObject::tr("Select a DWG file"), QString(),
        QObject::tr("DWG files (*.dwg);;All files (*)"));
    if (fn.isEmpty()) {
        return;
    }
    std::string path = QFile::encodeName(fn).constData();

    // Every import goes into a fresh document of its own, named after the
    // DWG file - so the .fcstd save dialog always defaults to
    // <dwg basename>.fcstd, and one DWG never mixes into another document's
    // tree.  (Importing into the *active* document used to silently swallow
    // the drawing into whatever happened to be open, e.g. a leftover "DWG"
    // document, which then saved under the wrong name.)  FreeCAD unique-ifies
    // both the internal name and the Label when the same DWG is imported
    // twice, so both documents stay distinct.
    const QString base = QFileInfo(fn).completeBaseName();
    App::Document* doc = App::GetApplication().newDocument(
        base.isEmpty() ? "DWG" : base.toUtf8().constData());
    if (!doc) {
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Import DWG"),
                             QObject::tr("Could not create a document."));
        return;
    }

    // Pure C++ import: LibreDWG read + block expansion + matrix transforms +
    // OCC shape building. No Python involved.
    //
    // No undo transaction on purpose: huge drawings (tens of thousands of
    // objects) would make the undo snapshots eat gigabytes of RAM. The import
    // is therefore not undoable as a single step.
    std::string error;
    int created = 0;
    int skipped = 0;

    const auto tStart = std::chrono::steady_clock::now();
    auto elapsedSeconds = [&tStart]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now()
                                             - tStart)
            .count();
    };

    // Lightweight progress on the main window's status bar instead of a modal
    // pop-up: the bar only becomes visible after the first update (~250 ms in),
    // so quick imports never flash any UI.  The importer reports 0..1 + a
    // phase text on this same (GUI) thread; the callback pumps events so the
    // bar stays repainted and responsive.
    QMainWindow* mw = Gui::getMainWindow();
    if (mw) {
        mw->setEnabled(false);  // freeze the UI: no re-entrant commands mid-import
    }
    QStatusBar* statusBar = mw ? mw->statusBar() : nullptr;
    QProgressBar bar(statusBar);
    bar.setRange(0, 1000);
    bar.setValue(0);
    bar.setTextVisible(true);
    bar.hide();
    if (statusBar) {
        statusBar->addWidget(&bar, 1);  // stretch so the bar fills the strip
    }
    // Refresh the bar at most every ~250 ms (and always on the final update).
    // Each refresh costs a repaint + a Qt event pump, and doing it on every
    // import callback would measurably slow the import down.
    auto lastUi = std::chrono::steady_clock::now();
    QString lastPhase;
    auto updateProgress = [&bar, &lastUi, &lastPhase, statusBar](
                              double f, const std::string& phase) {
        const bool isEnd = (f >= 1.0 - 1e-9);
        const auto now = std::chrono::steady_clock::now();
        if (!isEnd && std::chrono::duration<double>(now - lastUi).count()
                          < 0.25) {
            return;  // throttled: let the import run at full speed
        }
        lastUi = now;
        const QString ph = QString::fromUtf8(phase.c_str());
        if (ph != lastPhase) {  // set the phase text only when it changes
            lastPhase = ph;
            if (ph.isEmpty()) {
                bar.setFormat(QString());
            } else {
                bar.setFormat(ph + QStringLiteral("  %p%"));
            }
        }
        bar.setValue(static_cast<int>(f * 1000.0));
        if (statusBar && !bar.isVisible()) {
            bar.show();  // the import has been running for a while
        }
        QCoreApplication::processEvents();
    };
    auto hideProgress = [statusBar, mw, &bar]() {
        if (statusBar) {
            statusBar->removeWidget(&bar);
        }
        bar.hide();
        if (mw) {
            mw->setEnabled(true);
        }
    };

    const bool okImport = SWDWG::DWGImporter::importDWG(
        doc, path, error, created, skipped, updateProgress);
    if (!okImport) {
        hideProgress();
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Import DWG"),
                             QObject::tr("Import failed:\n%1")
                                 .arg(QString::fromUtf8(error.c_str())));
        Base::Console().message("[SWDWG] Import failed after %.2f s\n",
                                elapsedSeconds());
        return;
    }
    // Make the drawing look like the original: apply the per-group
    // color / linetype / line weight stored on each imported feature.
    updateProgress(0.96, "Applying DWG styles...");
    applyDWGStyles(doc);
    updateProgress(1.0, "Import finished");
    hideProgress();
    // Always report the total wall-clock time of the import (key operation
    // feedback - intentionally not gated by sw_debug).
    Base::Console().message(
        "[SWDWG] Import finished in %.2f s (%d created, %d skipped)\n",
        elapsedSeconds(), created, skipped);
    if (statusBar) {
        statusBar->showMessage(
            QObject::tr("DWG import finished in %1 s (%2 objects created)")
                .arg(elapsedSeconds(), 0, 'f', 2)
                .arg(created),
            10000);
    }
    updateActive();
}

//===========================================================================
// 导出功能 BEGIN：CmdSWDWGExport - export the selection / document to DWG
//===========================================================================
// 只用 LibreDWG 的写接口，不经过 DXF，也不依赖任何外部转换器。
// 非 ASCII 目标路径由 DWGWriter 内部处理（ASCII 临时名 + 改名），这里直接调用。

DEF_STD_CMD(CmdSWDWGExport)

CmdSWDWGExport::CmdSWDWGExport()
    : Command("SWDWG_Export")
{
    sAppModule = "SWDWG";
    sGroup = QT_TR_NOOP("SWDWG");
    sMenuText = QT_TR_NOOP("&Export DWG...");
    sToolTipText = QT_TR_NOOP(
        "Write the selected objects (or the whole document) into an AutoCAD "
        "2018 DWG file");
    sWhatsThis = "SWDWG_Export";
    sStatusTip = QT_TR_NOOP(
        "Export the selected objects (or the whole document) to a DWG file");
    sPixmap = "SWDWGWorkbench";
    sAccel = "";
    eType = (CmdType)0;
}

void CmdSWDWGExport::activated(int)
{
    App::Document* doc = App::GetApplication().getActiveDocument();
    if (!doc) {
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Export DWG"),
                             QObject::tr("There is no active document."));
        return;
    }

    // 选中的对象；没选中就导出整个文档（与自由导出习惯一致）。
    std::vector<App::DocumentObject*> objects;
    std::set<App::DocumentObject*> seen;
    for (const auto& sel : Gui::Selection().getSelectionEx(doc->getName())) {
        // 导出功能：getObject() 返回 const 指针，导出只读对象，const_cast 安全
        App::DocumentObject* obj = const_cast<App::DocumentObject*>(sel.getObject());
        if (obj && obj->getDocument() == doc && seen.insert(obj).second) {
            objects.push_back(obj);
        }
    }
    const bool wholeDocument = objects.empty();
    if (wholeDocument) {
        objects = doc->getObjects();
    }
    if (objects.empty()) {
        QMessageBox::information(Gui::getMainWindow(), QObject::tr("Export DWG"),
                                 QObject::tr("There is nothing to export."));
        return;
    }

    const QString suggested =
        QDir::home().filePath(QString::fromUtf8(doc->getName()) + QStringLiteral(".dwg"));
    QString fn = QFileDialog::getSaveFileName(
        Gui::getMainWindow(), QObject::tr("Export DWG"), suggested,
        QObject::tr("DWG files (*.dwg);;All files (*)"));
    if (fn.isEmpty()) {
        return;
    }
    if (!fn.endsWith(QStringLiteral(".dwg"), Qt::CaseInsensitive)) {
        fn += QStringLiteral(".dwg");
    }

    std::string error;
    std::vector<SWDWG::Entity> entities;
    std::vector<SWDWG::DWGOutLayer> layers;
    std::vector<SWDWG::DWGOutLinetype> linetypes;
    // 导出功能：几何转换里可能抛出 OCC 的异常（例如图纸里带无效拓扑时）。
    // FreeCAD 的命令框架只捕获 Base::Exception，漏出去的 C++ 异常会让整个
    // 程序退出，所以这里一律转成对话框错误。
    int skippedObjects = 0;
    try {
        skippedObjects = SWDWG::DWGCompose::compose(doc, objects, entities, layers,
                                                    linetypes, error);
    }
    catch (const std::exception& exc) {
        error = std::string("geometry conversion failed: ") + exc.what();
    }
    catch (...) {
        error = "geometry conversion failed (unknown C++ exception)";
    }
    if (!error.empty()) {
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Export DWG"),
                             QString::fromUtf8(error.c_str()));
        return;
    }
    // 导出功能：选中的可能是 Part 容器 / Origin / 轴网等没有几何的对象，
    // 这种情况自动退回"导出整个文档"，避免用户以为导出坏了。
    if (entities.empty() && !wholeDocument) {
        Base::Console().message(
            "[SWDWG] Selection has no exportable geometry, exporting the whole "
            "document instead\n");
        objects = doc->getObjects();
        entities.clear();
        layers.clear();
        error.clear();
        try {
            skippedObjects = SWDWG::DWGCompose::compose(doc, objects, entities,
                                                        layers, linetypes, error);
        }
        catch (const std::exception& exc) {
            error = std::string("geometry conversion failed: ") + exc.what();
        }
        catch (...) {
            error = "geometry conversion failed (unknown C++ exception)";
        }
        if (!error.empty()) {
            QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Export DWG"),
                                 QString::fromUtf8(error.c_str()));
            return;
        }
    }
    if (entities.empty()) {
        QMessageBox::information(
            Gui::getMainWindow(), QObject::tr("Export DWG"),
            QObject::tr("Document \"%1\" (%2 objects) contains no exportable "
                        "geometry.")
                .arg(QString::fromUtf8(doc->getName()))
                .arg((int)objects.size()));
        return;
    }

    SWDWG::DWGWriteStats stats;
    bool written = false;
    try {
        written = SWDWG::DWGWriter::writeDWG(fn.toUtf8().toStdString(), entities,
                                             layers, linetypes, error, stats);
    }
    catch (const std::exception& exc) {
        error = std::string("DWG writing failed: ") + exc.what();
    }
    catch (...) {
        error = "DWG writing failed (unknown C++ exception)";
    }
    if (!written) {
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Export DWG"),
                             QObject::tr("Export failed:\n%1")
                                 .arg(QString::fromUtf8(error.c_str())));
        return;
    }

    const int skipped = stats.skipped + skippedObjects;
    Base::Console().message(
        "[SWDWG] Export finished: %d entities, %d layers, %d skipped -> %s\n",
        stats.entities, stats.layers, skipped, fn.toUtf8().constData());
    QMessageBox::information(
        Gui::getMainWindow(), QObject::tr("Export DWG"),
        QObject::tr("Wrote %1 entities on %2 layers into:\n%3\n\nAutoCAD 2018 "
                    "(AC1032) format%4")
            .arg(stats.entities)
            .arg(stats.layers)
            .arg(fn)
            .arg(skipped > 0 ? QObject::tr(", %1 objects skipped").arg(skipped)
                             : QString()));
}

//===========================================================================
// 导出功能 END
//===========================================================================

//===========================================================================
// CmdSWDWGViewSHX - view the glyphs of a .shx font / shape file
//===========================================================================
DEF_STD_CMD(CmdSWDWGViewSHX)

CmdSWDWGViewSHX::CmdSWDWGViewSHX()
    : Command("SWDWG_ViewSHX")
{
    sAppModule = "SWDWG";
    sGroup = QT_TR_NOOP("SWDWG");
    sMenuText = QT_TR_NOOP("&View SHX Font...");
    sToolTipText = QT_TR_NOOP(
        "Pick a compiled AutoCAD shape file (.shx) and draw every shape / "
        "glyph it contains on a grid");
    sWhatsThis = "SWDWG_ViewSHX";
    sStatusTip = QT_TR_NOOP(
        "Pick a .shx file and draw every shape it contains");
    sPixmap = "SWDWGWorkbench";
    sAccel = "";
    eType = (CmdType)0;
}

void CmdSWDWGViewSHX::activated(int)
{
    QString fn = QFileDialog::getOpenFileName(
        Gui::getMainWindow(), QObject::tr("Select a SHX font file"), QString(),
        QObject::tr(
            "AutoCAD shape files (*.shx);;All files (*)"));
    if (fn.isEmpty()) {
        return;
    }
    std::string path = QFile::encodeName(fn).constData();

    App::Document* doc = App::GetApplication().getActiveDocument();
    if (!doc) {
        doc = App::GetApplication().newDocument("SHX");
    }
    if (!doc) {
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("View SHX"),
                             QObject::tr("Could not create a document."));
        return;
    }

    std::string error;
    if (!SWDWG::viewShxFont(doc, path, error)) {
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("View SHX"),
                             QObject::tr("Could not draw the SHX file:\n%1")
                                 .arg(QString::fromUtf8(error.c_str())));
        return;
    }
    updateActive();
    // Fit the whole glyph table in the view.
    doCommand(Gui::Command::Gui,
              "Gui.SendMsgToActiveView(\"ViewFit\")");
}

//===========================================================================
// CmdSWDWGMultiInstance - demo SoMultipleInstance instancing
//===========================================================================
// Test command: creates a Rows x Cols grid whose every marker is drawn by a
// single Coin3D SoMultipleInstance node.  It demonstrates how the same
// technique can be used to place repeated small symbols (complex-linetype
// shapes, hatch arrows, dimension ticks, ...) without building an
// independent scene-graph branch per occurrence.
//
// Re-run the command to cycle the template (diamond -> plus -> octagon) and
// edit Rows / Cols / Spacing on the created object to watch the instance
// matrix array rebuild live.
DEF_STD_CMD(CmdSWDWGMultiInstance)

CmdSWDWGMultiInstance::CmdSWDWGMultiInstance()
    : Command("SWDWG_MultiInstance")
{
    sAppModule = "SWDWG";
    sGroup = QT_TR_NOOP("SWDWG");
    sMenuText = QT_TR_NOOP("&Multi-Instance Demo");
    sToolTipText = QT_TR_NOOP(
        "Create a grid demo rendered through one SoMultipleInstance node");
    sWhatsThis = "SWDWG_MultiInstance";
    sStatusTip = QT_TR_NOOP(
        "Create a grid demo rendered through one SoMultipleInstance node");
    sPixmap = "SWDWGWorkbench";
    sAccel = "";
    eType = (CmdType)0;
}

void CmdSWDWGMultiInstance::activated(int)
{
    App::Document* doc = App::GetApplication().getActiveDocument();
    if (!doc) {
        doc = App::GetApplication().newDocument("MultiInstance");
    }
    if (!doc) {
        QMessageBox::warning(Gui::getMainWindow(),
                             QObject::tr("Multi-Instance Demo"),
                             QObject::tr("Could not create a document."));
        return;
    }

    // Remove a previous demo instance so re-running the command rebuilds it
    // (and cycling the template is obvious).
    App::DocumentObject* old = doc->getObject("InstancingDemo");
    if (old) {
        doc->removeObject(old->getNameInDocument());
        doc->recompute();
    }

    auto* obj = doc->addObject<SWDWG::MultiInstanceTest>("InstancingDemo");
    if (!obj) {
        QMessageBox::warning(Gui::getMainWindow(),
                             QObject::tr("Multi-Instance Demo"),
                             QObject::tr("Could not create the demo object."));
        return;
    }
    obj->Rows.setValue(12);
    obj->Cols.setValue(18);
    obj->Spacing.setValue(15.0);
    obj->Template.setValue(0);
    obj->Label.setValue("InstancingDemo");
    doc->recompute();

    updateActive();
    // Frame the whole grid in the view.
    doCommand(Gui::Command::Gui, "Gui.SendMsgToActiveView(\"ViewFit\")");
}

void CreateSWDWGCommands(void)
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();
    rcCmdMgr.addCommand(new CmdSWDWGImport());
    // ======================= 导出功能 BEGIN =======================
    rcCmdMgr.addCommand(new CmdSWDWGExport());
    // ======================= 导出功能 END =========================
    rcCmdMgr.addCommand(new CmdSWDWGViewSHX());
    rcCmdMgr.addCommand(new CmdSWDWGMultiInstance());
}
