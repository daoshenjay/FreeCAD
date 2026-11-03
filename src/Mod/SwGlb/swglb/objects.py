# SPDX-License-Identifier: LGPL-2.1-or-later
"""Imported glTF primitives as their own object with a dedicated view provider.

The scene graph is built by us (textured triangles only), so no second surface
competes with the texture in the depth buffer -- the earlier approach painted
the image on top of a coincident Part/Mesh surface and produced grey patches.
"""

import os

import FreeCAD as App
import FreeCADGui as Gui
from pivy import coin

from . import reader

try:
    from swmaterial import texture as texture_module
except Exception:  # pragma: no cover - soft dependency on SwMaterial
    texture_module = None


_READ_CACHE = {}  # (path, mtime) -> primitives list from reader.read()


def _load(path, index):
    # reader.read parses the whole glb; every object of one file shares the
    # same primitives, so cache the parse result per file (mtime keyed, so
    # re-importing a changed file re-reads it)
    try:
        key = (path, os.path.getmtime(path))
    except OSError:
        key = (path, None)
    primitives = _READ_CACHE.get(key)
    if primitives is None:
        primitives = reader.read(path)
        _READ_CACHE[key] = primitives
    if 0 <= index < len(primitives):
        return primitives[index]
    return None


# -- selection highlight --------------------------------------------------
# FreeCAD's selection override paints emissive (lit) / diffuse (BASE_COLOR)
# colours, but a bound SoTexture2 keeps MODULATEing them away, so textured
# objects appear not to highlight at all (see GlbView._refresh_selection).

_selection_observer = None

# last preselection seen by the observer (python callbacks carry plain names)
_preselected = {"doc": None, "obj": None}


class _GlbSelectionObserver:
    """Re-evaluates the texture state whenever the selection changes."""

    def addSelection(self, doc, obj, sub, pnt=None):
        if _preselected["doc"] is not None:
            # selection supersedes the preselection (and when nothing was
            # selected no clearSelection event is sent, so restore here)
            previous = (_preselected["doc"], _preselected["obj"])
            _preselected["doc"] = _preselected["obj"] = None
            if previous[0] != doc or previous[1] != obj:
                _refresh_object(previous[0], previous[1])
        _refresh_object(doc, obj)

    def removeSelection(self, doc, obj, sub):
        _refresh_object(doc, obj)

    def setPreselection(self, doc, obj, sub):
        previous = (_preselected["doc"], _preselected["obj"])
        _preselected["doc"] = doc
        _preselected["obj"] = obj
        # moving the mouse from one object to the next only sends a new
        # SetPreselect -- restore the previous object's look here
        if previous[0] and (previous[0] != doc or previous[1] != obj):
            _refresh_object(previous[0], previous[1])
        _refresh_object(doc, obj)

    def removePreselection(self, doc=None, obj=None, sub=None):
        stored_doc, stored_obj = _preselected["doc"], _preselected["obj"]
        _preselected["doc"] = _preselected["obj"] = None
        # the event's names may be empty -- fall back to what we remember
        doc = doc or stored_doc
        obj = obj or stored_obj
        if doc and obj:
            _refresh_object(doc, obj)

    def clearSelection(self, doc=None):
        _preselected["doc"] = _preselected["obj"] = None
        for document in App.listDocuments().values():
            for entry in document.Objects:
                if not hasattr(entry, "GlbPath"):
                    continue
                view_object = getattr(entry, "ViewObject", None)
                proxy = getattr(view_object, "Proxy", None)
                if proxy is not None:
                    try:
                        proxy._refresh_selection()
                    except Exception:
                        pass


def _refresh_object(doc_name, obj_name):
    try:
        obj = App.getDocument(doc_name).getObject(obj_name)
        obj.ViewObject.Proxy._refresh_selection()
    except Exception:
        pass


def _view_colour(key, fallback):
    """A selection colour from the view preferences, like
    Gui::SelectionColors::viewPreferenceColor does (same key, same fallback).

    fallback is an sRGB tuple; the preference stores packed 0xrrggbbaa.
    """
    try:
        packed_default = (
            (int(fallback[0] * 255) << 24)
            | (int(fallback[1] * 255) << 16)
            | (int(fallback[2] * 255) << 8)
            | 0xFF
        )
        packed = App.ParamGet("User parameter:BaseApp/Preferences/View").GetUnsigned(
            key, packed_default
        )
        return (
            ((packed >> 24) & 0xFF) / 255.0,
            ((packed >> 16) & 0xFF) / 255.0,
            ((packed >> 8) & 0xFF) / 255.0,
        )
    except Exception:
        return fallback


# matches Gui::SelectionColors::highlightFallbackColor()
_HIGHLIGHT_FALLBACK = (10.0 / 255.0, 200.0 / 255.0, 1.0)


def _install_selection_observer():
    global _selection_observer
    if _selection_observer is not None:
        return
    observer = _GlbSelectionObserver()
    try:
        Gui.Selection.addObserver(observer)
    except Exception:
        return
    _selection_observer = observer


