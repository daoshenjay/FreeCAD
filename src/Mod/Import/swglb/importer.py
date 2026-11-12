# SPDX-License-Identifier: LGPL-2.1-or-later
"""Import a .glb/.gltf into FreeCAD with its textures.

FreeCAD's own importer keeps only the triangles, so we build one mesh object per
glTF primitive and attach the textured overlay (with the UVs from the file) to
it.  Objects and textures therefore stay paired -- no bounding-box guessing.
"""

import os

import FreeCAD as App

try:
    import FreeCADGui as Gui
except Exception:  # FreeCADCmd has no GUI subsystem
    Gui = None

try:
    import Mesh as MeshModule
except Exception:  # pragma: no cover
    MeshModule = None

from . import objects as glb_objects, reader

try:
    from swmaterial import texture as texture_module
except Exception:  # pragma: no cover
    texture_module = None


def import_glb(path, document=None, scale=1.0):
    """Import ``path``; returns (document, objects, textures)."""
    if MeshModule is None:
        raise RuntimeError("Mesh 模块不可用")

    primitives = reader.read(path)
    if not primitives:
        raise ValueError("文件里没有可导入的几何")

    name = os.path.splitext(os.path.basename(path))[0]
    created = document is None
    if created:
        document = App.newDocument(name)
        document.Label = name

    # Models can sit kilometres away from the origin; float32 positions and the
    # depth buffer cannot handle that, so the geometry is moved to the origin.
    xs, ys, zs = [], [], []
    for primitive in primitives:
        xs.extend(primitive["positions"][0::3])
        ys.extend(primitive["positions"][1::3])
        zs.extend(primitive["positions"][2::3])
    offset = (
        (min(xs) + max(xs)) / 2.0 if xs else 0.0,
        (min(ys) + max(ys)) / 2.0 if ys else 0.0,
        (min(zs) + max(zs)) / 2.0 if zs else 0.0,
    )
    if max(abs(value) for value in offset) <= 1000.0:
        offset = (0.0, 0.0, 0.0)

    objects = 0
    textures = 0
    for order, primitive in enumerate(primitives):
        if not primitive["indices"]:
            continue  # points/lines: nothing to show, reader recorded the skip
        obj = glb_objects.make_object(
            document,
            "GlbMesh%03d" % order,
            path,
            order,
            (-offset[0], -offset[1], -offset[2]),
        )
        label = primitive.get("name") or "mesh %d" % (order + 1)
        obj.Label = label

        objects += 1
        if primitive.get("image") and primitive.get("uvs"):
            textures += 1

    if not objects:
        raise ValueError("文件里没有可显示的三角形图元")
    if reader.LAST_SKIPPED:
        names = ", ".join(name or "?" for name, _mode in reader.LAST_SKIPPED[:5])
        App.Console.PrintWarning(
            "SwGlb: 跳过 %d 个点/线图元（无法显示为面）：%s\n"
            % (len(reader.LAST_SKIPPED), names)
        )

    document.recompute()
    try:
        Gui.updateGui()
        if texture_module is not None:
            # the view provider scene graphs were just built at runtime; Coin's
            # automatic clipping measures an empty box in this fork and the
            # model stays invisible until a preselection forces a re-measure
            texture_module.stabilize_clipping()
        view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
        if view is not None:
            view.viewIsometric()
            view.fitAll()
    except Exception:
        pass
    return document, objects, textures


def open(filename):
    """Entry point for FreeCAD's import registry (File > Open, drag & drop,
    Import dialog): ``swglb.importer.open("model.glb")``."""
    return import_glb(filename)


def insert(filename, docname):
    """Entry point for drag & drop and File > Import.

    FreeCAD always calls insert for these (even without an open document) and
    passes the target document name; when that document does not exist yet
    (typically the placeholder name "Unnamed") the module is expected to
    create it (see ModuleIO::importFiles).
    """
    document = App.listDocuments().get(docname) if docname else None
    if document is None:
        # the placeholder document does not exist yet: create it under the
        # file's name instead of the throwaway "Unnamed"
        return import_glb(filename)
    return import_glb(filename, document=document)













