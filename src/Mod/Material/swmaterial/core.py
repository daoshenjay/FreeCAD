# SPDX-License-Identifier: LGPL-2.1-or-later

"""Core material helpers for the material dock.

Nothing in here imports Qt, so the same code can be driven from a script::

    import swmaterial.core as ms
    entry = ms.find("Copper")
    ms.apply_material(App.ActiveDocument.Box, entry)

The material list comes from FreeCAD's material system (``import Materials``).
When that module is unavailable a small built-in palette is used instead.
"""

import os

import FreeCAD as App

try:  # the new material system, available in FreeCAD 1.0 and later
    import Materials
except Exception:  # pragma: no cover - only when the Material module is missing
    Materials = None


# --- fallback palette -------------------------------------------------------
# name, diffuse colour, shininess
FALLBACK_MATERIALS = [
    ("Steel", (0.55, 0.57, 0.60, 1.0), 0.65),
    ("Aluminium", (0.78, 0.79, 0.80, 1.0), 0.55),
    ("Copper", (0.85, 0.55, 0.25, 1.0), 0.85),
    ("Brass", (0.87, 0.72, 0.35, 1.0), 0.80),
    ("Gold", (0.95, 0.78, 0.30, 1.0), 0.90),
    ("Chrome", (0.85, 0.87, 0.90, 1.0), 0.95),
    ("Plastic red", (0.85, 0.15, 0.15, 1.0), 0.40),
    ("Plastic blue", (0.15, 0.35, 0.85, 1.0), 0.40),
    ("Rubber", (0.15, 0.15, 0.15, 1.0), 0.10),
    ("Wood", (0.60, 0.40, 0.20, 1.0), 0.20),
]


def _parse_color(value, default=(0.8, 0.8, 0.8, 1.0)):
    """Return an (r, g, b, a) tuple from whatever the material system gives us."""
    if value is None:
        return default
    if isinstance(value, (tuple, list)):
        values = list(value)
    else:
        text = str(value).strip().strip("()[]")
        values = [part for part in text.replace(";", ",").split(",") if part.strip()]
    try:
        numbers = [float(v) for v in values]
    except (TypeError, ValueError):
        return default
    if len(numbers) >= 4:
        return tuple(numbers[:4])
    if len(numbers) == 3:
        return tuple(numbers) + (1.0,)
    return default


def _parse_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


class MaterialEntry:
    """A material as shown in the workbench: a name plus appearance values."""

    __slots__ = (
        "name",
        "library",
        "uuid",
        "description",
        "ambient",
        "diffuse",
        "specular",
        "emissive",
        "shininess",
        "transparency",
    )

    def __init__(
        self,
        name,
        diffuse=(0.8, 0.8, 0.8, 1.0),
        ambient=None,
        specular=None,
        emissive=(0.0, 0.0, 0.0, 1.0),
        shininess=0.5,
        transparency=0.0,
        library="",
        uuid="",
        description="",
    ):
        self.name = name
        self.diffuse = _parse_color(diffuse)
        self.ambient = _parse_color(ambient, self.diffuse)
        self.specular = _parse_color(specular, (0.5, 0.5, 0.5, 1.0))
        self.emissive = _parse_color(emissive, (0.0, 0.0, 0.0, 1.0))
        self.shininess = _parse_float(shininess, 0.5)
        self.transparency = _parse_float(transparency, 0.0)
        self.library = library or ""
        self.uuid = uuid or ""
        self.description = description or ""

    def __repr__(self):
        return "MaterialEntry({!r})".format(self.name)

    def copy(self, **changes):
        clone = MaterialEntry(
            self.name,
            self.diffuse,
            self.ambient,
            self.specular,
            self.emissive,
            self.shininess,
            self.transparency,
            self.library,
            self.uuid,
            self.description,
        )
        for key, value in changes.items():
            setattr(clone, key, value)
        return clone

    def to_app_material(self, transparency=None, shininess=None):
        """Build the ``App.Material`` that a view provider understands."""
        mat = App.Material()
        mat.AmbientColor = self.ambient
        mat.DiffuseColor = self.diffuse
        mat.SpecularColor = self.specular
        mat.EmissiveColor = self.emissive
        mat.Shininess = self.shininess if shininess is None else float(shininess)
        mat.Transparency = self.transparency if transparency is None else float(transparency)
        return mat


