import os, time, json, traceback
import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.path.join(os.environ["TEMP"], "swqa")
os.makedirs(OUT, exist_ok=True)
RESULTS = []


def check(name, condition, detail=""):
    RESULTS.append((name, bool(condition), detail))
    print(("PASS " if condition else "FAIL ") + name + ((" | " + detail) if detail and not condition else ""), flush=True)


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
    images = {}
    for name, color in (("red", (200, 60, 50)), ("blue", (50, 110, 190))):
        img = QtGui.QImage(128, 128, QtGui.QImage.Format.Format_RGBA8888)
        img.fill(QtGui.QColor(*color))
        path = os.path.join(OUT, name + ".png")
        img.save(path)
        images[name] = path

    Gui.activateWorkbench("MaterialWorkbench")
    doc = App.newDocument("QA2")
    box = doc.addObject("Part::Box", "Cube")
    doc.recompute()
    Gui.updateGui()

    import swmaterial.mapping as mp
    import swmaterial.texture as texture
    from swmaterial import panel as panel_module

    # base + override, persistence
    texture.apply_texture(box, images["red"], mp.Mapping(mode=mp.CUBIC, scale_u=2.0))
    texture.apply_texture(box, images["blue"], mp.Mapping(mode=mp.PARAMETRIC, rotation=45.0), faces=[1])
    path = os.path.join(OUT, "qa2.FCStd")
    doc.saveAs(path)
    App.closeDocument(doc.Name)
    doc = App.openDocument(path)
    Gui.updateGui()
    box = doc.getObject("Cube")
    layers = texture.read_layers(box)
    check("persistence base+override",
          len(layers) == 2 and layers[0]["faces"] is None and layers[1]["faces"] == [1]
          and abs(layers[1]["mapping"].rotation - 45.0) < 1e-9,
          str([(l["faces"], l["mapping"].mode) for l in layers]))
    check("persistence overlay rebuilt", texture.has_texture(box))

    # legacy fallback on a fresh object
    box2 = doc.addObject("Part::Box", "Legacy")
    box2.addProperty("App::PropertyFile", "TextureImage", "Texture")
    box2.TextureImage = images["red"]
    legacy = texture.read_layers(box2)
    check("legacy fallback", len(legacy) == 1 and legacy[0]["faces"] is None, str(legacy))

    # missing image is reported, not silently ignored
    try:
        texture.apply_texture(box, os.path.join(OUT, "nope.png"), mp.Mapping())
        check("missing image raises", False)
    except ValueError as exc:
        check("missing image raises", "not found" in str(exc).lower(), str(exc))

    # undo / redo through the document transaction
    undo_before = len(texture.read_layers(box))
    texture.apply_texture(box, images["red"], mp.Mapping(mode=mp.PLANAR), faces=[2])
    after_apply = len(texture.read_layers(box))
    App.ActiveDocument.undo()
    undo_after = len(texture.read_layers(box))
    check("undo removes the new layer", undo_before == undo_after and after_apply > undo_before,
          "before=%d after=%d undo=%d" % (undo_before, after_apply, undo_after))

    # multi object apply
    cyl = doc.addObject("Part::Cylinder", "Cyl")
    doc.recompute()
    texture.apply_texture(box, images["blue"], mp.Mapping(mode=mp.AUTO))
    texture.apply_texture(cyl, images["blue"], mp.Mapping(mode=mp.AUTO))
    check("two objects textured", texture.has_texture(box) and texture.has_texture(cyl))

    # performance on a dense shape
    sphere = doc.addObject("Part::Sphere", "Big")
    sphere.Radius = 60
    doc.recompute()
    started = time.time()
    texture.apply_texture(sphere, images["red"], mp.Mapping(mode=mp.AUTO))
    build = time.time() - started
    check("build < 2s on dense sphere", build < 2.0, "%.2fs" % build)
    started = time.time()
    texture.update_layer(sphere, 0, mp.Mapping(mode=mp.SPHERICAL))
    update = time.time() - started
    check("live update < 2s on dense sphere", update < 2.0, "%.2fs" % update)

    # panel state machine
    panel = panel_module.get_panel()
    panel.show_texture_tab()
    QtWidgets.QApplication.processEvents()
    check("panel scope selector", panel.scope.count() == 3)
    check("panel live toggle", panel.live_update.isChecked())

    # panel: selection restore (whole vs face) on a fresh object
    fresh = doc.addObject("Part::Box", "Fresh")
    fresh.Placement.Base = App.Vector(0, 120, 0)
    doc.recompute()
    panel.texture_path.setText(images["red"])
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "Fresh")
    wait(500)
    panel.scope.setCurrentIndex(panel.scope.findData("object"))
    panel.mapping_mode.setCurrentIndex(panel.mapping_mode.findData("cubic"))
    panel.scale_u.setValue(2.0)
    wait(200)
    panel.apply_texture()
    wait(200)
    panel.scope.setCurrentIndex(panel.scope.findData("keep"))
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "Fresh", "Face2")
    wait(700)
    panel.mapping_mode.setCurrentIndex(panel.mapping_mode.findData("parametric"))
    wait(400)
    panel.mapping_rotation.setValue(30.0)
    wait(500)
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "Fresh")
    wait(900)
    whole = (panel.mapping_mode.currentData(), round(panel.scale_u.value(), 2), round(panel.mapping_rotation.value(), 1))
    check("restore whole-object params", whole == ("cubic", 2.0, 0.0), str(whole))
    Gui.Selection.clearSelection(); Gui.Selection.addSelection(doc.Name, "Fresh", "Face2")
    wait(900)
    face = (panel.mapping_mode.currentData(), round(panel.mapping_rotation.value(), 1))
    check("restore face params", face == ("parametric", 30.0), str(face))

    # live update must not touch the base when editing a face override
    layers_before = [(l["mapping"].mode, l["faces"]) for l in texture.read_layers(fresh)]
    panel.scale_u.setValue(1.5)
    wait(600)
    layers_after = [(l["mapping"].mode, l["faces"]) for l in texture.read_layers(fresh)]
    base_same = layers_before[0] == layers_after[0] if layers_before and layers_after else False
    check("live edit only touches the override", base_same, "%s -> %s" % (layers_before, layers_after))
except Exception:
    traceback.print_exc()

failed = [name for name, ok, _d in RESULTS if not ok]
print("SUMMARY %d/%d passed" % (len(RESULTS) - len(failed), len(RESULTS)), flush=True)
if failed:
    print("FAILED:", failed, flush=True)
QtCore.QTimer.singleShot(400, shutdown)



