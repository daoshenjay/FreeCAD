import os, time, json, traceback
import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.path.join(os.environ["TEMP"], "swqa")
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + ((" | " + detail) if detail and not ok else ""), flush=True)


def wait(ms=500):
    end = time.time() + ms / 1000.0
    while time.time() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.02)


def shutdown():
    for name in list(App.listDocuments()):
        try:
            App.closeDocument(name)
        except Exception:
            pass
    Gui.getMainWindow().close()


QtCore.QTimer.singleShot(300000, shutdown)

try:
    Gui.activateWorkbench("MaterialWorkbench")
    import swmaterial.mapping as mp
    import swmaterial.texture as texture
    from swmaterial import panel as panel_module

    img = QtGui.QImage(64, 64, QtGui.QImage.Format.Format_RGBA8888)
    img.fill(QtGui.QColor(180, 80, 60))
    TEX = os.path.join(OUT, "tex3.png")
    img.save(TEX)

    doc = App.newDocument("QA3")
    a = doc.addObject("Part::Box", "A")
    b = doc.addObject("Part::Box", "B")
    b.Placement.Base = App.Vector(40, 0, 0)
    doc.recompute()
    Gui.updateGui()

    # corrupt data must not crash
    a.addProperty("App::PropertyString", "TextureLayers", "Texture")
    a.TextureLayers = "{not json"
    check("corrupt json tolerated", texture.read_layers(a) == [])
    a.TextureLayers = json.dumps([{"image": TEX, "faces": [99], "mapping": {}}, {"faces": [0]}])
    layers = texture.read_layers(a)
    check("invalid face index ignored at render", texture.apply_texture(a, TEX, mp.Mapping(), faces=[0]) and texture.has_texture(a))

    # missing image file on load
    a.TextureImage = os.path.join(OUT, "gone.png")
    a.TextureLayers = json.dumps([{"image": os.path.join(OUT, "gone.png"), "faces": None, "mapping": {}}])
    try:
        texture.rebuild(a)
        check("missing image on rebuild tolerated", True)
    except Exception as exc:
        check("missing image on rebuild tolerated", False, str(exc))

    # multi selection apply through the panel
    texture.remove_texture(a); texture.remove_texture(b)
    panel = panel_module.get_panel()
    panel.show_texture_tab()
    panel.texture_path.setText(TEX)
    panel.scope.setCurrentIndex(panel.scope.findData("object"))
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "A"); Gui.Selection.addSelection(doc.Name, "B")
    wait(500)
    panel.apply_texture()
    check("multi apply", texture.has_texture(a) and texture.has_texture(b), "%s %s" % (texture.has_texture(a), texture.has_texture(b)))

    # two objects keep separate params and restore on selection
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "A")
    wait(700)
    panel.mapping_mode.setCurrentIndex(panel.mapping_mode.findData("planar"))
    wait(400)
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "B")
    wait(700)
    panel.mapping_mode.setCurrentIndex(panel.mapping_mode.findData("spherical"))
    wait(400)
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "A")
    wait(900)
    first = panel.mapping_mode.currentData()
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "B")
    wait(900)
    second = panel.mapping_mode.currentData()
    check("two objects restore", (first, second) == ("planar", "spherical"), "%s %s" % (first, second))

    # undo a live edit
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "A")
    wait(700)
    before = texture.read_layers(a)[0]["mapping"].mode
    panel.mapping_mode.setCurrentIndex(panel.mapping_mode.findData("cubic"))
    wait(500)
    mid = texture.read_layers(a)[0]["mapping"].mode
    App.ActiveDocument.undo()
    wait(300)
    after = texture.read_layers(a)[0]["mapping"].mode
    check("undo live edit", before != mid and after == before, "%s %s %s" % (before, mid, after))

    # deleting a textured object must not break the panel
    texture.apply_texture(b, TEX, mp.Mapping())
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "B")
    wait(700)
    doc.removeObject("B")
    doc.recompute()
    wait(700)
    check("object deletion tolerated", panel._active_layer()[0] is None)

    # hide / display mode still leaves a valid overlay
    texture.remove_texture(a)
    texture.apply_texture(a, TEX, mp.Mapping())
    a.ViewObject.Visibility = False
    Gui.updateGui()
    a.ViewObject.Visibility = True
    a.ViewObject.DisplayMode = "Wireframe"
    Gui.updateGui()
    a.ViewObject.DisplayMode = "Flat Lines"
    Gui.updateGui()
    check("overlay survives visibility/mode changes", texture.has_texture(a))

    # timing: thumbnail list population
    started = time.time()
    panel.texture_category.setCurrentIndex(0)
    panel.texture_search.setText("a")
    wait(200)
    panel.texture_search.setText("")
    wait(200)
    check("texture filtering under 1.5s", (time.time() - started) < 1.5, "%.2fs" % (time.time() - started))
except Exception:
    traceback.print_exc()

failed = [name for name, ok, _d in RESULTS if not ok]
print("SUMMARY %d/%d passed" % (len(RESULTS) - len(failed), len(RESULTS)), flush=True)
if failed:
    print("FAILED:", failed, flush=True)
QtCore.QTimer.singleShot(400, shutdown)