def _entry_from_library_material(material):
    """Convert a ``Materials.Material`` into a MaterialEntry.

    Returns None for materials without any appearance information (gases,
    costs, machining data, ...), they cannot be shown as a colour swatch.
    """
    try:
        if not material.hasAppearanceProperty("DiffuseColor"):
            return None
    except Exception:
        return None
    name = getattr(material, "Name", "") or "Material"
    values = {}
    for key in ("AmbientColor", "DiffuseColor", "SpecularColor", "EmissiveColor"):
        try:
            values[key] = material.getAppearanceValue(key)
        except Exception:
            values[key] = None
    try:
        shininess = material.getAppearanceValue("Shininess")
    except Exception:
        shininess = 0.5
    try:
        transparency = material.getAppearanceValue("Transparency")
    except Exception:
        transparency = 0.0
    return MaterialEntry(
        name,
        diffuse=_parse_color(values.get("DiffuseColor"), (0.8, 0.8, 0.8, 1.0)),
        ambient=_parse_color(values.get("AmbientColor"), values.get("DiffuseColor")),
        specular=_parse_color(values.get("SpecularColor"), (0.5, 0.5, 0.5, 1.0)),
        emissive=_parse_color(values.get("EmissiveColor"), (0.0, 0.0, 0.0, 1.0)),
        shininess=_parse_float(shininess, 0.5),
        transparency=_parse_float(transparency, 0.0),
        library=getattr(material, "LibraryName", "") or "",
        uuid=getattr(material, "UUID", "") or "",
        description=getattr(material, "Description", "") or "",
    )


_CACHE = None


def load_materials(force=False):
    """Return the list of materials offered by the workbench.

    The FreeCAD material libraries are used when possible; the built-in palette
    is appended so the workbench is usable even with an empty library.
    """
    global _CACHE
    if _CACHE is not None and not force:
        return _CACHE

    entries = []
    if Materials is not None:
        try:
            manager = Materials.MaterialManager()
            for material in manager.Materials.values():
                try:
                    entry = _entry_from_library_material(material)
                except Exception:
                    continue
                if entry is not None:
                    entries.append(entry)
        except Exception:
            entries = []
        entries.sort(key=lambda item: (item.library.lower(), item.name.lower()))

    known = {item.name.lower() for item in entries}
    for name, color, shininess in FALLBACK_MATERIALS:
        if name.lower() not in known:
            entries.append(
                MaterialEntry(name, diffuse=color, shininess=shininess, library="Built-in")
            )

    _CACHE = entries
    return entries


def find(name, materials=None):
    """Look a material up by name (case-insensitive, first match wins)."""
    if not name:
        return None
    wanted = name.lower()
    for entry in materials if materials is not None else load_materials():
        if entry.name.lower() == wanted:
            return entry
    return None


# --- applying materials -----------------------------------------------------


def can_take_material(obj):
    """True when the object has a view provider with a ShapeAppearance property."""
    view = getattr(obj, "ViewObject", None)
    return view is not None and hasattr(view, "ShapeAppearance")


def filter_targets(objects):
    """Keep only objects that can receive a material."""
    return [obj for obj in objects if can_take_material(obj)]


def face_count(obj):
    """Number of faces of a shape-like object, 0 when it has no Shape."""
    shape = getattr(obj, "Shape", None)
    try:
        return len(shape.Faces)
    except Exception:
        return 0


def apply_material(obj, entry, faces=None, transparency=None, shininess=None):
    """Assign ``entry`` to ``obj``.

    ``faces`` is an optional list of zero-based face indices; when given only
    those faces get the material and the remaining faces keep the default one.
    Returns True when something was assigned.
    """
    if not can_take_material(obj):
        return False
    view = obj.ViewObject
    mat = entry.to_app_material(transparency=transparency, shininess=shininess)

    if faces:
        total = face_count(obj)
        if total <= 0:
            total = max(faces) + 1
        materials = [entry.to_app_material() for _ in range(total)]
        for index in faces:
            if 0 <= index < total:
                materials[index] = mat
        view.ShapeAppearance = materials
    else:
        view.ShapeAppearance = [mat]
    return True


def shades(entry, count=6):
    """Return ``count`` tints/shades of ``entry``, used for the per-face mode."""
    result = []
    for index in range(max(1, count)):
        factor = 1.0 - 0.55 * (index / max(1, count - 1)) if count > 1 else 1.0
        color = tuple(min(1.0, max(0.0, channel * factor)) for channel in entry.diffuse[:3])
        clone = entry.copy()
        clone.diffuse = color + (entry.diffuse[3],)
        clone.ambient = tuple(c * 0.3 for c in color) + (1.0,)
        clone.name = "{} {}".format(entry.name, index + 1)
        result.append(clone)
    return result


def apply_palette(obj, entry, count=None):
    """Give every face its own shade of ``entry`` (a quick "material map" look)."""
    if not can_take_material(obj):
        return False
    total = face_count(obj) or (count or 6)
    palette = shades(entry, total)
    obj.ViewObject.ShapeAppearance = [item.to_app_material() for item in palette]
    return True


def clear_material(obj):
    """Reset the appearance to FreeCAD's default material."""
    if not can_take_material(obj):
        return False
    obj.ViewObject.ShapeAppearance = [App.Material()]
    return True


def describe(entry):
    """One-line summary used in tooltips and the status bar."""
    color = "#{:02x}{:02x}{:02x}".format(
        int(entry.diffuse[0] * 255), int(entry.diffuse[1] * 255), int(entry.diffuse[2] * 255)
    )
    return "{} | {} | shininess {:.2f} | transparency {:.2f}".format(
        entry.name, color, entry.shininess, entry.transparency
    )

