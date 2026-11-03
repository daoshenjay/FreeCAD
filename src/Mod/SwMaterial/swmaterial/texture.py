# SPDX-License-Identifier: LGPL-2.1-or-later

"""Image textures for solids, optionally limited to selected faces.

FreeCAD's own view providers do not draw the image stored in an ``App::Material``
(Part shapes have no texture coordinates), so the workbench builds the textured
geometry itself:

* the requested faces are tessellated and their vertices get UVs from the
  selected mapping mode (see :mod:`swmaterial.mapping`),
* the triangles are put into a small Coin scene graph
  (``SoTexture2`` + ``SoTextureCoordinate2`` + ``SoIndexedFaceSet``) that is
  inserted into the object's own ``RootNode``, just before the display-mode
  switch.  A polygon offset keeps it in front of the real shape, and the
  overlay is unpickable so selection still works on the solid,
* everything is stored in one dynamic property, ``TextureLayers`` (JSON)::

      [{"image": "D:/tex/brick.png",
        "mapping": {...},
        "faces": [0, 1, 2]}]        # faces == null  ->  the whole object

  so a texture survives save/reload, and different faces can carry different
  images/mappings; :class:`TextureObserver` rebuilds the nodes after loading or
  after the shape changed.

The result is a *display* effect only: the texture is not part of the shape and
is not written by STEP/IGES/3MF exporters.
"""

import json
import os

import FreeCAD as App
from PySide import QtGui

from . import mapping as mapping_module

try:
    from pivy import coin
except ImportError:  # pragma: no cover - FreeCAD always ships pivy
    coin = None

PROP_IMAGE = "TextureImage"
PROP_SCALE = "TextureScale"
PROP_MAPPING = "TextureMapping"
PROP_LAYERS = "TextureLayers"
GROUP = "Texture"
SEPARATOR_NAME = "SwMaterialTexture"

_layers = {}
_busy = False
_FACE_COUNTS = {}
_PENDING_REBUILD = set()


def _key(obj):
    doc = getattr(obj, "Document", None)
    return (doc.Name if doc is not None else "", obj.Name)


def _can_texture(obj):
    view = getattr(obj, "ViewObject", None)
    shape = getattr(obj, "Shape", None)
    return coin is not None and view is not None and shape is not None and hasattr(view, "RootNode")


def ensure_properties(obj):
    """Add the properties that remember the texture inside the document."""
    if not hasattr(obj, PROP_IMAGE):
        obj.addProperty("App::PropertyFile", PROP_IMAGE, GROUP, "Image projected on the object")
    if not hasattr(obj, PROP_SCALE):
        obj.addProperty("App::PropertyFloat", PROP_SCALE, GROUP, "Texture repeats")
        obj.TextureScale = 1.0
    if not hasattr(obj, PROP_MAPPING):
        obj.addProperty("App::PropertyString", PROP_MAPPING, GROUP, "Texture mapping settings")
        obj.TextureMapping = mapping_module.Mapping().to_json()
    if not hasattr(obj, PROP_LAYERS):
        obj.addProperty("App::PropertyString", PROP_LAYERS, GROUP, "Texture layers (JSON)")


# --- stored layers ----------------------------------------------------------


def _layer_entry(image, mapping, faces):
    return {"image": image, "mapping": mapping, "faces": faces}


def read_layers(obj):
    """Layers stored on the object (legacy single-image properties included)."""
    text = getattr(obj, PROP_LAYERS, "") or ""
    if text:
        try:
            data = json.loads(text)
        except (ValueError, TypeError):
            data = []
        layers = []
        for item in data:
            if not isinstance(item, dict):
                continue
            image = item.get("image") or ""
            if not image:
                continue
            entry = _layer_entry(
                image,
                mapping_module.Mapping.from_dict(item.get("mapping") or {}),
                item.get("faces"),
            )
            if item.get("mesh"):
                entry["mesh"] = item["mesh"]
            layers.append(entry)
        if layers:
            return layers
    image = getattr(obj, PROP_IMAGE, "") or ""
    if image:
        return [_layer_entry(image, stored_mapping(obj), None)]
    return []


