# SPDX-License-Identifier: LGPL-2.1-or-later

"""Texture mapping modes for the SwMaterial overlay.

The overlay builds its own textured mesh, so every mode is just a function
``(world point, face info, frame) -> (u, v)``:

* ``auto``         per face: cylindrical for cylinder faces, spherical for
  sphere faces, planar for the caps of a cylindrical body, box mapping
  otherwise (the comfortable default)
* ``cubic``        box mapping, the axis is chosen per face from the normal
* ``planar``       orthographic projection along ``direction``
* ``elevation``    planar projection from above (``direction`` forced to +Z)
* ``directional``  planar projection along any direction
* ``cylindrical``  wrap around ``direction`` as the cylinder axis
* ``spherical``    wrap around ``direction`` as the polar axis

``parametric`` (surface parameter space) and ``solid`` (3D texture) are not
implemented yet.
"""

import json
import math

import FreeCAD as App

AUTO = "auto"
CUBIC = "cubic"
PARAMETRIC = "parametric"
PLANAR = "planar"
ELEVATION = "elevation"
DIRECTIONAL = "directional"
CYLINDRICAL = "cylindrical"
SPHERICAL = "spherical"

MODES = [
    (AUTO, "自动 / auto（按面）"),
    (CUBIC, "盒式 / cubic"),
    (PARAMETRIC, "参数域 / parametric"),
    (PLANAR, "平面 / planar"),
    (ELEVATION, "高程投影 / elevation drape"),
    (DIRECTIONAL, "定向投影 / directional drape"),
    (CYLINDRICAL, "柱面 / cylindrical"),
    (SPHERICAL, "球面 / spherical"),
]

MODE_IDS = {mode for mode, _label in MODES}

CAP_DOT = 0.9  # |normal . axis| above this counts as "cap" for the auto mode


def label(mode):
    for identifier, text in MODES:
        if identifier == mode:
            return text
    return mode


def _vector(value, default=(0.0, 0.0, 0.0)):
    if isinstance(value, App.Vector):
        return App.Vector(value)
    try:
        values = list(value)
    except TypeError:
        return App.Vector(*default)
    if len(values) < 3:
        values = list(default)
    return App.Vector(float(values[0]), float(values[1]), float(values[2]))


def _corners(box):
    return [
        App.Vector(x, y, z)
        for x in (box.XMin, box.XMax)
        for y in (box.YMin, box.YMax)
        for z in (box.ZMin, box.ZMax)
    ]


def face_info(face):
    """What the mapping needs to know about one face."""
    info = {
        "normal": face.normalAt(0, 0),
        "kind": "other",
        "axis": None,
        "center": None,
        "radius": 0.0,
        "surface": None,
        "prange": None,
    }
    try:
        surface = face.Surface
    except Exception:
        return info
    info["surface"] = surface
    try:
        info["prange"] = tuple(face.ParameterRange)
    except Exception:
        info["prange"] = None
    kind = type(surface).__name__.lower()
    if kind == "cylinder":
        info.update(kind="cylinder", axis=surface.Axis, center=surface.Center, radius=surface.Radius)
    elif kind == "sphere":
        info.update(kind="sphere", center=surface.Center, radius=surface.Radius)
    elif kind == "plane":
        info["kind"] = "plane"
        info["axis"] = surface.Axis
    return info


def uses_wrap(mode, info=None):
    """True when the resulting u coordinate can jump across the seam."""
    if mode in (CYLINDRICAL, SPHERICAL):
        return True
    if mode == AUTO and info is not None:
        return info.get("kind") in ("cylinder", "sphere")
    return False


WRAP_REPEAT = "repeat"
WRAP_CLAMP = "clamp"
WRAP_MIRROR = "mirror"

WRAPS = [
    (WRAP_REPEAT, "重复 / repeat"),
    (WRAP_CLAMP, "钳制 / clamp"),
    (WRAP_MIRROR, "镜像 / mirror"),
]


