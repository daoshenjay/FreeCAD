# SPDX-License-Identifier: LGPL-2.1-or-later
"""Export solid objects (and imported glTF meshes) to glTF/GLB, with textures.

FreeCAD's own glTF export (``Import.export``) hands the shape to OCCT's
``RWGltf_CafWriter``, which writes positions and normals only: the B-rep has no
UV nodes and the material colours are only forwarded when the caller passes
``(object, colours)`` tuples.  The textures of SwMaterial live in the Coin scene
graph, so no exporter can see them at all.

Objects created by the glTF importer have no ``Shape`` either: they are
``App::FeaturePython`` proxies around the triangles of the source file.  Their
triangles, UVs and image are read through ``GlbView.mesh_data()`` (and the
SwMaterial layer when a texture was projected on them), so importing a glb and
exporting it again keeps what the viewport shows.

This module therefore writes the file itself.  It reuses the FreeCAD APIs
(``Shape.tessellate``, ``ShapeAppearance``) and the *same* mapping code as the
viewport, so the exported UVs match what is displayed.
"""

import json
import os
import time

import FreeCAD as App

try:
    from swmaterial import mapping as mapping_module
    from swmaterial import texture as texture_module
except Exception:  # pragma: no cover - SwMaterial missing
    mapping_module = None
    texture_module = None

from .gltf import GlbWriter

try:
    import numpy as _np
except Exception:  # pragma: no cover
    _np = None

MAX_FACES = 400000

# mapping modes whose UV only depends on the point, so the whole shape can be
# tessellated in one call instead of once per face
POSITION_ONLY = {"planar", "elevation", "directional"}


def _deflection(shape):
    try:
        diagonal = shape.BoundBox.DiagonalLength
    except Exception:
        diagonal = 0.0
    if diagonal <= 0.0:
        return 0.2
    return max(0.05, min(3.0, diagonal * 0.002))


def _tessellate(face, tolerance):
    try:
        points, facets = face.tessellate(tolerance)
    except Exception:
        return [], []
    return points, facets


def _to_yup(values, size, offset_mm=None, scale=1.0):
    """FreeCAD is Z-up millimetres, glTF is Y-up metres.

    (x, y, z) -> (x, z, -y) * scale.  ``offset_mm`` subtracts an (integer)
    millimetre offset *before* the conversion -- the subtraction happens in
    float64, only the result is stored as float32, which keeps distant
    models (1000 km) precise.  Pass it for positions only; normals and UVs
    must not be shifted or scaled.
    """
    if _np is not None:
        array = _np.asarray(values, dtype=_np.float64).reshape(-1, size)
        if offset_mm is not None and size == 3:
            array = array - _np.asarray(offset_mm, dtype=_np.float64)
        if size < 3:
            return (array.reshape(-1) * scale).tolist()
        array = array[:, [0, 2, 1]]
        array[:, 2] *= -1.0
        return (array * scale).reshape(-1).tolist()
    ox = oy = oz = 0.0
    if offset_mm is not None and size == 3:
        ox, oy, oz = offset_mm
    out = []
    for index in range(0, len(values), size):
        x, y, z = values[index: index + 3]
        out.extend(((x - ox) * scale, (z - oz) * scale, -(y - oz) * scale))
        rest = values[index + 3: index + size]
        if rest:
            out.extend(rest)
    return out


def _vertex_normals(positions, indices):
    """Per-vertex normals from the triangles (numpy when available)."""
    if _np is None:
        normals = [0.0] * len(positions)
        for position in range(0, len(indices), 3):
            i0, i1, i2 = indices[position], indices[position + 1], indices[position + 2]
            ax, ay, az = positions[i0 * 3: i0 * 3 + 3]
            bx, by, bz = positions[i1 * 3: i1 * 3 + 3]
            cx, cy, cz = positions[i2 * 3: i2 * 3 + 3]
            ux, uy, uz = bx - ax, by - ay, bz - az
            vx, vy, vz = cx - ax, cy - ay, cz - az
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            for index in (i0, i1, i2):
                normals[index * 3] += nx
                normals[index * 3 + 1] += ny
                normals[index * 3 + 2] += nz
        for index in range(0, len(normals), 3):
            nx, ny, nz = normals[index: index + 3]
            length = (nx * nx + ny * ny + nz * nz) ** 0.5
            if length > 1e-12:
                normals[index] = nx / length
                normals[index + 1] = ny / length
                normals[index + 2] = nz / length
            else:
                normals[index + 2] = 1.0
        return normals

    points = _np.asarray(positions, dtype=_np.float32).reshape(-1, 3)
    faces = _np.asarray(indices, dtype=_np.int64).reshape(-1, 3)
    a = points[faces[:, 0]]
    b = points[faces[:, 1]]
    c = points[faces[:, 2]]
    triangles = _np.cross(b - a, c - a)
    normals = _np.empty_like(points)
    flat = faces.reshape(-1)
    for axis in range(3):
        weights = _np.repeat(triangles[:, axis], 3)
        normals[:, axis] = _np.bincount(flat, weights=weights, minlength=len(points))
    lengths = _np.linalg.norm(normals, axis=1)
    lengths[lengths < 1e-12] = 1.0
    normals /= lengths[:, None]
    return normals.reshape(-1).tolist()