class GlbFeature:
    """Document object: keeps the file and primitive index, no Shape."""

    def __init__(self, obj):
        obj.Proxy = self

    def execute(self, obj):
        pass

    def onChanged(self, obj, prop):
        pass


class GlbView:
    """View provider that draws the textured triangles itself."""

    def __init__(self, view):
        # set fields BEFORE assigning Proxy: the assignment makes FreeCAD
        # call attach() synchronously, which already runs _rebuild()
        self.view = view
        self.nodes = None
        self.texture_switches = []
        view.Proxy = self

    # -- scene graph -----------------------------------------------------
    def attach(self, view):
        self.view = view
        self.separator = coin.SoSeparator()
        self.texture_switches = []
        try:
            view.addDisplayMode(self.separator, "Textured")
        except Exception:
            pass
        self._rebuild(view.Object)
        _install_selection_observer()

    def _rebuild(self, obj):
        separator = getattr(self, "separator", None)
        if separator is None:
            return
        self._sig = self._signature(obj)
        self.texture_switches = []
        self.light_model = None
        self.material = None
        self._base_ambient = (1.0, 1.0, 1.0)
        while separator.getNumChildren():
            separator.removeChild(separator.getChild(0))
        try:
            primitive = _load(obj.GlbPath, obj.Primitive)
        except Exception:
            primitive = None
        if not primitive or not primitive["indices"]:
            try:
                App.Console.PrintWarning(
                    "SwGlb: {} 没有可显示的三角形（图元缺失或不是三角形）\n".format(obj.Label)
                )
            except Exception:
                pass
            return

        offset = obj.Offset
        positions = primitive["positions"]
        points = []
        for index in range(0, len(positions), 3):
            points.append(
                coin.SbVec3f(
                    positions[index] + offset.x,
                    positions[index + 1] + offset.y,
                    positions[index + 2] + offset.z,
                )
            )

        # baseColorFactor of the glTF material (already converted to sRGB by
        # the reader); with a texture MODULATE multiplies it -- matching glTF
        # (factor x texture)
        colour = primitive.get("colour") or (1.0, 1.0, 1.0, 1.0)
        material = coin.SoMaterial()
        material.diffuseColor.setValue(colour[0], colour[1], colour[2])
        material.specularColor.setValue(0.0, 0.0, 0.0)
        material.emissiveColor.setValue(0.0, 0.0, 0.0)
        # transparency only with alphaMode BLEND: the glTF default OPAQUE
        # ignores the alpha of baseColorFactor (some exporters write alpha=0
        # for plain opaque colours -- honouring it would hide the model)
        if len(colour) > 3 and colour[3] < 1.0 and primitive.get("alpha_mode") == "BLEND":
            material.transparency.setValue(1.0 - colour[3])

        has_texture = bool(primitive["image"] and primitive["uvs"])
        light_model = coin.SoLightModel()
        # keep references for the selection/preselection painting regardless
        # of the texture branch
        self.light_model = light_model
        self.material = material
        # native textured objects (Part) render PHONG + texture: the viewer's
        # highlight/selection override is an emissive add-on, and emissive is
        # only part of the lit render equation -- under BASE_COLOR it is
        # silently dropped and highlighted objects would show no highlight
        material.ambientColor.setValue(0.2, 0.2, 0.2)
        material.shininess.setValue(1.0)
        light_model.model.setValue(coin.SoLightModel.PHONG)
        if has_texture:
            self._base_ambient = (0.2, 0.2, 0.2)
        else:
            # unlit BASE_COLOR renders light coloured models flat -- a white
            # GIS model is indistinguishable from the light background and
            # seems to not import at all. Shading (viewer headlight, Coin
            # generates face normals) keeps the colour readable. The ambient
            # reflectance stays at the Part workbench default (0.2): brighter
            # values wash the headlight shading away (every face renders
            # flat), pure black deviates from the native look.
            material.ambientColor.setValue(0.2, 0.2, 0.2)
            material.shininess.setValue(1.0)
            light_model.model.setValue(coin.SoLightModel.PHONG)

        hints = coin.SoShapeHints()
        hints.vertexOrdering.setValue(coin.SoShapeHints.COUNTERCLOCKWISE)
        if primitive.get("double_sided"):
            # files in the wild often carry CW winding and rely on double
            # sided rendering; SOLID would enable backface culling and can
            # hide the entire model (it looks fine in viewers that render
            # both sides by default)
            hints.shapeType.setValue(coin.SoShapeHints.UNKNOWN_FACE_TYPE)
        else:
            hints.shapeType.setValue(coin.SoShapeHints.SOLID)
        hints.faceType.setValue(coin.SoShapeHints.CONVEX)

        coords = coin.SoCoordinate3()
        coords.point.setValues(points)
        faces = coin.SoIndexedFaceSet()
        # glTF stores a plain triangle list; SoIndexedFaceSet needs one -1
        # terminator after each face, otherwise Coin renders the whole array
        # as a single (fan triangulated) polygon and the mesh looks shattered
        coord_index = []
        for start in range(0, len(primitive["indices"]), 3):
            coord_index.extend(primitive["indices"][start:start + 3])
            coord_index.append(-1)
        faces.coordIndex.setValues(coord_index)

        nodes = [light_model, material, hints, coords]
        if has_texture:
            from PySide import QtGui

            image = QtGui.QImage(primitive["image"])
            if not image.isNull():
                image = image.convertToFormat(QtGui.QImage.Format.Format_RGBA8888)
                texture = coin.SoTexture2()
                texture.image.setValue(
                    coin.SbVec2s(image.width(), image.height()), 4, bytes(image.constBits())
                )
                texture.model.setValue(coin.SoTexture2.MODULATE)
                texture.wrapS.setValue(coin.SoTexture2.REPEAT)
                texture.wrapT.setValue(coin.SoTexture2.REPEAT)
                uvs = primitive["uvs"]
                tex_coords = coin.SoTextureCoordinate2()
                tex_coords.point.setValues(
                    [(uvs[i], uvs[i + 1]) for i in range(0, len(uvs), 2)]
                )
                # texture + UVs live under a switch so the selection
                # highlight can turn the texture off (Coin's SoTexture2 has
                # no enable field)
                branch = coin.SoSwitch()
                branch.addChild(texture)
                branch.addChild(tex_coords)
                branch.whichChild.setValue(0)
                nodes.append(branch)
                self.texture_switches.append(branch)
        nodes.append(faces)
        for node in nodes:
            separator.addChild(node)

        self._refresh_selection()

        if texture_module is not None:
            # the scene graph just changed at runtime; Coin's automatic
            # clipping measures an empty box in this fork (near/far collapse,
            # model invisible until something forces a re-measure), so rely on
            # the NavigationStyle clipping like SwMaterial does
            try:
                texture_module.stabilize_clipping()
            except Exception:
                pass

    def _refresh_selection(self):
        """Paint the preselection highlight without hiding the texture.

        The top level viewer paints *selected* paths itself (emissive, and
        for BASE_COLOR objects a diffuse override) -- a bound texture keeps
        rendering, modulated with the selection colour, exactly like native
        textured Part objects. Preselection is NOT painted for plain Coin
        scene graphs though, so it is painted here the same way: emissive
        highlight colour on top of the untouched textured rendering.
        """
        selected = False
        try:
            obj = self.view.Object
            for entry in Gui.Selection.getSelection(obj.Document.Name):
                if entry.Name == obj.Name:
                    selected = True
                    break
        except Exception:
            return
        preselected = (
            _preselected["doc"] == obj.Document.Name
            and _preselected["obj"] == obj.Name
        )
        if self.material is None:
            return
        try:
            if preselected and not selected:
                # HighlightColor preference, same source and fallback as the
                # native paint path (SelectionColors::viewPreferenceColor)
                colour = _view_colour("HighlightColor", _HIGHLIGHT_FALLBACK)
                self.material.emissiveColor.setValue(*colour)
            else:
                # selected: the viewer paints it; unhighlighted: clean
                self.material.emissiveColor.setValue(0.0, 0.0, 0.0)
        except Exception:
            pass

    def _signature(self, obj):
        offset = obj.Offset
        return (obj.GlbPath, obj.Primitive, offset.x, offset.y, offset.z)

    def updateData(self, obj, prop):
        # a recompute broadcasts updateData for every changed property; only
        # rebuild when the glTF data signature actually differs from what is
        # on screen (import touches 3 properties per object -> 1 rebuild)
        if prop in ("GlbPath", "Primitive", "Offset"):
            try:
                if self._signature(obj) == getattr(self, "_sig", None):
                    return
            except Exception:
                pass
            self._rebuild(obj)

    def onChanged(self, view, prop):
        pass

    def getDisplayModes(self, view):
        return ["Textured"]

    def getDefaultDisplayMode(self):
        return "Textured"

    def getBoundingBox(self, view):
        try:
            obj = view.Object
            primitive = _load(obj.GlbPath, obj.Primitive)
            positions = primitive["positions"]
            xs = positions[0::3]
            ys = positions[1::3]
            zs = positions[2::3]
            offset = obj.Offset
            return App.BoundBox(
                min(xs) + offset.x, min(ys) + offset.y, min(zs) + offset.z,
                max(xs) + offset.x, max(ys) + offset.y, max(zs) + offset.z,
            )
        except Exception:
            return App.BoundBox()

    def doubleClicked(self, view):
        return True

    def setEdit(self, view, mode):
        return False

    def getIcon(self):
        return ""


def make_object(document, name, path, index, offset):
    obj = document.addObject("App::FeaturePython", name)
    obj.addProperty("App::PropertyFile", "GlbPath", "glTF", "source file")
    obj.addProperty("App::PropertyInteger", "Primitive", "glTF", "primitive index")
    obj.addProperty("App::PropertyVector", "Offset", "glTF", "import offset")
    obj.GlbPath = path
    obj.Primitive = index
    obj.Offset = App.Vector(*offset)
    GlbFeature(obj)
    if obj.ViewObject is not None:
        GlbView(obj.ViewObject)
    return obj