def write_layers(obj, layers):
    """Persist the layers (and keep the legacy single-image properties in sync)."""
    data = []
    for layer in layers:
        item = {
            "image": layer["image"],
            "mapping": layer["mapping"].to_dict(),
            "faces": layer["faces"],
        }
        if layer.get("mesh"):
            item["mesh"] = layer["mesh"]
        data.append(item)
    obj.TextureLayers = json.dumps(data)
    if layers:
        first = layers[0]
        obj.TextureImage = first["image"]
        obj.TextureMapping = first["mapping"].to_json()
        obj.TextureScale = first["mapping"].scale


def _face_count(obj):
    try:
        shape = obj.Shape
    except Exception:
        return 0
    if shape is None:
        return 0
    cached = _FACE_COUNTS.get(id(obj))
    if cached is not None and cached[0] == id(shape):
        return cached[1]
    try:
        total = len(shape.Faces)
    except Exception:
        return 0
    _FACE_COUNTS[id(obj)] = (id(shape), total)
    return total


# --- scene graph ------------------------------------------------------------


def _load_image(path):
    image = QtGui.QImage(path)
    if image.isNull():
        raise ValueError("Cannot read image: {}".format(path))
    return image.convertToFormat(QtGui.QImage.Format.Format_RGBA8888)


def _display_inverse(root, index):
    """Inverse of the transforms that are applied before the overlay.

    The view provider stores the object's placement (and, in this fork, a
    visual coordinate offset) as transforms in ``RootNode``; the shape itself
    is stored in world coordinates.  The overlay must undo those transforms,
    otherwise it is shifted by the placement.
    """
    region = coin.SbViewportRegion(100, 100)
    total = coin.SbMatrix()
    total.makeIdentity()
    found = False
    for position in range(index):
        node = root.getChild(position)
        if not node.getTypeId().isDerivedFrom(coin.SoTransform.getClassTypeId()):
            continue
        action = coin.SoGetMatrixAction(region)
        action.apply(node)
        total.multRight(action.getMatrix())
        found = True
    if not found:
        return None
    # pivy exposes SbMatrix::inverse either as in-place (returns bool) or as a
    # function returning a new matrix, depending on the build.
    result = total.inverse()
    if hasattr(result, "multVecMatrix"):
        return result
    if result:
        return total
    return None


def _to_display_space(matrix, point):
    if matrix is None:
        return point
    try:
        vector = matrix.multVecMatrix(coin.SbVec3f(point.x, point.y, point.z))
        return App.Vector(vector[0], vector[1], vector[2])
    except TypeError:
        x, y, z = matrix.multVecMatrix((point.x, point.y, point.z))
        return App.Vector(x, y, z)


_mesh_cache = {}


def _deflection(shape):
    """Tessellation tolerance: fine enough to look right, coarse enough to be fast."""
    try:
        diagonal = shape.BoundBox.DiagonalLength
    except Exception:
        diagonal = 0.0
    if diagonal <= 0.0:
        return 0.2
    return max(0.05, min(3.0, diagonal * 0.002))


def _tessellate_faces(shape, face_indices):
    """Tessellate the requested faces, reusing the previous result when possible."""
    try:
        key = shape.hashCode()
    except Exception:
        key = None
    store = _mesh_cache.get(key) if key is not None else None
    if store is None:
        store = {}
        if key is not None:
            if len(_mesh_cache) > 8:
                _mesh_cache.pop(next(iter(_mesh_cache)))
            _mesh_cache[key] = store
    wanted = [index for index in face_indices if index not in store]
    if wanted:
        tolerance = _deflection(shape)
        try:
            all_faces = list(shape.Faces)   # one access instead of one per face
        except Exception:
            all_faces = []
        for index in wanted:
            try:
                face = all_faces[index]
                pts, facets = face.tessellate(tolerance)
            except Exception:
                store[index] = None
                continue
            store[index] = (face, pts, facets) if len(pts) >= 3 and facets else None
    return store


