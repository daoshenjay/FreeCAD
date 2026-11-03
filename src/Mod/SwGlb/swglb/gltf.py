# SPDX-License-Identifier: LGPL-2.1-or-later
"""Minimal, dependency free glTF 2.0 / GLB writer.

Only what the exporter needs:

* float32 POSITION / NORMAL / TEXCOORD_0 accessors,
* uint32 indices,
* JPEG/PNG images embedded in the binary chunk,
* one PBR material per image (``baseColorTexture``) or per plain colour
  (``baseColorFactor``).

The file layout follows the glTF 2.0 specification: a 12 byte header, a JSON
chunk padded with spaces and a BIN chunk padded with zeros, both 4 byte aligned.
"""

import array
import json
import struct

FLOAT = 5126
UNSIGNED_INT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963

REPEAT = 10497
LINEAR = 9729
LINEAR_MIPMAP = 9987


class GlbWriter:
    """Collects buffers/accessors and serialises them as one .glb file."""

    def __init__(self, generator="SwGlb"):
        self.bin = bytearray()
        self.views = []
        self.accessors = []
        self.images = []
        self.textures = []
        self.materials = []
        self.meshes = []
        self.nodes = []
        self.generator = generator
        self._image_index = {}
        self._material_index = {}

    # -- buffers ---------------------------------------------------------
    def _pad(self):
        while len(self.bin) % 4:
            self.bin.append(0)

    def _view(self, payload, target):
        self._pad()
        offset = len(self.bin)
        self.bin.extend(payload)
        view = {
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(payload),
            "target": target,
        }
        self.views.append(view)
        return len(self.views) - 1

    def _float_view(self, values, target):
        payload = array.array("f", values).tobytes()
        return self._view(payload, target)

    def _accessor(self, view, component, count, kind, minimum=None, maximum=None):
        accessor = {
            "bufferView": view,
            "componentType": component,
            "count": count,
            "type": kind,
        }
        if minimum is not None:
            accessor["min"] = minimum
            accessor["max"] = maximum
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def add_positions(self, points):
        """``points`` is a flat list of floats (x, y, z, ...)."""
        view = self._float_view(points, ARRAY_BUFFER)
        count = len(points) // 3
        xs, ys, zs = points[0::3], points[1::3], points[2::3]
        minimum = [min(xs), min(ys), min(zs)]
        maximum = [max(xs), max(ys), max(zs)]
        return self._accessor(view, FLOAT, count, "VEC3", minimum, maximum)

    def add_vectors(self, values, size):
        view = self._float_view(values, ARRAY_BUFFER)
        return self._accessor(view, FLOAT, len(values) // size, "VEC2" if size == 2 else "VEC3")

    def add_indices(self, values):
        payload = array.array("I", values).tobytes()
        view = self._view(payload, ELEMENT_ARRAY_BUFFER)
        return self._accessor(view, UNSIGNED_INT, len(values), "SCALAR")

    # -- images / materials ---------------------------------------------
    def add_image(self, path, payload, mime):
        key = path
        if key in self._image_index:
            return self._image_index[key]
        view = self._view(payload, None)
        image = {"bufferView": view, "mimeType": mime, "name": path.rsplit("/", 1)[-1]}
        self.images.append(image)
        self.textures.append({"sampler": 0, "source": len(self.images) - 1})
        self._image_index[key] = len(self.textures) - 1
        return self._image_index[key]

    def add_texture_material(self, texture_index, name="texture"):
        key = ("tex", texture_index)
        if key in self._material_index:
            return self._material_index[key]
        material = {
            "name": name,
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": texture_index},
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
        }
        self.materials.append(material)
        self._material_index[key] = len(self.materials) - 1
        return self._material_index[key]

    def add_color_material(self, colour, name="colour"):
        """``colour`` is a linear RGBA tuple."""
        key = ("col", tuple(round(float(value), 4) for value in colour))
        if key in self._material_index:
            return self._material_index[key]
        material = {
            "name": name,
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": list(key[1][:4]),
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
        }
        self.materials.append(material)
        self._material_index[key] = len(self.materials) - 1
        return self._material_index[key]

    # -- meshes ----------------------------------------------------------
    def add_mesh(self, name, primitives):
        self.meshes.append({"name": name, "primitives": primitives})
        return len(self.meshes) - 1

    def add_node(self, name, mesh, translation=None):
        node = {"name": name, "mesh": mesh}
        if translation:
            node["translation"] = list(translation)
        self.nodes.append(node)
        return len(self.nodes) - 1

    # -- output ----------------------------------------------------------
    def build(self, scene_name="Scene", extras=None):
        scene = {"name": scene_name, "nodes": list(range(len(self.nodes)))}
        if extras is not None:
            # e.g. [236200.0, 0.0, -3970003.0]: the metre offset every
            # position was shifted by before writing (see export._world_offset)
            scene["extras"] = [float(value) for value in extras]
        document = {
            "asset": {"version": "2.0", "generator": self.generator},
            "scene": 0,
            "scenes": [scene],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "accessors": self.accessors,
            "bufferViews": self.views,
            "buffers": [{"byteLength": len(self.bin)}],
        }
        if self.materials:
            document["materials"] = self.materials
        if self.textures:
            document["textures"] = self.textures
            document["images"] = self.images
            document["samplers"] = [
                {"wrapS": REPEAT, "wrapT": REPEAT, "magFilter": LINEAR, "minFilter": LINEAR_MIPMAP}
            ]

        json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
        while len(json_chunk) % 4:
            json_chunk += b" "
        bin_chunk = bytes(self.bin)
        while len(bin_chunk) % 4:
            bin_chunk += b"\x00"

        total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
        header = struct.pack("<III", 0x46546C67, 2, total)
        body = struct.pack("<II", len(json_chunk), 0x4E4F534A) + json_chunk
        body += struct.pack("<II", len(bin_chunk), 0x004E4942) + bin_chunk
        return header + body

    def save(self, path, scene_name="Scene", extras=None):
        payload = self.build(scene_name, extras)
        with open(path, "wb") as handle:
            handle.write(payload)
        return len(payload)