def _srgb_to_linear(value):
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


_APPEARANCE = {}
_COLOUR_CACHE = {}


def _appearance(obj):
    key = id(obj)
    if key in _APPEARANCE:
        return _APPEARANCE[key]
    appearance = None
    try:
        appearance = obj.ShapeAppearance
    except Exception:
        appearance = None
    if not appearance:
        try:
            appearance = obj.ViewObject.ShapeAppearance
        except Exception:
            appearance = None
    _APPEARANCE[key] = appearance if appearance else []
    return _APPEARANCE[key]


def _face_colour(obj, index):
    """Diffuse colour of one face, converted from sRGB to linear."""
    default = (0.7, 0.7, 0.7, 1.0)
    appearance = _appearance(obj)
    try:  # the App property is authoritative, the view provider may differ
        appearance = obj.ShapeAppearance
    except Exception:
        appearance = None
    if not appearance:
        try:
            appearance = obj.ViewObject.ShapeAppearance
        except Exception:
            appearance = None
    if not appearance:
        return default
    try:
        colour = appearance[min(index, len(appearance) - 1)].DiffuseColor
    except Exception:
        return default
    if not colour:
        return default
    key = tuple(float(component) for component in colour)
    cached = _COLOUR_CACHE.get(key)
    if cached is not None:
        return cached
    values = key + (1.0,) if len(key) == 3 else key
    result = tuple(_srgb_to_linear(component) for component in values[:3]) + (values[3],)
    _COLOUR_CACHE[key] = result
    return result


def _image_for(path):
    extension = os.path.splitext(path)[1].lower()
    mime = "image/png" if extension == ".png" else "image/jpeg"
    if extension not in (".png", ".jpg", ".jpeg"):
        return None, None
    try:
        with open(path, "rb") as handle:
            return handle.read(), mime
    except OSError:
        return None, None


def _mesh_object(obj):
    """True for imported glTF objects (no ``Shape``, only triangle proxies)."""
    view = getattr(obj, "ViewObject", None)
    proxy = getattr(view, "Proxy", None)
    return proxy is not None and callable(getattr(proxy, "mesh_data", None))


def _mesh_data(obj):
    """Triangles of an imported glTF object, or ``None`` for other objects."""
    if not _mesh_object(obj):
        return None
    try:
        data = obj.ViewObject.Proxy.mesh_data()
    except Exception:
        return None
    if not data or not data.get("indices"):
        return None
    return data


def _mesh_bound_box(obj):
    data = _mesh_data(obj)
    if data is None:
        return None
    positions = data["positions"]
    if not positions:
        return None
    box = App.BoundBox()
    for index in range(0, len(positions), 3):
        box.add(App.Vector(positions[index], positions[index + 1], positions[index + 2]))
    return box


def _recover_image(obj):
    """Re-extract the texture of an imported glTF object.

    ``reader.read`` writes the embedded images to a temporary folder; when that
    folder was cleaned (or the document was reopened later) the stored path no
    longer exists and the image has to be pulled out of the source file again.
    """
    path = getattr(obj, "GlbPath", None)
    index = getattr(obj, "Primitive", None)
    if not path or index is None:
        return None
    try:
        from . import reader as reader_module

        primitive = reader_module.read(path)[int(index)]
    except Exception:
        return None
    return primitive.get("image")


def _mesh_triangles(obj):
    """Positions, UVs, indices and image for an imported glTF object.

    SwMaterial stores a projected layer as ``{"mesh": {positions, indices,
    "projected": True}}`` without UVs, because they depend on the mapping.
    Those are recomputed here with the same code the viewport uses, so the glb
    that is written keeps the texture the user applied.
    """
    data = _mesh_data(obj)
    if data is None:
        return None
    positions = list(data["positions"])
    indices = list(data["indices"])
    uvs = list(data.get("uvs") or [])
    image = data.get("image") or None
    if image and not os.path.exists(image):
        image = _recover_image(obj)
    colour = data.get("colour")

    if texture_module is not None:
        try:
            layers = texture_module.read_layers(obj)
        except Exception:
            layers = []
        for layer in layers:
            payload = layer.get("mesh")
            if not payload:
                continue
            positions, uvs, indices = texture_module._decode_mesh(payload)
            mapping = layer.get("mapping")
            if payload.get("projected") and mapping is not None:
                positions, uvs, indices = texture_module._project_mesh(positions, indices, mapping)
            image = layer.get("image") or None
            break

    return positions, uvs, indices, image, colour