def _build_group(shape, image, mapping, face_indices, matrix=None):
    """One textured sub-mesh for a group of faces sharing image + mapping."""
    texture = coin.SoTexture2()
    texture.image.setValue(
        coin.SbVec2s(image.width(), image.height()), 4, bytes(image.constBits())
    )
    texture.model.setValue(coin.SoTexture2.MODULATE)
    wrap = (
        coin.SoTexture2.CLAMP
        if mapping.wrap == mapping_module.WRAP_CLAMP
        else coin.SoTexture2.REPEAT
    )
    texture.wrapS.setValue(wrap)
    texture.wrapT.setValue(wrap)

    frame = mapping.frame(shape)
    points, uvs, indices = [], [], []
    meshes = _tessellate_faces(shape, face_indices)
    for index in face_indices:
        mesh = meshes.get(index)
        if mesh is None:
            continue
        face, pts, facets = mesh
        info = mapping_module.face_info(face)
        face_uvs = [mapping.uv(point, info, frame) for point in pts]
        if mapping_module.uses_wrap(mapping.mode, info):
            unwrapped = mapping_module.fix_seam([u for u, _v in face_uvs])
            face_uvs = list(zip(unwrapped, [v for _u, v in face_uvs]))
        offset = len(points)
        kind = info.get("kind")
        if matrix is None:
            points.extend(pts)
        else:
            points.extend(_to_display_space(matrix, point) for point in pts)
        for u, v in face_uvs:
            tu, tv = mapping.transform(u, v, kind)
            uvs.append((tu, 1.0 - tv))
        for facet in facets:
            indices.extend((offset + facet[0], offset + facet[1], offset + facet[2], -1))

    group = coin.SoSeparator()
    if not indices:
        return group
    coords = coin.SoCoordinate3()
    coords.point.setValues(points)
    tex_coords = coin.SoTextureCoordinate2()
    tex_coords.point.setValues(uvs)
    faces = coin.SoIndexedFaceSet()
    faces.coordIndex.setValues(indices)

    offset_node = coin.SoPolygonOffset()
    offset_node.factor.setValue(-2.0)
    offset_node.units.setValue(-2.0)
    offset_node.styles.setValue(coin.SoPolygonOffset.FILLED)
    offset_node.on.setValue(True)

    for node in (offset_node, texture, tex_coords, coords, faces):
        group.addChild(node)
    return group


def _decode_mesh(payload):
    """Raw triangles stored by the GLB importer (base64 float/uint arrays)."""
    import array
    import base64

    def unpack(key, typecode):
        raw = base64.b64decode(payload.get(key, ""))
        values = array.array(typecode)
        values.frombytes(raw)
        return values

    positions = unpack("positions", "f")
    uvs = unpack("uvs", "f")
    indices = unpack("indices", "I")
    return positions, uvs, indices


def _build_mesh_group(payload, image, matrix=None):
    """Overlay for imported glTF triangles (UVs come from the file)."""
    positions, uvs, indices = _decode_mesh(payload)
    if not indices:
        return None
    points = []
    for index in range(0, len(positions), 3):
        point = App.Vector(positions[index], positions[index + 1], positions[index + 2])
        points.append(_to_display_space(matrix, point) if matrix is not None else point)

    group = coin.SoSeparator()
    texture = coin.SoTexture2()
    texture.image.setValue(
        coin.SbVec2s(image.width(), image.height()), 4, bytes(image.constBits())
    )
    texture.model.setValue(coin.SoTexture2.MODULATE)
    texture.wrapS.setValue(coin.SoTexture2.REPEAT)
    texture.wrapT.setValue(coin.SoTexture2.REPEAT)

    coords = coin.SoCoordinate3()
    coords.point.setValues(points)
    tex_coords = coin.SoTextureCoordinate2()
    tex_coords.point.setValues([(uvs[i], uvs[i + 1]) for i in range(0, len(uvs), 2)])
    faces = coin.SoIndexedFaceSet()
    # glTF stores a plain triangle list; SoIndexedFaceSet needs one -1
    # terminator after each face, otherwise Coin renders the whole array
    # as a single (fan triangulated) polygon and the mesh looks shattered
    coord_index = []
    for start in range(0, len(indices), 3):
        coord_index.extend(int(indices[position]) for position in range(start, min(start + 3, len(indices))))
        coord_index.append(-1)
    faces.coordIndex.setValues(coord_index)

    # imported meshes are rendered by their own node; a stronger offset keeps
    # the texture in front even when the model sits far from the origin
    # an imported mesh is only a proxy for picking, so draw its texture on top;
    # the material must be neutral/white, otherwise MODULATE darkens the image
    material = coin.SoMaterial()
    material.diffuseColor.setValue(1.0, 1.0, 1.0)
    material.ambientColor.setValue(0.9, 0.9, 0.9)
    material.emissiveColor.setValue(0.0, 0.0, 0.0)
    material.specularColor.setValue(0.0, 0.0, 0.0)
    offset = coin.SoPolygonOffset()
    offset.factor.setValue(-2.0)
    offset.units.setValue(-2.0)
    offset.styles.setValue(coin.SoPolygonOffset.FILLED)
    offset.on.setValue(True)
    drawn = coin.SoDrawStyle()
    drawn.style.setValue(coin.SoDrawStyle.FILLED)
    drawn.lineWidth.setValue(1.0)
    hints = coin.SoShapeHints()
    hints.vertexOrdering.setValue(coin.SoShapeHints.COUNTERCLOCKWISE)
    hints.shapeType.setValue(coin.SoShapeHints.SOLID)
    hints.faceType.setValue(coin.SoShapeHints.CONVEX)
    for node in (offset, drawn, hints, material, texture, tex_coords, coords, faces):
        group.addChild(node)
    return group


