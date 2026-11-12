"""Regression test: texturing an imported glTF mesh (no ``Shape``).

SwMaterial projects the image on the triangles that the glTF view provider
exposes instead of on Part faces.  Round trip: box -> glb -> import -> texture.
"""

import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

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
    os.makedirs(OUT, exist_ok=True)
    Gui.activateWorkbench("MaterialWorkbench")
    import swmaterial.mapping as mp
    import swmaterial.texture as texture
    import swglb.export as glb_export
    import swglb.importer as glb_import

    from PySide import QtGui

    img = QtGui.QImage(64, 64, QtGui.QImage.Format.Format_RGBA8888)
    img.fill(QtGui.QColor(180, 80, 60))
    TEX = os.path.join(OUT, "mesh_tex.png")
    img.save(TEX)
    GLB = os.path.join(OUT, "mesh_tex.glb")

    doc = App.newDocument("MeshTex")
    box = doc.addObject("Part::Box", "Box")
    doc.recompute()
    glb_export.export_objects([box], GLB)
    check("glb exported", os.path.isfile(GLB) and os.path.getsize(GLB) > 0)

    glb_doc, objects, textures = glb_import.import_glb(GLB)
    App.setActiveDocument(glb_doc.Name)
    if hasattr(Gui, "updateGui"):
        Gui.updateGui()
    wait(300)
    mesh = [obj for obj in glb_doc.Objects if obj.TypeId == "App::FeaturePython"]
    check("glb object imported", len(mesh) == 1, repr([o.TypeId for o in glb_doc.Objects]))
    if not mesh:
        raise RuntimeError("no imported object")
    obj = mesh[0]

    check("mesh is a texture target", texture.can_texture(obj) and texture.filter_texture_targets([obj]) == [obj])
    check("mesh has triangles", obj.ViewObject.Proxy.mesh_data()["indices"] != [])

    master = mp.Mapping()
    check("texture applied to mesh", texture.apply_texture(obj, TEX, master))
    layers = texture.read_layers(obj)
    check("mesh layer stored", len(layers) == 1 and bool(layers[0].get("mesh")), repr(layers[:1]))
    check("mesh texture visible", texture.has_texture(obj) and bool(texture._layers.get(texture._key(obj))))

    planar = mp.Mapping(mode=mp.PLANAR)
    check("mapping can be changed", texture.update_layer(obj, 0, planar))
    check("projected layer kept", bool(texture.read_layers(obj)[0].get("mesh", {}).get("projected")))

    faces_mode = texture.apply_texture(obj, TEX, planar, faces=[0])
    check("face scope rejected on mesh", faces_mode is False)

    # exporting the imported mesh must keep its triangles, UVs and image
    ROUND = os.path.join(OUT, "mesh_tex_roundtrip.glb")
    stats, _size = glb_export.export_objects([obj], ROUND)
    check("mesh exported again", stats.get("objects") == 1 and stats.get("triangles", 0) > 0, repr(stats))
    back_doc, back_objects, back_textures = glb_import.import_glb(ROUND)
    check(
        "textured mesh survives glb round trip",
        back_objects == 1 and back_textures == 1,
        "%r objects / %r textures" % (back_objects, back_textures),
    )
    if back_doc is not None:
        App.closeDocument(back_doc.Name)
    App.setActiveDocument(glb_doc.Name)
    if hasattr(Gui, "updateGui"):
        Gui.updateGui()
    wait(200)

    saved = os.path.join(OUT, "mesh_tex.FCStd")
    glb_doc.saveAs(saved)
    App.closeDocument(glb_doc.Name)
    reopened = App.openDocument(saved)
    App.setActiveDocument(reopened.Name)
    wait(300)
    again = [o for o in reopened.Objects if o.TypeId == "App::FeaturePython"][0]
    check("mesh layer survives reload", len(texture.read_layers(again)) == 1 and texture.has_texture(again))

    texture.remove_texture(again)
    check("mesh texture removed", texture.read_layers(again) == [] and not texture.has_texture(again))
except Exception:
    traceback.print_exc()

failed = [name for name, ok, _d in RESULTS if not ok]
print("SUMMARY %d/%d passed" % (len(RESULTS) - len(failed), len(RESULTS)), flush=True)
if failed:
    print("FAILED:", failed, flush=True)
QtCore.QTimer.singleShot(400, shutdown)
