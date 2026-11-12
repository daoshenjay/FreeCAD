# SPDX-License-Identifier: LGPL-2.1-or-later

"""Where the material dock finds its icons and built-in textures.

The code is part of the Material module now: Python files are installed into
``Mod/Material/swmaterial`` while the icons and the texture library are
installed below the Material resource directory (``<resource>/Mod/Material/
Resources``).  As a fallback the paths next to this package are used, which
keeps a source checkout usable without installing; there the icons sit in
``Gui/Resources/icons``.
"""

import os

import FreeCAD as App


def _module_roots():
    return [
        os.path.join(App.getResourceDir(), "Mod", "Material"),
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    ]


def _first_dir(*parts):
    for root in _module_roots():
        candidate = os.path.join(root, *parts)
        if os.path.isdir(candidate):
            return candidate
    return ""


def icon_dir():
    return _first_dir("Resources", "icons") or _first_dir("Gui", "Resources", "icons")


def icon(name):
    directory = icon_dir()
    return os.path.join(directory, name) if directory else name


def texture_dir():
    """Root folder of the built-in texture library (one sub folder per category)."""
    return _first_dir("Resources", "textures")


def thumb_dir():
    directory = os.path.join(texture_dir(), "_thumbs") if texture_dir() else ""
    return directory if directory and os.path.isdir(directory) else ""