def add_mesh_layer(obj, image_path, positions, uvs, indices, store=True):
    """Attach a display-only textured mesh (used when importing a .glb)."""
    import array
    import base64

    ensure_properties(obj)
    payload = {
        "positions": base64.b64encode(array.array("f", positions).tobytes()).decode("ascii"),
        "uvs": base64.b64encode(array.array("f", uvs).tobytes()).decode("ascii"),
        "indices": base64.b64encode(array.array("I", indices).tobytes()).decode("ascii"),
    }
    layers = read_layers(obj)
    layers.append({"image": os.path.abspath(image_path), "mapping": mapping_module.Mapping(), "faces": None, "mesh": payload})
    _refresh(obj, layers)
    if store:
        global _busy
        with _transaction(obj, "导入贴图"):
            _busy = True
            try:
                write_layers(obj, layers)
            finally:
                _busy = False
    return True

def _build_overlay(obj, layers, matrix=None):
    """Scene graph with one sub-mesh per (image, mapping) group."""
    shape = getattr(obj, "Shape", None)
    total = _face_count(obj) if shape is not None else 0
    assignment = [None] * max(total, 1)

    for position, layer in enumerate(layers):
        faces = layer["faces"]
        targets = range(total) if faces is None else faces
        for index in targets:
            if 0 <= index < total:
                assignment[index] = position

    groups = {}
    for index, position in enumerate(assignment):
        if position is None:
            continue
        groups.setdefault(position, []).append(index)

    separator = coin.SoSeparator()
    separator.setName(SEPARATOR_NAME)

    pick = coin.SoPickStyle()
    pick.style.setValue(coin.SoPickStyle.UNPICKABLE)
    separator.addChild(pick)

    for position, faces in groups.items():
        layer = layers[position]
        image_path = layer["image"]
        if not os.path.isfile(image_path):
            continue
        image = _load_image(image_path)
        if layer.get("mesh"):
            group = _build_mesh_group(layer["mesh"], image, matrix)
            if group is not None:
                separator.addChild(group)
            continue
        if shape is None:
            continue
        separator.addChild(_build_group(shape, image, layer["mapping"], faces, matrix))

    # mesh-only objects (imported glTF) have no faces to assign, so their
    # texture layers are added directly from the stored triangle data
    for position, layer in enumerate(layers):
        if position in groups or not layer.get("mesh"):
            continue
        image_path = layer["image"]
        if not os.path.isfile(image_path):
            continue
        group = _build_mesh_group(layer["mesh"], _load_image(image_path), matrix)
        if group is not None:
            separator.addChild(group)
    return separator


def _insert_index(root):
    """Index of the display-mode switch (the overlay is mounted inside it)."""
    for index in range(root.getNumChildren()):
        if str(root.getChild(index).getName()) == "ModeSwitch":
            return index
    return root.getNumChildren()


