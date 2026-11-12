# SPDX-License-Identifier: LGPL-2.1-or-later
"""Minimal glTF/GLB reader used to bring textures back into FreeCAD.

FreeCAD's own glTF import (``Import``) keeps only the triangles; the materials,
images and UVs are dropped by ``ReaderGltf.cpp``.  This module parses the file
itself so ``swglb`` can rebuild the textured overlay on the imported object.
"""

import base64
import json
import os
import struct
import tempfile

COMPONENT = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}


def _chunks(data):
    offset = 12
    document, binary = {}, b""
    while offset + 8 <= len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8: offset + 8 + length]
        offset += 8 + length
        if kind == 0x4E4F534A:
            document = json.loads(chunk.decode("utf-8"))
        elif kind == 0x004E4942:
            binary = chunk
    return document, binary


def _accessor(document, binary, index):
    accessor = document["accessors"][index]
    view = document["bufferViews"][accessor["bufferView"]]
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    count = accessor["count"]
    component = accessor["componentType"]
    code, size = COMPONENT[component]
    width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}.get(accessor["type"], 1)
    stride = view.get("byteStride") or (size * width)
    values = []
    for item in range(count):
        base = start + item * stride
        values.extend(struct.unpack_from("<" + code * width, binary, base))

    # normalized integer attributes (common for UVs in some exporters) have to
    # be scaled to 0..1, otherwise the texture repeats thousands of times
    if accessor.get("normalized") and component in (5120, 5121, 5122, 5123):
        scale = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}[component]
        values = [value / scale for value in values]
    return values


def _image(document, binary, texture_index):
    """(bytes, mime) of a texture, following ``textures -> images``."""
    textures = document.get("textures") or []
    if not (0 <= texture_index < len(textures)):
        return None, None
    images = document.get("images") or []
    source = textures[texture_index].get("source")
    if source is None or not (0 <= source < len(images)):
        return None, None
    image = images[source]
    view_index = image.get("bufferView")
    if view_index is not None:
        view = document["bufferViews"][view_index]
        start = view.get("byteOffset", 0)
        return binary[start: start + view["byteLength"]], image.get("mimeType", "image/png")
    uri = image.get("uri")
    if not uri:
        return None, None
    if uri.startswith("data:"):
        header, _, payload = uri.partition(",")
        mime = header[5:].split(";")[0] or "image/png"
        return base64.b64decode(payload), mime
    return _read_uri(uri), None


def _material_colour(document, index):
    materials = document.get("materials") or []
    if not (0 <= index < len(materials)):
        return None
    factor = (materials[index].get("pbrMetallicRoughness") or {}).get("baseColorFactor")
    if not factor:
        return None

    def to_srgb(value):  # glTF stores linear values
        if value <= 0.0031308:
            return value * 12.92
        return 1.055 * (value ** (1.0 / 2.4)) - 0.055

    return tuple(round(to_srgb(component), 4) for component in factor[:3]) + (factor[3] if len(factor) > 3 else 1.0,)


_URI_BASE = {"dir": ""}

# (name, mode) of primitives dropped by the last read() call -- points and
# lines cannot be shown as faces, so the importer can explain empty imports
LAST_SKIPPED = []


def _read_uri(uri):
    """External image referenced by a relative path inside the .glb folder."""
    try:
        if uri.startswith("file://"):
            uri = uri[7:]
        path = uri if os.path.isabs(uri) else os.path.join(_URI_BASE["dir"], uri)
        with open(path, "rb") as handle:
            return handle.read()
    except OSError:
        return None


