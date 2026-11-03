# SPDX-License-Identifier: LGPL-2.1-or-later

"""Where the workbench finds its icons and built-in textures.

FreeCAD installs Python files into ``Mod/SwMaterial`` but resources into
``<resource>/Mod/SwMaterial/Resources`` (``Data/Mod/...`` on Windows), so the
resource paths are resolved through ``App.getResourceDir()`` first and only
then next to the module itself (useful when running straight from a source
tree).
"""

import os

import FreeCAD as App


def _resource_root():
    candidates = [
        os.path.join(App.getResourceDir(), "Mod", "SwMaterial", "Resources"),
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Resources"),
    ]
    for candidate in candidates:
        if os.path.isdir(candidate):
            return candidate
    return ""


def icon_dir():
    root = _resource_root()
    directory = os.path.join(root, "icons") if root else ""
    return directory if directory and os.path.isdir(directory) else ""


def icon(name):
    directory = icon_dir()
    return os.path.join(directory, name) if directory else name


def texture_dir():
    """Root folder of the built-in texture library (one sub folder per category)."""
    root = _resource_root()
    directory = os.path.join(root, "textures") if root else ""
    return directory if directory and os.path.isdir(directory) else ""


def thumb_dir():
    directory = os.path.join(texture_dir(), "_thumbs") if texture_dir() else ""
    return directory if directory and os.path.isdir(directory) else ""