def _mount_points(obj):
    """Where the overlay has to be attached.

    The view provider hides an object by switching its ``ModeSwitch`` off, so
    the overlay must live *inside* the switch -- otherwise it keeps rendering
    while the object is hidden, and it also has to follow display-mode changes
    (Shaded / Flat Lines / Wireframe / Points).  One copy per surface-capable
    display mode is the simplest way to satisfy both; the wireframe and points
    modes get nothing, so the texture does not fight with them.
    """
    root = obj.ViewObject.RootNode
    index = _insert_index(root)
    switch = root.getChild(index) if index < root.getNumChildren() else None
    if switch is None or str(switch.getTypeId().getName()) != "Switch":
        return [root]
    children = [switch.getChild(child) for child in range(switch.getNumChildren())]
    try:
        modes = list(obj.ViewObject.listDisplayModes())
    except Exception:
        modes = []
    if len(modes) == len(children):
        children = [
            child
            for child, mode in zip(children, modes)
            if mode not in ("Wireframe", "Points")
        ]
    return children or [root]


def stabilize_clipping():
    """Keep the view from collapsing its near/far planes after a scene change.

    Coin's automatic clipping measures the scene bounding box on the next
    redraw.  In this FreeCAD fork that measurement comes out empty after a
    runtime change of a view provider's scene graph, so the camera ends up with
    ``near = -0.55`` / ``far = 0`` and the whole model disappears.  The fork
    drives clipping from ``NavigationStyle`` itself (double precision object
    bounds), so switching Coin's auto clipping off is safe here and keeps the
    planes sane.
    """
    if coin is None:
        return
    try:
        import FreeCADGui as Gui

        document = Gui.ActiveDocument
        view = document.ActiveView if document is not None else None
        if view is None:
            return
        manager = view.getViewer().getSoRenderManager()
        if manager.getAutoClipping() != coin.SoRenderManager.NO_AUTO_CLIPPING:
            manager.setAutoClipping(coin.SoRenderManager.NO_AUTO_CLIPPING)
    except Exception:
        pass


# --- public api -------------------------------------------------------------


_highlights = {}


def _highlight_separator(obj, faces, matrix=None, colour=(0.1, 0.9, 0.3)):
    """Filled highlight of the given faces, drawn just above the texture.

    A polygon offset (a bit stronger than the texture overlay's) keeps it
    visible above the image while normal depth testing still lets the rest of
    the model hide it -- a face on the far side stays hidden.
    """
    shape = obj.Shape
    total = len(shape.Faces)
    indices = range(total) if faces is None else list(faces)

    points, index_list = [], []
    meshes = _tessellate_faces(shape, [i for i in indices if 0 <= i < total])
    for index in indices:
        if not (0 <= index < total):
            continue
        mesh = meshes.get(index)
        if mesh is None:
            continue
        face, pts, facets = mesh
        offset = len(points)
        points.extend(_to_display_space(matrix, point) for point in pts)
        for facet in facets:
            index_list.extend([offset + facet[0], offset + facet[1], offset + facet[2], -1])

    separator = coin.SoSeparator()
    separator.setName("SwMaterialSelection")
    if not index_list:
        return separator

    material = coin.SoMaterial()
    material.diffuseColor.setValue(*colour)
    material.emissiveColor.setValue(colour[0] * 0.45, colour[1] * 0.45, colour[2] * 0.45)
    material.transparency.setValue(0.5)

    offset_node = coin.SoPolygonOffset()
    offset_node.factor.setValue(-4.0)
    offset_node.units.setValue(-4.0)
    offset_node.styles.setValue(coin.SoPolygonOffset.FILLED)
    offset_node.on.setValue(True)

    coords = coin.SoCoordinate3()
    coords.point.setValues(points)
    faces_node = coin.SoIndexedFaceSet()
    faces_node.coordIndex.setValues(index_list)
    pick = coin.SoPickStyle()
    pick.style.setValue(coin.SoPickStyle.UNPICKABLE)
    for node in (pick, offset_node, material, coords, faces_node):
        separator.addChild(node)
    return separator

def set_highlight(obj, faces):
    """Highlight the selected faces of a textured object (or clear it)."""
    if coin is None:
        return
    clear_highlight(obj)
    if not _can_texture(obj):
        return
    if not faces:
        return
    root = obj.ViewObject.RootNode
    matrix = _display_inverse(root, _insert_index(root))
    separator = _highlight_separator(obj, faces, matrix)
    parents = _mount_points(obj)
    for parent in parents:
        parent.addChild(separator)
    _highlights[_key(obj)] = (separator, parents)