class Mapping:
    """One mapping mode, the frame it is evaluated in and the UV adjustments."""

    __slots__ = (
        "mode",
        "origin",
        "direction",
        "rotation",
        "scale_u",
        "scale_v",
        "offset_u",
        "offset_v",
        "flip_u",
        "flip_v",
        "swap_uv",
        "wrap",
        "tile",
        "fit",
    )

    def __init__(
        self,
        mode=AUTO,
        origin=None,
        direction=(0.0, 0.0, 1.0),
        rotation=0.0,
        scale=1.0,
        scale_u=None,
        scale_v=None,
        offset_u=0.0,
        offset_v=0.0,
        flip_u=False,
        flip_v=False,
        swap_uv=False,
        wrap=WRAP_REPEAT,
        tile=0.0,
        fit=True,
    ):
        self.mode = mode if mode in MODE_IDS else AUTO
        # origin None means "centre of the object's bounding box"
        self.origin = None if origin is None else _vector(origin)
        self.direction = _vector(direction, (0.0, 0.0, 1.0))
        self.rotation = float(rotation)
        base = max(0.001, float(scale))
        self.scale_u = max(0.001, float(scale_u)) if scale_u is not None else base
        self.scale_v = max(0.001, float(scale_v)) if scale_v is not None else base
        self.offset_u = float(offset_u)
        self.offset_v = float(offset_v)
        self.flip_u = bool(flip_u)
        self.flip_v = bool(flip_v)
        self.swap_uv = bool(swap_uv)
        self.wrap = wrap if wrap in {item[0] for item in WRAPS} else WRAP_REPEAT
        # real-world size: one image covers ``tile`` millimetres
        self.tile = max(0.0, float(tile or 0.0))
        self.fit = bool(fit) or self.tile <= 0.0

    @property
    def scale(self):
        """Uniform scale (legacy name used by the TextureScale property)."""
        return self.scale_u

    # -- serialisation ---------------------------------------------------
    def to_dict(self):
        return {
            "mode": self.mode,
            "origin": None if self.origin is None else [self.origin.x, self.origin.y, self.origin.z],
            "direction": [self.direction.x, self.direction.y, self.direction.z],
            "rotation": self.rotation,
            "scale": self.scale_u,
            "scale_u": self.scale_u,
            "scale_v": self.scale_v,
            "offset_u": self.offset_u,
            "offset_v": self.offset_v,
            "flip_u": self.flip_u,
            "flip_v": self.flip_v,
            "swap_uv": self.swap_uv,
            "wrap": self.wrap,
            "tile": self.tile,
            "fit": self.fit,
        }

    def to_json(self):
        return json.dumps(self.to_dict())

    @classmethod
    def from_dict(cls, data):
        if not isinstance(data, dict):
            return cls()
        return cls(
            mode=data.get("mode", AUTO),
            origin=data.get("origin"),
            direction=data.get("direction", (0.0, 0.0, 1.0)),
            rotation=data.get("rotation", 0.0),
            scale=data.get("scale", 1.0),
            scale_u=data.get("scale_u"),
            scale_v=data.get("scale_v"),
            offset_u=data.get("offset_u", 0.0),
            offset_v=data.get("offset_v", 0.0),
            flip_u=data.get("flip_u", False),
            flip_v=data.get("flip_v", False),
            swap_uv=data.get("swap_uv", False),
            wrap=data.get("wrap", WRAP_REPEAT),
            tile=data.get("tile", 0.0),
            fit=data.get("fit", True),
        )

    @classmethod
    def from_json(cls, text):
        if not text:
            return cls()
        try:
            return cls.from_dict(json.loads(text))
        except (ValueError, TypeError):
            return cls()

    def copy(self):
        return Mapping(
            self.mode,
            self.origin,
            self.direction,
            self.rotation,
            scale_u=self.scale_u,
            scale_v=self.scale_v,
            offset_u=self.offset_u,
            offset_v=self.offset_v,
            flip_u=self.flip_u,
            flip_v=self.flip_v,
            swap_uv=self.swap_uv,
            wrap=self.wrap,
            tile=self.tile,
            fit=self.fit,
        )

    # -- frame -----------------------------------------------------------
    def frame(self, shape):
        """Resolve the frame (origin, basis, size, bounds) for a shape."""
        box = shape.BoundBox
        center = App.Vector(
            (box.XMin + box.XMax) / 2.0,
            (box.YMin + box.YMax) / 2.0,
            (box.ZMin + box.ZMax) / 2.0,
        )
        size = max(box.XLength, box.YLength, box.ZLength) or 1.0
        if not self.fit and self.tile > 0.0:
            size = self.tile
        origin = self.origin if self.origin is not None else center

        axis = App.Vector(self.direction)
        if axis.Length < 1e-9:
            axis = App.Vector(0, 0, 1)
        axis.normalize()
        if self.mode == ELEVATION:
            axis = App.Vector(0, 0, 1)

        reference = App.Vector(0, 0, 1)
        if abs(axis.dot(reference)) > 0.99:
            reference = App.Vector(0, 1, 0)
        right = reference.cross(axis)
        right.normalize()
        up = axis.cross(right)
        up.normalize()

        # Rotation is applied to the resulting UV (see transform()), so it works
        # for every mode -- including cubic and parametric, which do not use
        # this basis at all.
        corners = _corners(box)
        has_cylinder = False
        for face in shape.Faces:
            try:
                if type(face.Surface).__name__.lower() == "cylinder":
                    has_cylinder = True
                    break
            except Exception:
                continue

        return {
            "origin": origin,
            "axis": axis,
            "right": right,
            "up": up,
            "size": size,
            "scale": self.scale,
            "corners": corners,
            "has_cylinder": has_cylinder,
        }

    # -- per vertex uv ---------------------------------------------------
    def uv(self, point, info, frame):
        """Raw (u, v) of one vertex, before the user adjustments."""
        mode = self.mode

        if mode == AUTO:
            return _auto(point, info, frame)
        if mode == CUBIC:
            return _cubic(point, info["normal"], frame)
        if mode == PARAMETRIC:
            return _parametric(point, info, frame)
        if mode in (PLANAR, ELEVATION, DIRECTIONAL):
            return _planar(point, frame["origin"], frame["right"], frame["up"], frame["size"])
        if mode == CYLINDRICAL:
            axial_min, axial_max = _axial_bounds(frame["axis"], frame)
            return _cylindrical(
                point,
                frame["origin"],
                frame["axis"],
                frame["right"],
                frame["up"],
                axial_min,
                axial_max,
                tile=frame.get("tile", 0.0),
                radius=float(info.get("radius") or 0.0),
            )
        if mode == SPHERICAL:
            return _spherical(
                point,
                frame["origin"],
                frame["axis"],
                frame["right"],
                frame["up"],
                tile=frame.get("tile", 0.0),
            )
        return _auto(point, info, frame)

    def transform(self, u, v, kind=None):
        """Apply rotation / swap / flip / scale / offset / wrap to a raw uv pair."""
        if self.swap_uv:
            u, v = v, u
        if self.rotation:
            # Texture-space rotation.  Modes whose raw uv is 0..1 across the
            # surface rotate around the middle of the face; world projection
            # modes rotate around the mapping origin (their uv origin).
            centre = 0.5 if (
                self.mode in (PARAMETRIC, CYLINDRICAL, SPHERICAL) or kind in ("cylinder", "sphere")
            ) else 0.0
            angle = math.radians(self.rotation)
            cos_a, sin_a = math.cos(angle), math.sin(angle)
            u -= centre
            v -= centre
            u, v = u * cos_a - v * sin_a, u * sin_a + v * cos_a
            u += centre
            v += centre
        if self.flip_u:
            u = 1.0 - u
        if self.flip_v:
            v = 1.0 - v
        u = u * self.scale_u + self.offset_u
        v = v * self.scale_v + self.offset_v
        if self.wrap == WRAP_CLAMP:
            u = min(1.0, max(0.0, u))
            v = min(1.0, max(0.0, v))
        elif self.wrap == WRAP_MIRROR:
            u = _mirror(u)
            v = _mirror(v)
        return u, v