def _build_mesh(writer, obj, stats, offset_mm=None):
    """Write one imported glTF object back as a glTF primitive."""
    triangles = _mesh_triangles(obj)
    if triangles is None:
        return False
    positions, uvs, indices, image, colour = triangles
    indices = [int(index) for index in indices]
    if not indices:
        return False

    normals = _vertex_normals(positions, indices)
    positions = _to_yup(positions, 3, offset_mm, 0.001)
    normals = _to_yup(normals, 3)
    if not uvs:
        uvs = [0.0] * (2 * (len(positions) // 3))

    primitive = {
        "attributes": {
            "POSITION": writer.add_positions(positions),
            "NORMAL": writer.add_vectors(normals, 3),
            "TEXCOORD_0": writer.add_vectors(uvs, 2),
        },
        "indices": writer.add_indices(indices),
        "mode": 4,
    }

    payload, mime = _image_for(image) if image else (None, None)
    if payload is not None:
        texture_index = writer.add_image(image, payload, mime)
        primitive["material"] = writer.add_texture_material(
            texture_index, os.path.basename(image)
        )
        stats["textures"] += 1
    else:
        rgba = tuple(list(colour)[:4]) if colour else (1.0, 1.0, 1.0, 1.0)
        if len(rgba) < 4:
            rgba = rgba + (1.0,) * (4 - len(rgba))
        primitive["material"] = writer.add_color_material(rgba)

    mesh = writer.add_mesh(obj.Name, [primitive])
    writer.add_node(obj.Label or obj.Name, mesh)
    stats["objects"] += 1
    stats["triangles"] += len(indices) // 3
    return True


def _groups(obj, faces=None):
    """Faces of ``obj`` grouped by their texture layer (or by colour)."""
    shape = obj.Shape
    if faces is None:
        faces = list(shape.Faces)
    total = len(faces)
    layers = []
    if texture_module is not None:
        try:
            layers = texture_module.read_layers(obj)
        except Exception:
            layers = []

    assignment = [None] * total
    for position, layer in enumerate(layers):
        faces = layer["faces"]
        targets = range(total) if faces is None else faces
        for index in targets:
            if 0 <= index < total:
                assignment[index] = position

    result = {}
    for index, position in enumerate(assignment):
        if position is None:
            colour = _face_colour(obj, index)
            key = ("colour", colour)
        else:
            layer = layers[position]
            key = ("image", layer["image"], id(layer))
        bucket = result.setdefault(key, {"faces": [], "layer": layers[position] if position is not None else None})
        bucket["faces"].append(index)
    return result


def _fast_layer(obj, layers):
    """The single layer that covers the whole object with a point-only mapping."""
    if len(layers) != 1:
        return None
    layer = layers[0]
    if layer["faces"] is not None:
        return None
    if mapping_module is None or layer["mapping"].mode not in POSITION_ONLY:
        return None
    return layer


def _build_fast(writer, obj, layer, stats, offset_mm=None):
    """One tessellation for the whole shape (no per-face work)."""
    shape = obj.Shape
    tolerance = _deflection(shape)
    try:
        points, facets = shape.tessellate(tolerance)
    except Exception:
        return False
    if not facets:
        return False

    mapping = layer["mapping"] if layer is not None else None
    frame = mapping.frame(shape) if mapping is not None else None
    positions, uvs, indices = [], [], []
    try:
        for point in points:
            positions.extend((point.x, point.y, point.z))
            if mapping is not None:
                u, v = mapping.transform(*mapping.uv(point, {}, frame))
                uvs.extend((u, 1.0 - v))
            else:
                uvs.extend((0.0, 0.0))
    except Exception:
        return False   # needs per-face information, fall back to the slow path
    for facet in facets:
        indices.extend(facet)

    normals = _vertex_normals(positions, indices)
    positions = _to_yup(positions, 3, offset_mm, 0.001)
    normals = _to_yup(normals, 3)

    primitive = {
        "attributes": {
            "POSITION": writer.add_positions(positions),
            "NORMAL": writer.add_vectors(normals, 3),
            "TEXCOORD_0": writer.add_vectors(uvs, 2),
        },
        "indices": writer.add_indices(indices),
        "mode": 4,
    }
    if mapping is not None:
        payload, mime = _image_for(layer["image"])
        if payload is not None:
            texture_index = writer.add_image(layer["image"], payload, mime)
            primitive["material"] = writer.add_texture_material(
                texture_index, layer["image"].rsplit("/", 1)[-1]
            )
            stats["textures"] += 1
        else:
            primitive["material"] = writer.add_color_material(_face_colour(obj, 0))
    else:
        primitive["material"] = writer.add_color_material(_face_colour(obj, 0))

    mesh = writer.add_mesh(obj.Name, [primitive])
    writer.add_node(obj.Label or obj.Name, mesh)
    stats["objects"] += 1
    stats["triangles"] += len(indices) // 3
    return True


def _build_object(writer, obj, stats, offset_mm=None):
    shape = obj.Shape
    layers = []
    if texture_module is not None:
        try:
            layers = texture_module.read_layers(obj)
        except Exception:
            layers = []

    fast_layer = _fast_layer(obj, layers)
    if not layers or fast_layer is not None:
        # colour-only object: one tessellation, one material
        colours = set()
        try:
            faces = list(shape.Faces)
        except Exception:
            return
        for index in range(len(faces)):
            if len(colours) > 1:
                break
            colours.add(_face_colour(obj, index))
        if len(colours) <= 1:
            if _build_fast(writer, obj, fast_layer, stats, offset_mm):
                return

    faces = list(shape.Faces)
    if not faces:
        return
    tolerance = _deflection(shape)
    frame = None
    if mapping_module is not None:
        try:
            frame = mapping_module.Mapping().frame(shape)
        except Exception:
            frame = None

    primitives = []
    for key, bucket in _groups(obj, faces).items():
        layer = bucket["layer"]
        mapping = layer["mapping"] if layer is not None else None
        if mapping is not None:
            try:
                frame = mapping.frame(shape)
            except Exception:
                pass

        positions, normals, uvs, indices = [], [], [], []
        vertex_count = 0
        for index in bucket["faces"]:
            try:
                face = faces[index]
            except Exception:
                continue
            points, facets = _tessellate(face, tolerance)
            if not facets:
                continue
            info = mapping_module.face_info(face) if (mapping and mapping_module) else {}
            face_uvs = []
            if mapping is not None:
                face_uvs = [mapping.uv(point, info, frame) for point in points]
                if mapping_module.uses_wrap(mapping.mode, info):
                    unwrapped = mapping_module.fix_seam([u for u, _v in face_uvs])
                    face_uvs = list(zip(unwrapped, [v for _u, v in face_uvs]))
                face_uvs = [mapping.transform(u, v, info.get("kind")) for u, v in face_uvs]

            start = vertex_count
            for position, point in enumerate(points):
                positions.extend((point.x, point.y, point.z))
                if face_uvs:
                    u, v = face_uvs[position]
                    # glTF puts v=0 at the top row, mapping puts it at the bottom
                    uvs.extend((u, 1.0 - v))
                else:
                    uvs.extend((0.0, 0.0))
                normals.extend((0.0, 0.0, 0.0))
            vertex_count += len(points)

            for facet in facets:
                a, b, c = facet[0], facet[1], facet[2]
                indices.extend((start + a, start + b, start + c))

        if not indices:
            continue

        normals = _vertex_normals(positions, indices)
        positions = _to_yup(positions, 3, offset_mm, 0.001)
        normals = _to_yup(normals, 3)
        uvs = uvs  # texture coordinates stay 2D
        primitive = {
            "attributes": {
                "POSITION": writer.add_positions(positions),
                "NORMAL": writer.add_vectors(normals, 3),
                "TEXCOORD_0": writer.add_vectors(uvs, 2),
            },
            "indices": writer.add_indices(indices),
            "mode": 4,
        }

        if key[0] == "image":
            payload, mime = _image_for(key[1])
            if payload is not None:
                texture_index = writer.add_image(key[1], payload, mime)
                primitive["material"] = writer.add_texture_material(
                    texture_index, os.path.basename(key[1])
                )
                stats["textures"] += 1
            else:
                primitive["material"] = writer.add_color_material(_face_colour(obj, bucket["faces"][0]))
        else:
            primitive["material"] = writer.add_color_material(key[1])
        primitives.append(primitive)
        stats["triangles"] += len(indices) // 3

    if primitives:
        mesh = writer.add_mesh(obj.Name, primitives)
        writer.add_node(obj.Label or obj.Name, mesh)
        stats["objects"] += 1


def _world_offset(objects):
    """Integer metre offset for the scenes.extras relative-to-origin trick.

    Float32 cannot keep useful precision at 1000 km distances (steps of
    ~6 cm), so every position is exported relative to the rounded centre of
    the overall bounding box and the offset is written to ``scenes[0].extras``
    (metres, as in [236200.0, 0.0, -3970003.0]).  Returns the millimetre
    offset for the vertex math and the metre triple for the JSON.
    """
    total = None
    for obj in objects:
        box = None
        try:
            box = obj.Shape.BoundBox
        except Exception:
            box = _mesh_bound_box(obj)
        if box is None:
            continue
        if total is None:
            total = App.BoundBox(box)
        else:
            total.add(box)
    if total is None:
        return (0.0, 0.0, 0.0), [0.0, 0.0, 0.0]
    centre = total.Center
    extras = [
        float(round(centre.x * 0.001)),
        float(round(centre.y * 0.001)),
        float(round(centre.z * 0.001)),
    ]
    offset_mm = tuple(value * 1000.0 for value in extras)
    return offset_mm, extras


def _write_sidecars(path, stats):
    """Index.json / Properties.json next to an exported .glb.

    GIS consumers of the pipeline read them: Index.json lists the glb files
    of a document (one document -> one glb), Properties.json lists the models
    inside each glb.  Only .glb exports get them, .gltf does not.  Both files
    are rewritten on every export and share one millisecond timestamp.
    """
    if os.path.splitext(path)[1].lower() != ".glb":
        return
    timestamp = int(time.time() * 1000)
    name = os.path.basename(path)
    code = os.path.splitext(name)[0]
    directory = os.path.dirname(path)
    index = {
        "type": "glbData",
        "glbs": [{"name": name, "timestamp": timestamp, "code": code}],
    }
    properties = [
        {
            "glbName": name,
            "locationCode": code,
            "timestamp": timestamp,
            "elements": [{"properties": []} for _ in range(stats.get("objects", 0))],
        }
    ]
    for filename, payload in (("Index.json", index), ("Properties.json", properties)):
        try:
            target = os.path.join(directory, filename)
            with open(target, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, ensure_ascii=False, indent=2)
        except OSError as exc:
            App.Console.PrintWarning("SwGlb: 写 {} 失败: {}\n".format(filename, exc))


def export_objects(objects, path, scene_name="SwGlb"):
    """Write ``objects`` (Part shapes) to ``path`` as one .glb file."""
    writer = GlbWriter()
    stats = {"objects": 0, "triangles": 0, "textures": 0, "materials": 0}
    expanded = expand(objects)
    offset_mm, extras = _world_offset(expanded)
    for obj in expanded:
        if getattr(obj, "Shape", None) is None:
            # imported glTF objects have no Shape at all: write their triangles
            _build_mesh(writer, obj, stats, offset_mm)
            continue
        try:
            if len(obj.Shape.Faces) > MAX_FACES:
                App.Console.PrintWarning(
                    "SwGlb: 跳过 {}（面数超过 {}）\n".format(obj.Label, MAX_FACES)
                )
                continue
        except Exception:
            continue
        _build_object(writer, obj, stats, offset_mm)
    if not stats["objects"]:
        raise ValueError("没有可导出的实体")
    stats["materials"] = len(writer.materials)
    size = writer.save(path, scene_name, extras)
    _write_sidecars(path, stats)
    return stats, size


def expand(objects):
    """Add the shapes hidden inside containers (Part::Compound, PartDesign Body…)."""
    result = []
    for obj in objects:
        if getattr(obj, "Shape", None) is not None or _mesh_object(obj):
            result.append(obj)
            continue
        for attribute in ("Tip", "Group"):
            try:
                children = getattr(obj, attribute, None)
            except Exception:
                children = None
            if not children:
                continue
            if not isinstance(children, (list, tuple)):
                children = [children]
            result.extend(expand(list(children)))
            break
    return result


def visible_objects():
    """All Part objects that are currently visible (fallback when nothing is selected)."""
    result = []
    document = App.ActiveDocument
    if document is None:
        return result
    for obj in document.Objects:
        shape = getattr(obj, "Shape", None)
        if shape is None and not _mesh_object(obj):
            continue
        if shape is not None and not getattr(shape, "Faces", None):
            continue
        view = getattr(obj, "ViewObject", None)
        if view is not None and not getattr(view, "Visibility", True):
            continue
        result.append(obj)
    return result


def export(objects, filename):
    """Entry point for FreeCAD's export registry (File > Export, object
    context menu). ``objects`` is the selection (or the whole document);
    containers are expanded just like the workbench export button does."""
    return export_objects(expand(objects), filename)