def clear_highlight(obj):
    entry = _highlights.pop(_key(obj), None)
    if entry is None:
        return
    separator, parents = entry
    for parent in parents:
        try:
            parent.removeChild(separator)
        except Exception:
            pass


def _refresh(obj, layers):
    """Rebuild the overlay of ``obj`` from ``layers``."""
    view = obj.ViewObject
    root = view.RootNode
    old = _layers.pop(_key(obj), None)
    if old is not None:
        separator, parents = old
        for parent in parents:
            try:
                parent.removeChild(separator)
            except Exception:
                pass
    if not layers:
        return False
    index = _insert_index(root)
    matrix = _display_inverse(root, index)
    separator = _build_overlay(obj, layers, matrix)
    parents = _mount_points(obj)
    for parent in parents:
        parent.addChild(separator)
    _layers[_key(obj)] = (separator, parents)
    return True


_batch = {"doc": None, "label": ""}


def begin_batch(label="应用贴图"):
    """Open one transaction for a run of apply/remove calls (much faster)."""
    doc = App.ActiveDocument
    if doc is None:
        return None
    if _batch["doc"] is None:
        doc.openTransaction(label)
        _batch["doc"] = doc
        _batch["label"] = label
    return doc


def end_batch():
    doc = _batch["doc"]
    if doc is None:
        return
    _batch["doc"] = None
    try:
        doc.commitTransaction()
    except Exception:
        pass


def _transaction(obj, label):
    """Context manager that makes the property writes undoable."""
    from contextlib import contextmanager

    @contextmanager
    def guard():
        if _batch["doc"] is not None:
            yield
            return
        doc = getattr(obj, "Document", None)
        opened = False
        if doc is not None:
            try:
                doc.openTransaction(label)
                opened = True
            except Exception:
                opened = False
        try:
            yield
        except Exception:
            if opened:
                try:
                    doc.abortTransaction()
                except Exception:
                    pass
            raise
        else:
            if opened:
                try:
                    doc.commitTransaction()
                except Exception:
                    pass

    return guard()


def apply_texture(obj, path, mapping=None, faces=None, store=True, replace=False):
    """Texture ``faces`` (all faces when None) of ``obj`` with the image at ``path``.

    ``replace`` drops every existing layer of the object first, so the new
    texture is the only one; otherwise the other faces keep their textures.
    """
    if not _can_texture(obj):
        return False
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise ValueError("Image not found: {}".format(path))
    if store:
        ensure_properties(obj)

    if mapping is None:
        mapping = stored_mapping(obj)
    elif isinstance(mapping, mapping_module.Mapping):
        mapping = mapping.copy()

    total = _face_count(obj)
    if faces is not None:
        faces = sorted({int(index) for index in faces if 0 <= int(index) < total})
        if not faces:
            return False

    layers = [] if replace else read_layers(obj)

    entry = _layer_entry(path, mapping, faces)
    if faces is None:
        # a whole-object texture replaces everything
        kept = [entry]
    else:
        # a face texture is an *override*: the base (whole-object) layer stays
        # untouched and later layers win while rendering
        kept = list(layers)
        for position, layer in enumerate(kept):
            if layer["faces"] == faces:
                kept[position] = entry
                break
        else:
            kept.append(entry)

    stabilize_clipping()
    _refresh(obj, kept)

    if store:
        global _busy
        with _transaction(obj, "应用贴图"):
            _busy = True
            try:
                write_layers(obj, kept)
            finally:
                _busy = False
    return True


def update_layer(obj, index, mapping, store=True):
    """Replace the mapping of one stored layer (used for live editing)."""
    layers = read_layers(obj)
    if not (0 <= index < len(layers)):
        return False
    if isinstance(mapping, mapping_module.Mapping):
        mapping = mapping.copy()
    layers[index]["mapping"] = mapping
    _refresh(obj, layers)
    if store:
        global _busy
        with _transaction(obj, "编辑贴图参数"):
            _busy = True
            try:
                write_layers(obj, layers)
            finally:
                _busy = False
    return True


