# SPDX-License-Identifier: LGPL-2.1-or-later

"""The built-in texture library shipped with the material dock.

Layout on disk (under ``Resources/textures``)::

    textures/
        bump/      *.jpg *.png ...    # height / bump / concrete style maps
        pattern/   *.jpg *.png ...    # colour patterns (brick, paving, timber, ...)
        _thumbs/   bump/, pattern/    # 96x96 previews, generated when packaging

Every sub folder that does not start with ``_`` is one category; drop more
folders (or more images) in there and they show up in the browser on the next
start.  Thumbnails are optional: the browser falls back to a placeholder icon.
"""

import os

from . import paths

IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp", ".gif")

CATEGORY_LABELS = {
    "bump": "凹凸 / bump",
    "pattern": "图案 / pattern",
}


class TextureItem:
    """One entry of the library."""

    __slots__ = ("name", "path", "thumb", "category")

    def __init__(self, name, path, thumb, category):
        self.name = name
        self.path = path
        self.thumb = thumb
        self.category = category

    def __repr__(self):
        return "TextureItem({!r})".format(self.name)


def categories():
    """Category folder names, in a stable order (bump first, then alphabetical)."""
    root = paths.texture_dir()
    if not root:
        return []
    found = [
        name
        for name in os.listdir(root)
        if not name.startswith("_") and os.path.isdir(os.path.join(root, name))
    ]
    preferred = [name for name in CATEGORY_LABELS if name in found]
    rest = sorted(name for name in found if name not in preferred)
    return preferred + rest


def category_label(category):
    return CATEGORY_LABELS.get(category, category)


def _thumb_path(category, filename):
    root = paths.thumb_dir()
    if not root:
        return ""
    candidate = os.path.join(root, category, os.path.splitext(filename)[0] + ".jpg")
    return candidate if os.path.isfile(candidate) else ""


def list_textures(category=None):
    """All textures of one category (or of every category)."""
    root = paths.texture_dir()
    if not root:
        return []
    result = []
    for name in categories() if category is None else [category]:
        folder = os.path.join(root, name)
        if not os.path.isdir(folder):
            continue
        for filename in sorted(os.listdir(folder), key=lambda item: item.lower()):
            if os.path.splitext(filename)[1].lower() not in IMAGE_EXTENSIONS:
                continue
            full = os.path.join(folder, filename)
            if not os.path.isfile(full):
                continue
            result.append(
                TextureItem(
                    os.path.splitext(filename)[0],
                    full,
                    _thumb_path(name, filename),
                    name,
                )
            )
    return result


def count(category=None):
    return len(list_textures(category))