def _mirror(value):
    value = value % 2.0
    return value if value <= 1.0 else 2.0 - value


def _auto(point, info, frame):
    kind = info.get("kind")
    if kind == "cylinder":
        axis = info["axis"] if info["axis"] is not None else frame["axis"]
        origin = info["center"] if info["center"] is not None else frame["origin"]
        right, up = _basis(axis)
        axial_min, axial_max = _axial_bounds(axis, frame, origin)
        return _cylindrical(point, origin, axis, right, up, axial_min, axial_max, tile=frame.get("tile", 0.0), radius=float(info.get("radius") or 0.0))
    if kind == "sphere":
        origin = info["center"] if info["center"] is not None else frame["origin"]
        return _spherical(point, origin, frame["axis"], frame["right"], frame["up"])
    if kind == "plane" and frame["has_cylinder"]:
        normal = info["normal"]
        if abs(normal.normalize().dot(frame["axis"])) > CAP_DOT:
            right, up = _basis(normal)
            return _planar(point, frame["origin"], right, up, frame["size"])
    return _cubic(point, info["normal"], frame)


def _basis(axis):
    """Orthonormal (right, up) pair perpendicular to ``axis``."""
    reference = App.Vector(0, 0, 1)
    if abs(axis.dot(reference)) > 0.99:
        reference = App.Vector(0, 1, 0)
    right = reference.cross(axis)
    right.normalize()
    up = axis.cross(right)
    up.normalize()
    return right, up