def remove_texture(obj, faces=None, store=True):
    """Remove the texture of ``faces`` (everything when None).

    For a face list only the *topmost* layer covering each face is removed, so
    an override disappears and the whole-object (base) texture shows through
    again.
    """
    layers = read_layers(obj)
    total = _face_count(obj)
    if faces is None:
        layers = []
    else:
        faces = sorted({int(index) for index in faces if 0 <= int(index) < total})
        hits = {}
        for face in faces:
            for position in range(len(layers) - 1, -1, -1):
                layer = layers[position]
                covered = range(total) if layer["faces"] is None else set(layer["faces"])
                if face in covered:
                    hits.setdefault(position, set()).add(face)
                    break
        kept = []
        for position, layer in enumerate(layers):
            removed = hits.get(position)
            if not removed:
                kept.append(layer)
                continue
            covered = set(range(total)) if layer["faces"] is None else set(layer["faces"])
            remaining = sorted(covered - removed)
            if remaining:
                kept.append(_layer_entry(layer["image"], layer["mapping"], remaining))
        layers = kept

    _refresh(obj, layers)
    if store:
        global _busy
        with _transaction(obj, "移除贴图"):
            _busy = True
            try:
                ensure_properties(obj)
                write_layers(obj, layers)
                if not layers and hasattr(obj, PROP_IMAGE):
                    obj.TextureImage = ""
            finally:
                _busy = False
    return True


def has_texture(obj, faces=None):
    if _key(obj) not in _layers:
        return False
    if faces is None:
        return True
    layers = read_layers(obj)
    wanted = {int(index) for index in faces}
    for layer in layers:
        covered = set(range(_face_count(obj))) if layer["faces"] is None else set(layer["faces"])
        if wanted & covered:
            return True
    return False


def stored_mapping(obj):
    """Mapping stored on the object (cubic with the legacy scale as fallback)."""
    if hasattr(obj, PROP_MAPPING):
        text = getattr(obj, PROP_MAPPING, "") or ""
        if text:
            return mapping_module.Mapping.from_json(text)
    mapping = mapping_module.Mapping()
    try:
        value = float(getattr(obj, PROP_SCALE, 1.0) or 1.0)
    except (TypeError, ValueError):
        value = 1.0
    mapping.scale_u = mapping.scale_v = value
    return mapping


def stored_texture(obj):
    """Return (path, mapping) of the first layer, for convenience."""
    layers = read_layers(obj)
    if not layers:
        return "", mapping_module.Mapping()
    return layers[0]["image"], layers[0]["mapping"]


def rebuild(obj):
    """Rebuild from the properties stored in the document."""
    layers = read_layers(obj)
    if not layers:
        return False
    return _refresh(obj, layers)


def rebuild_document(doc, force=False):
    """Recreate every overlay of ``doc`` that is missing from the scene.

    ``force`` rebuilds even when the cache claims the overlay is there: after a
    document is closed and reopened the old nodes are gone, but the cache entry
    (keyed by document + object name) would otherwise suppress the rebuild.
    """
    if doc is None:
        return 0
    count = 0
    for obj in getattr(doc, "Objects", []):
        if force:
            old = _layers.pop(_key(obj), None)
            if old is not None:
                separator, parents = old
                for parent in parents:
                    try:
                        parent.removeChild(separator)
                    except Exception:
                        pass
        elif _key(obj) in _layers:
            continue
        layers = read_layers(obj)
        if not layers:
            continue
        missing = [layer["image"] for layer in layers if layer["image"] and not os.path.isfile(layer["image"])]
        if missing and len(missing) == len(layers):
            App.Console.PrintWarning(
                "SwMaterial: {} 的贴图文件已不存在，无法恢复：{}\n".format(obj.Label, missing[0])
            )
        try:
            if _refresh(obj, layers):
                count += 1
        except Exception:
            continue
    return count


def _deferred_rebuild(doc_name, delay=300):
    """Rebuild once the GUI has created the view providers for a loaded file."""
    from PySide import QtCore

    if doc_name in _PENDING_REBUILD:
        return
    _PENDING_REBUILD.add(doc_name)

    def run():
        _PENDING_REBUILD.discard(doc_name)
        try:
            doc = App.getDocument(doc_name)
        except Exception:
            return
        try:
            rebuild_document(doc, force=True)
        except Exception:
            pass

    QtCore.QTimer.singleShot(delay, run)