def _node_matrix(node):
    """4x4 matrix from a glTF node (translation / rotation / scale or matrix)."""
    if "matrix" in node:
        values = node["matrix"]
        return [[values[col * 4 + row] for col in range(4)] for row in range(4)]
    tx, ty, tz = (node.get("translation") or [0.0, 0.0, 0.0])[:3]
    qx, qy, qz, qw = (node.get("rotation") or [0.0, 0.0, 0.0, 1.0])[:4]
    sx, sy, sz = (node.get("scale") or [1.0, 1.0, 1.0])[:3]
    xx, yy, zz = qx * qx, qy * qy, qz * qz
    xy, xz, yz = qx * qy, qx * qz, qy * qz
    wx, wy, wz = qw * qx, qw * qy, qw * qz
    rotation = [
        [1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)],
        [2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)],
        [2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)],
    ]
    return [
        [rotation[0][0] * sx, rotation[0][1] * sy, rotation[0][2] * sz, tx],
        [rotation[1][0] * sx, rotation[1][1] * sy, rotation[1][2] * sz, ty],
        [rotation[2][0] * sx, rotation[2][1] * sy, rotation[2][2] * sz, tz],
        [0.0, 0.0, 0.0, 1.0],
    ]


def _multiply(a, b):
    return [[sum(a[row][k] * b[k][col] for k in range(4)) for col in range(4)] for row in range(4)]


def _apply(matrix, x, y, z):
    return (
        matrix[0][0] * x + matrix[0][1] * y + matrix[0][2] * z + matrix[0][3],
        matrix[1][0] * x + matrix[1][1] * y + matrix[1][2] * z + matrix[1][3],
        matrix[2][0] * x + matrix[2][1] * y + matrix[2][2] * z + matrix[2][3],
    )


def _walk_nodes(document, on_primitive):
    """Visit every mesh with its accumulated world matrix."""
    nodes = document.get("nodes") or []
    scenes = document.get("scenes") or []
    roots = []
    if scenes and scenes[doc_scene(document)].get("nodes"):
        roots = scenes[doc_scene(document)]["nodes"]
    else:
        roots = list(range(len(nodes)))
    identity = [[1.0 if row == col else 0.0 for col in range(4)] for row in range(4)]
    seen = set()

    def visit(index, parent):
        if not (0 <= index < len(nodes)) or index in seen:
            return
        seen.add(index)
        node = nodes[index]
        matrix = _multiply(parent, _node_matrix(node))
        if node.get("mesh") is not None:
            on_primitive(node["mesh"], matrix, node.get("name") or "")
        for child in node.get("children") or []:
            visit(child, matrix)

    for root in roots:
        visit(root, identity)


def doc_scene(document):
    return int(document.get("scene") or 0)

def _cache_dir():
    path = os.path.join(tempfile.gettempdir(), "swglb_import")
    os.makedirs(path, exist_ok=True)
    return path