def _axial_bounds(axis, frame, origin=None):
    if origin is None:
        origin = frame["origin"]
    values = [(corner - origin).dot(axis) for corner in frame["corners"]]
    return min(values), max(values)


def _parametric(point, info, frame):
    """Textures the face's own (u, v) parameter domain, 0..1 across the range."""
    surface = info.get("surface")
    prange = info.get("prange")
    if surface is None or not prange or len(prange) < 4:
        return _cubic(point, info["normal"], frame)
    try:
        u, v = surface.parameter(point)
    except Exception:
        return _cubic(point, info["normal"], frame)
    u1, u2, v1, v2 = prange
    span_u = (u2 - u1) or 1.0
    span_v = (v2 - v1) or 1.0
    return (u - u1) / span_u, (v - v1) / span_v


def _planar(point, origin, right, up, size):
    delta = point - origin
    return delta.dot(right) / size, delta.dot(up) / size


def _cubic(point, normal, frame):
    """Box mapping: pick the dominant axis of the face normal."""
    size = frame["size"]
    delta = point - frame["origin"]
    nx, ny, nz = abs(normal.x), abs(normal.y), abs(normal.z)
    if nz >= nx and nz >= ny:
        return delta.x / size, delta.y / size
    if ny >= nx:
        return delta.x / size, delta.z / size
    return delta.y / size, delta.z / size


def _cylindrical(point, origin, axis, right, up, axial_min, axial_max, tile=0.0, radius=0.0):
    delta = point - origin
    axial = delta.dot(axis)
    radial = delta - axis * axial
    angle = math.atan2(radial.dot(up), radial.dot(right))
    if tile > 0.0:
        # real-world size: u follows the arc length, v follows the height
        radius = radius or radial.Length or 1.0
        return (angle * radius) / tile, (axial - axial_min) / tile
    u = angle / (2.0 * math.pi) + 0.5
    span = (axial_max - axial_min) or 1.0
    v = (axial - axial_min) / span
    return u, v


def _spherical(point, origin, axis, right, up, tile=0.0):
    delta = point - origin
    length = delta.Length
    if length < 1e-9:
        return 0.5, 0.5
    angle = math.atan2(delta.dot(up), delta.dot(right))
    cosine = max(-1.0, min(1.0, delta.dot(axis) / length))
    polar = math.acos(cosine)
    if tile > 0.0:
        return (angle * length) / tile, (polar * length) / tile
    return angle / (2.0 * math.pi) + 0.5, polar / math.pi


def fix_seam(values):
    """Unwrap u values of one face when they jump across the 0/1 seam.

    The values are sorted and the largest gap is located: if that gap is bigger
    than half the texture, the values are split there (that is the seam) and the
    low side is shifted by one period.  A face that really spans the whole turn
    has evenly spaced values, so its largest gap stays small and nothing moves.
    """
    if len(values) < 3:
        return values
    ordered = sorted(values)
    gaps = [(ordered[index + 1] - ordered[index], index) for index in range(len(ordered) - 1)]
    gaps.append(((ordered[0] + 1.0) - ordered[-1], len(ordered) - 1))
    biggest, index = max(gaps)
    if biggest <= 0.5:
        return values
    threshold = ordered[index] + 1e-9
    return [value + 1.0 if value <= threshold else value for value in values]