class TextureObserver:
    """Rebuild the overlays after loading, and when a shape changes."""

    def slotDeletedObject(self, obj):
        _layers.pop(_key(obj), None)
        _highlights.pop(_key(obj), None)

    def slotDeletedDocument(self, doc):
        self._drop_document(doc)

    def slotCloseDocument(self, doc):
        self._drop_document(doc)

    def _drop_document(self, doc):
        try:
            name = getattr(doc, "Name", "")
        except Exception:
            return
        if not name:
            return
        for cache in (_layers, _highlights):
            for key in [item for item in cache if item[0] == name]:
                cache.pop(key, None)

    def slotRecomputedDocument(self, doc):
        if _busy:
            return
        rebuild_document(doc)

    def slotActivateDocument(self, doc):
        if _busy:
            return
        rebuild_document(doc)
        # a freshly opened file has no view providers yet, so try again shortly
        try:
            _deferred_rebuild(doc.Name)
        except Exception:
            pass

    def slotFinishRestoreDocument(self, doc):
        if _busy:
            return
        try:
            _deferred_rebuild(doc.Name)
        except Exception:
            pass

    def slotChangedObject(self, obj, prop):
        global _busy
        name = getattr(prop, "Name", "")
        if _busy or name not in ("Shape", PROP_LAYERS):
            return
        layers = read_layers(obj)
        if not layers:
            return
        _busy = True
        try:
            _refresh(obj, layers)
        except Exception:
            pass
        finally:
            _busy = False


def _selected_faces(obj):
    """Face indices selected on ``obj`` (0-based)."""
    import FreeCADGui as Gui

    faces = []
    for selection in Gui.Selection.getSelectionEx():
        try:
            if selection.Object.Name != obj.Name:
                continue
        except ReferenceError:
            continue
        for name in selection.SubElementNames:
            if name.startswith("Face"):
                try:
                    faces.append(int(name[4:]) - 1)
                except ValueError:
                    pass
    return faces


def refresh_highlights():
    """Mirror the current selection onto the textured objects."""
    import FreeCADGui as Gui

    selected = {}
    try:
        for selection in Gui.Selection.getSelectionEx():
            obj = selection.Object
            faces = selected.setdefault(obj.Name, (obj, []))[1]
            for name in selection.SubElementNames:
                if name.startswith("Face"):
                    try:
                        faces.append(int(name[4:]) - 1)
                    except ValueError:
                        pass
    except Exception:
        return

    for key in list(_highlights):
        separator, parents = _highlights[key]
        if key[1] not in selected:
            entry = _highlights.pop(key, None)
            if entry is not None:
                for parent in entry[1]:
                    try:
                        parent.removeChild(entry[0])
                    except Exception:
                        pass
    for name, (obj, faces) in selected.items():
        try:
            if faces and has_texture(obj):
                set_highlight(obj, faces)
            else:
                clear_highlight(obj)
        except Exception:
            continue


def _schedule_highlight_refresh():
    """Coalesce burst selection changes (Ctrl-clicking many objects)."""
    from PySide import QtCore

    if _highlight_timer is None:
        return
    _highlight_timer.start(80)


class SelectionObserver:
    """Keeps the face highlight alive outside the panel."""

    def addSelection(self, doc, obj, sub, pnt=None):
        _schedule_highlight_refresh()

    def removeSelection(self, doc, obj, sub):
        _schedule_highlight_refresh()

    def clearSelection(self, doc=None):
        _schedule_highlight_refresh()

    def setPreselection(self, doc, obj, sub):
        pass

    def removePreselection(self, doc, obj, sub):
        pass


_selection_observer = None
_highlight_timer = None


def _init_highlight_timer():
    global _highlight_timer
    if _highlight_timer is not None:
        return
    from PySide import QtCore

    timer = QtCore.QTimer()
    timer.setSingleShot(True)
    timer.timeout.connect(refresh_highlights)
    _highlight_timer = timer


def install_selection_observer():
    global _selection_observer
    if _selection_observer is None:
        _selection_observer = SelectionObserver()
        _init_highlight_timer()
        import FreeCADGui as Gui

        Gui.Selection.addObserver(_selection_observer)
    return _selection_observer


_observer = None


def install_observer():
    """Register the document observer once."""
    global _observer
    if _observer is None:
        _observer = TextureObserver()
        App.addDocumentObserver(_observer)
    try:
        install_selection_observer()
    except Exception:
        pass
    return _observer





