def inspect(path):
    """Quick facts about a file, used for diagnostics when nothing imports."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:4] != b"glTF":
        return {"error": "不是 GLB 文件"}
    document, _binary = _chunks(data)
    primitives = []
    for mesh in document.get("meshes") or []:
        primitives.extend(mesh.get("primitives") or [])
    attributes = set()
    for primitive in primitives:
        attributes.update((primitive.get("attributes") or {}).keys())
    images = document.get("images") or []
    modes = sorted({primitive.get("mode", 4) for primitive in primitives})
    quantized = any(accessor.get("normalized") for accessor in document.get("accessors") or [])
    return {
        "extensions": document.get("extensionsUsed") or [],
        "modes": modes,
        "normalized_accessors": quantized,
        "meshes": len(document.get("meshes") or []),
        "primitives": len(primitives),
        "materials": len(document.get("materials") or []),
        "images": len(images),
        "textures": len(document.get("textures") or []),
        "attributes": sorted(attributes),
        "image_uris": [("bufferView" if image.get("bufferView") is not None else (image.get("uri") or "")[:12]) for image in images[:4]],
    }


def read(path):
    """Return the textured primitives of ``path``.

    Each item holds ``positions`` (FreeCAD Z-up, flat floats), ``uvs``,
    ``indices``, ``image`` (path on disk or None) and ``colour`` (RGBA or None).
    """
    with open(path, "rb") as handle:
        data = handle.read()
    _URI_BASE["dir"] = os.path.dirname(os.path.abspath(path))
    if data[:4] != b"glTF":
        raise ValueError("不是 GLB 文件")

    document, binary = _chunks(data)
    meshes = document.get("meshes") or []
    primitives = []
    del LAST_SKIPPED[:]
    cache = _cache_dir()
    written = 0
    pending = []

    def collect(mesh_index, matrix, node_name):
        if 0 <= mesh_index < len(meshes):
            for primitive in meshes[mesh_index].get("primitives") or []:
                pending.append((primitive, matrix, node_name, mesh_index))

    _walk_nodes(document, collect)

    if not pending:
        for mesh_index, mesh in enumerate(meshes):
            for primitive in mesh.get("primitives") or []:
                pending.append((primitive, None, mesh.get("name") or "", mesh_index))

    identity = [[1.0 if row == col else 0.0 for col in range(4)] for row in range(4)]
    for primitive, matrix, node_name, mesh_index in pending:
        mesh = meshes[mesh_index] if 0 <= mesh_index < len(meshes) else {}
        if True:
            attributes = primitive.get("attributes") or {}
            if "POSITION" not in attributes:
                continue
            positions = _accessor(document, binary, attributes["POSITION"])
            uvs = _accessor(document, binary, attributes["TEXCOORD_0"]) if "TEXCOORD_0" in attributes else []
            indices = _accessor(document, binary, primitive["indices"]) if "indices" in primitive else []
            mode = primitive.get("mode", 4)

            # only triangle based primitives can be shown; points and lines are
            # recorded so the importer can tell the user why nothing appeared
            if mode in (0, 1, 2, 3):
                LAST_SKIPPED.append((node_name or mesh.get("name") or "", mode))
                continue
            if mode == 5:  # TRIANGLE_STRIP: every (i, i+1, i+2) is a triangle
                strip, indices = indices, []
                for k in range(len(strip) - 2):
                    a, b, c = strip[k], strip[k + 1], strip[k + 2]
                    if k % 2:  # strips flip winding every triangle
                        b, c = c, b
                    indices.extend((a, b, c))
            elif mode == 6:  # TRIANGLE_FAN: first vertex is shared
                fan, indices = indices, []
                for k in range(1, len(fan) - 1):
                    indices.extend((fan[0], fan[k], fan[k + 1]))

            # glTF is Y-up, FreeCAD is Z-up: (x, y, z) -> (x, -z, y)
            converted = []
            for index in range(0, len(positions), 3):
                x, y, z = positions[index: index + 3]
                if matrix is not None and matrix is not identity:
                    x, y, z = _apply(matrix, x, y, z)
                converted.extend((x, -z, y))

            image_path = None
            material = primitive.get("material")
            colour = None
            double_sided = False
            alpha_mode = "OPAQUE"
            if material is not None:
                materials = document.get("materials") or []
                entry = materials[material] if 0 <= material < len(materials) else {}
                texture = ((entry.get("pbrMetallicRoughness") or {}).get("baseColorTexture") or {}).get("index")
                if texture is not None:
                    payload, mime = _image(document, binary, texture)
                    if payload:
                        extension = ".png" if "png" in (mime or "") else ".jpg"
                        written += 1
                        image_path = os.path.join(cache, "glb_tex_%03d%s" % (written, extension))
                        with open(image_path, "wb") as handle:
                            handle.write(payload)
                colour = _material_colour(document, material)
                double_sided = bool(entry.get("doubleSided", False))
                # glTF: the alpha of baseColorFactor only matters with
                # alphaMode BLEND; the default OPAQUE (and MASK) ignore it.
                # Exporters that write alpha=0 while meaning opaque yellow
                # would otherwise render the whole model fully transparent.
                alpha_mode = entry.get("alphaMode") or "OPAQUE"

            if not indices:
                indices = list(range(len(converted) // 3))
            primitives.append(
                {
                    "name": node_name or mesh.get("name") or "",
                    "positions": converted,
                    "uvs": uvs,
                    "indices": indices,
                    "image": image_path,
                    "colour": colour,
                    "double_sided": double_sided,
                    "alpha_mode": alpha_mode,
                }
            )
    return primitives






