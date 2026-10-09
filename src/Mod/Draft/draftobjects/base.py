# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2009, 2010 Yorik van Havre <yorik@uncreated.net>        *
# *   Copyright (c) 2009, 2010 Ken Cline <cline@frii.com>                   *
# *   Copyright (c) 2019 Eliud Cabrera Castillo <e.cabrera-castillo@tum.de> *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   This program is distributed in the hope that it will be useful,       *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with this program; if not, write to the Free Software   *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************
"""Provides the object code for the base Draft object."""

## @package base
# \ingroup draftobjects
# \brief Provides the object code for the base Draft object.

## \addtogroup draftobjects
# @{

import struct

import FreeCAD as App

# Beyond this magnitude the float32 vertex lattice step (x * 2**-23) is
# already about 0.008 units, coarse enough to visibly collapse fine
# geometry such as a spline drawn far from the origin.
_FAR_ANCHOR_THRESHOLD = 2**16


def _f32(value):
    """Round-trip through float32, matching Coin SoSFVec3f field storage."""
    return struct.unpack("f", struct.pack("f", float(value)))[0]


class DraftObject(object):
    """The base class for Draft objects.

    Parameters
    ----------
    obj : a base C++ object
        The base object instantiated during creation,
        which commonly may be of types `Part::Part2DObjectPython`,
        `Part::FeaturePython`, or `App::FeaturePython`.

            >>> obj = App.ActiveDocument.addObject('Part::Part2DObjectPython')
            >>> DraftObject(obj)

        This class instance is stored in the `Proxy` attribute
        of the base object.
        ::
            obj.Proxy = self

    tp : str, optional
        It defaults to `'Unknown'`.
        It indicates the type of this scripted object,
        which will be assigned to the Proxy's `Type` attribute.

        This is useful to distinguish different types of scripted objects
        that may be derived from the same C++ class.

    Attributes
    ----------
    Type : str
        It indicates the type of scripted object.
        Normally `Type = tp`.

        All objects have a `TypeId` attribute, but this attribute
        refers to the C++ class only. Using the `Type` attribute
        allows distinguishing among various types of objects
        derived from the same C++ class.

            >>> print(obj.TypeId, "->", obj.Proxy.Type)
            Part::Part2DObjectPython -> Circle

    This class attribute is accessible through the `Proxy` object:
    `obj.Proxy.Type`.
    """

    def __init__(self, obj, tp="Unknown"):
        # This class is assigned to the Proxy attribute
        if obj:
            obj.Proxy = self
        self.Type = tp

    def onDocumentRestored(self, obj):
        # Object properties are updated when the document is opened.
        self.props_changed_clear()
        if self._far_anchor_candidate(obj) is not None:
            # The stored Shape still holds the collapsed world-space
            # vertices; anchor_far_points rewrites the data and execute
            # rebuilds the Shape, so only mark the object here.
            obj.touch()

    def _far_anchor_candidate(self, obj):
        """Return the far reference point, or None when no anchor is needed.

        Only Points-driven geometry with an identity Placement qualifies:
        a non-identity Placement already localizes the stored shape
        vertices, a Base link replaces the geometry wholesale, and a
        MapMode placement owns the Placement and must not be overwritten.
        """
        if not hasattr(obj, "Points") or not obj.Points:
            return None
        if getattr(obj, "Base", None):
            return None
        if getattr(obj, "MapMode", "Deactivated") not in ("Deactivated", None):
            return None
        if not (obj.Placement == App.Placement()):
            return None
        for p in obj.Points:
            if max(abs(p.x), abs(p.y), abs(p.z)) >= _FAR_ANCHOR_THRESHOLD:
                return p
        return None

    def anchor_far_points(self, obj):
        """Pose far-from-origin Points as local geometry with a Placement anchor.

        The part view provider strips the shape location and stores the
        vertices as float32, so absolute world vertices snap to the
        float32 lattice (64 units at 1e9) and a small curve collapses
        into a single cell. Migrating to local Points plus a
        float32-clean Placement anchor keeps every stored addend
        viewport-sized; the view provider's vertex mirror re-applies the
        anchor's sub-lattice remainder, landing the rendered result on
        the double-precision world coordinates again.

        This restores the invariant make_wire already establishes with a
        placement (Points are local to Placement), which the Draft
        modify functions assume. Returns True when the data changed.
        """
        ref = self._far_anchor_candidate(obj)
        if ref is None:
            return False
        anchor = App.Vector(_f32(ref.x), _f32(ref.y), _f32(ref.z))
        obj.Points = [p.sub(anchor) for p in obj.Points]
        obj.Placement = App.Placement(anchor, App.Rotation())
        return True

    def dumps(self):
        """Return a tuple of all serializable objects or None.

        When saving the document this object gets stored
        using Python's `json` module.

        Override this method to define the serializable objects to return.

        By default it returns the `Type` attribute.

        Returns
        -------
        str
            Returns the value of `Type`
        """
        return self.Type

    def loads(self, state):
        """Set some internal properties for all restored objects.

        When a document is restored this method is used to set some properties
        for the objects stored with `json`.

        Override this method to define the properties to change for the
        restored serialized objects.

        By default the `Type` was serialized, so `state` holds this value,
        which is re-assigned to the `Type` attribute.
        ::
            self.Type = state

        Parameters
        ----------
        state : state
            A serialized object.
        """
        if state:
            self.Type = state

    def execute(self, obj):
        """Run this method when the object is created or recomputed.

        Override this method to produce effects when the object
        is newly created, and whenever the document is recomputed.

        By default it does nothing.

        Parameters
        ----------
        obj : the scripted object.
            This commonly may be of types `Part::Part2DObjectPython`,
            `Part::FeaturePython`, or `App::FeaturePython`.
        """
        pass

    def onChanged(self, obj, prop):
        """Run this method when a property is changed.

        Override this method to handle the behavior
        of the object depending on changes that occur to its properties.

        By default it does nothing.

        Parameters
        ----------
        obj : the scripted object.
            This commonly may be of types `Part::Part2DObjectPython`,
            `Part::FeaturePython`, or `App::FeaturePython`.

        prop : str
            Name of the property that was modified.
        """
        pass

    def props_changed_store(self, prop):
        """Store the name of the property that will be changed in the
        self.props_changed list.

        The function should be called at the start of onChanged or onBeforeChange.

        The list is used to detect Placement-only changes. In such cases the
        Shape of an object does not need to be updated. Not updating the Shape
        avoids Uno/Redo issues for the Windows version and is also more efficient.
        """
        if not hasattr(self, "props_changed"):
            self.props_changed = [prop]
        else:
            self.props_changed.append(prop)

    def props_changed_clear(self):
        """Remove the self.props_changed attribute if it exists.

        The function should be called just before execute returns.
        """
        if hasattr(self, "props_changed"):
            delattr(self, "props_changed")

    def props_changed_placement_only(self, obj=None):
        """Return `True` if the self.props_changed list, after removing
        `_LinkTouched`, `Shape`, `Density`, `Volume` and `Mass` items,
        only contains `Placement` items.

        Parameters
        ----------
        obj : the scripted object. Need only be supplied if the Shape of obj
            is, or can be, derived from other objects.

        """
        if not hasattr(self, "props_changed"):
            return False

        # For some objects a dummy Placement property change (new and old
        # Placement are the same) is used in cases where a full recompute is
        # required. This function should then return `False`. The common
        # denominator seems to be a non-empty OutList.
        # https://github.com/FreeCAD/FreeCAD/issues/8771
        # https://forum.freecad.org/viewtopic.php?t=82436
        if obj is not None and obj.OutList:
            return False

        props = set(self.props_changed)
        for prop in ("_LinkTouched", "Shape", "Density", "Volume", "Mass"):
            if prop in props:
                props.remove(prop)
        return props == {"Placement"}


# Alias for compatibility with v0.18 and earlier
_DraftObject = DraftObject

## @}
