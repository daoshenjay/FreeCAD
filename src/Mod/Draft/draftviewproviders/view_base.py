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
"""Provides the viewprovider code for the base Draft object.

Many viewprovider classes may inherit this class in order to have
the same basic behavior."""

## @package view_base
# \ingroup draftviewproviders
# \brief Provides the viewprovider code for the base Draft object.

## \addtogroup draftviewproviders
# @{
import struct

import PySide.QtCore as QtCore
import PySide.QtGui as QtGui
from PySide.QtCore import QT_TRANSLATE_NOOP

import FreeCAD as App
from draftutils import gui_utils
from draftutils import params
from draftutils import utils
from draftutils.translate import translate

if App.GuiUp:
    from pivy import coin
    import FreeCADGui as Gui
    import Draft_rc

    # The module is used to prevent complaints from code checkers (flake8)
    bool(Draft_rc.__name__)


def _f32(value):
    """Round-trip through float32, matching Coin SoSFVec3f field storage."""
    return struct.unpack("f", struct.pack("f", float(value)))[0]


class ViewProviderDraft(object):
    """The base class for Draft view providers.

    Parameters
    ----------
    vobj : a base C++ view provider
        The view provider of the scripted object (`obj.ViewObject`),
        which commonly may be of types `PartGui::ViewProvider2DObjectPython`,
        `PartGui::ViewProviderPython`, or `Gui::ViewProviderFeaturePython`.

        A basic view provider is instantiated during the creation
        of the base C++ object, for example,
        `Part::Part2DObjectPython`, `Part::FeaturePython`,
        or `App::FeaturePython`.

            >>> obj = App.ActiveDocument.addObject('Part::Part2DObjectPython')
            >>> vobj = obj.ViewObject
            >>> ViewProviderDraft(vobj)

        This view provider class instance is stored in the `Proxy` attribute
        of the base view provider.
        ::
            vobj.Proxy = self

    Attributes
    ----------
    Object : the base C++ object
        The scripted document object that is associated
        with this view provider, which commonly may be of types
        `Part::Part2DObjectPython`, `Part::FeaturePython`,
        or `App::FeaturePython`.

    texture : coin.SoTexture2
        A texture that could be added to this object.

    texcoords : coin.SoTextureCoordinatePlane
        The coordinates defining a plane to use for aligning the texture.

    These class attributes are accessible through the `Proxy` object:
    `vobj.Proxy.Object`, `vobj.Proxy.texture`, etc.
    """

    def __init__(self, vobj):
        self.Object = vobj.Object
        self.texture = None
        self.texcoords = None

        self._set_properties(vobj)
        # This class is assigned to the Proxy attribute
        vobj.Proxy = self

    def _set_properties(self, vobj):
        """Set the properties of objects if they don't exist."""
        if not hasattr(vobj, "Pattern"):
            vobj.addProperty(
                "App::PropertyEnumeration",
                "Pattern",
                "Draft",
                QT_TRANSLATE_NOOP("App::Property", "Defines an SVG pattern."),
                locked=True,
            )
            patterns = list(utils.svg_patterns())
            patterns.sort()
            vobj.Pattern = ["None"] + patterns

        if not hasattr(vobj, "PatternSize"):
            vobj.addProperty(
                "App::PropertyFloat",
                "PatternSize",
                "Draft",
                QT_TRANSLATE_NOOP("App::Property", "Defines the size of the SVG pattern."),
                locked=True,
            )
            vobj.PatternSize = params.get_param("HatchPatternSize")

    def dumps(self):
        """Return a tuple of all serializable objects or None.

        When saving the document this view provider object gets stored
        using Python's `json` module.

        Since we have some un-serializable objects (Coin objects) in here
        we must define this method to return a tuple of all serializable
        objects or `None`.

        Override this method to define the serializable objects to return.

        By default it returns `None`.

        Returns
        -------
        None
        """
        return None

    def loads(self, state):
        """Set some internal properties for all restored objects.

        When a document is restored this method is used to set some properties
        for the objects stored with `json`.

        Override this method to define the properties to change for the
        restored serialized objects.

        By default no objects were serialized with `dumps`,
        so nothing needs to be done here, and it returns `None`.

        Parameters
        ----------
        state : state
            A serialized object.

        Returns
        -------
        None
        """
        return None

    def attach(self, vobj):
        """Set up the scene sub-graph of the view provider.

        This method should always be defined, even if it does nothing.

        Override this method to set up a custom scene.

        Parameters
        ----------
        vobj : the view provider of the scripted object.
            This is `obj.ViewObject`.
        """
        self.texture = None
        self.texcoords = None
        self.Object = vobj.Object
        self.onChanged(vobj, "Pattern")
        self._attach_vertex_offset(vobj)
        return

    def updateData(self, obj, prop):
        """Run when an object property is changed.

        Override this method to handle the behavior of the view provider
        depending on changes that occur to the real object's properties.

        The `Placement` property is tracked so the vertex-space precision
        correction stays in sync; no other property is tested by default.

        Parameters
        ----------
        obj : the base C++ object
            The scripted document object that is associated
            with this view provider, which commonly may be of types
            `Part::Part2DObjectPython`, `Part::FeaturePython`,
            or `App::FeaturePython`.

        prop : str
            Name of the property that was modified.
        """
        if App.GuiUp and getattr(self, "_coord_source", None) is None:
            # The shape coordinates only become reachable once the C++
            # attach has run; bind on the first property update after
            # attach instead of waiting for a timer tick.
            try:
                self._bind_vertex_coords(obj.ViewObject)
            except Exception:
                pass
        if prop == "Placement":
            self._update_vertex_offset(obj)
        return

    def _attach_vertex_offset(self, vobj):
        """Set up the vertex-space float32 precision correction.

        Coin composes the model matrix chain in single precision: the
        Placement transform can only store ``float32(Base)``, and at 1e9
        the float32 lattice spacing is 64 units, so *no* translation node
        can carry the sub-lattice remainder (every float translation lands
        on the lattice while the double Placement does not).  The
        correction therefore lives in vertex space: a private
        ``SoCoordinate3`` node inserted right after the C++ shape
        coordinates mirrors their points shifted by
        ``R^-1 * (Base - float32(Base))``.  The renderer then composes
        ``float32(Base) + R * (v + correction)`` and lands on the exact
        double Placement while every addend stays viewport-sized and
        float32-exact.

        The C++ coordinates node is created by the C++ attach that runs
        right after this Python attach, so binding is retried on the next
        event loop turns and from ``updateData``.  A field sensor
        re-mirrors the points whenever C++ rewrites them (shape rebuild,
        display mode, document restore).

        Near the origin the remainder is sub-ULP and the mirror only
        copies the points unchanged.
        """
        if not App.GuiUp:
            return
        self._unbind_vertex_offset()
        root = vobj.RootNode
        if root is None:
            return
        # Drop nodes left behind by the earlier anchor-based revision.
        i = 0
        while i < root.getNumChildren():
            child = root.getChild(i)
            if child.getName() in ("PrecisionAnchor", "PrecisionCoordOffset"):
                root.removeChild(child)
            else:
                i += 1
        if hasattr(self, "_precision_anchor"):
            del self._precision_anchor
        bound = False
        try:
            # Never let a bind failure abort the Python attach: that would
            # also skip the C++ attach chained after it.
            bound = self._bind_vertex_coords(vobj)
        except Exception:
            bound = False
        if not bound:
            # The C++ attach inserts the shape coordinates right after
            # this Python attach; retry once the event loop turns.
            QtCore.QTimer.singleShot(0, lambda: self._retry_vertex_coords(vobj, 0))
        return

    def _retry_vertex_coords(self, vobj, attempt):
        """Retry binding until the C++ shape coordinates appear."""
        try:
            if self._bind_vertex_coords(vobj):
                return
        except Exception:
            return
        if attempt < 4:
            QtCore.QTimer.singleShot(0, lambda: self._retry_vertex_coords(vobj, attempt + 1))

    def _bind_vertex_coords(self, vobj):
        """Mirror the C++ shape coordinates through a private node.

        Returns True once the mirror and its field sensor are in place.
        """
        if getattr(self, "_coord_source", None) is not None:
            return True
        if not App.GuiUp:
            return False
        root = vobj.RootNode
        source = None
        source_index = -1
        for i in range(root.getNumChildren()):
            child = root.getChild(i)
            if isinstance(child, coin.SoCoordinate3) and child.getName() != "PrecisionCoordOffset":
                source = child
                source_index = i
                break
        if source is None:
            return False
        node = coin.SoCoordinate3()
        node.setName("PrecisionCoordOffset")
        # pivy signature: insertChild(child, index)
        root.insertChild(node, source_index + 1)
        sensor = coin.SoFieldSensor(self._vertex_sensor_cb, None)
        sensor.attach(source.point)
        self._coord_source = source
        self._coord_node = node
        self._coord_root = root
        self._coord_sensor = sensor
        self._coord_offset = App.Vector(0, 0, 0)
        self._update_vertex_offset(vobj.Object)
        return True

    def _unbind_vertex_offset(self):
        """Detach the sensor and remove a previously inserted mirror."""
        sensor = getattr(self, "_coord_sensor", None)
        if sensor is not None:
            try:
                sensor.detach()
            except Exception:
                pass
        node = getattr(self, "_coord_node", None)
        root = getattr(self, "_coord_root", None)
        if node is not None and root is not None:
            try:
                root.removeChild(node)
            except Exception:
                pass
        self._coord_sensor = None
        self._coord_node = None
        self._coord_root = None
        self._coord_source = None
        self._coord_offset = App.Vector(0, 0, 0)
        return

    def _vertex_sensor_cb(self, data, sensor):
        """Re-mirror the points after C++ rewrites the shape coordinates."""
        try:
            self._mirror_vertex_coords()
        except Exception:
            pass

    def _update_vertex_offset(self, obj):
        """Recompute the local correction and refresh the mirror."""
        if getattr(self, "_coord_source", None) is None:
            return
        if obj is None or not hasattr(obj, "Placement"):
            return
        self._coord_offset = self._vertex_local_offset(obj)
        self._mirror_vertex_coords()
        return

    def _vertex_local_offset(self, obj):
        """Return ``R^-1 * (Base - float32(Base))`` in the local frame."""
        base = obj.Placement.Base
        dx = base.x - _f32(base.x)
        dy = base.y - _f32(base.y)
        dz = base.z - _f32(base.z)
        if dx == 0.0 and dy == 0.0 and dz == 0.0:
            return App.Vector(0, 0, 0)
        # Placement.inverse() returns a new Placement (Rotation.invert()
        # would mutate the object's rotation in place).
        return obj.Placement.inverse().Rotation.multVec(App.Vector(dx, dy, dz))

    def _mirror_vertex_coords(self):
        """Copy the C++ points plus the local correction into the mirror."""
        source = getattr(self, "_coord_source", None)
        node = getattr(self, "_coord_node", None)
        if source is None or node is None:
            return
        n = source.point.getNum()
        if n == 0:
            if node.point.getNum() != 0:
                node.point.setNum(0)
            return
        off = getattr(self, "_coord_offset", None) or App.Vector(0, 0, 0)
        points = list(source.point.getValues())
        if off.x or off.y or off.z:
            points = [(p[0] + off.x, p[1] + off.y, p[2] + off.z) for p in points]
        # setNum first so a shrinking rebuild cannot leave stale points.
        node.point.setNum(n)
        node.point.setValues(0, n, points)
        return

    def getDisplayModes(self, vobj):
        """Return a list of display modes.

        Override this method to return a list of strings with
        display mode styles, such as `'Flat Lines'`, `'Shaded'`,
        `'Wireframe'`, `'Points'`.

        By default it returns an empty list.

        Parameters
        ----------
        vobj : the view provider of the scripted object.
            This is `obj.ViewObject`.

        Returns
        -------
        list
            Empty list `[ ]`
        """
        modes = []
        return modes

    def getDefaultDisplayMode(self):
        """Return the default mode defined in getDisplayModes.

        Override this method to return a string with the default display mode.

        By default it returns `'Flat Lines'`.

        Returns
        -------
        str
            `'Flat Lines'`

        """
        return "Flat Lines"

    def setDisplayMode(self, mode):
        """Map the modes defined in attach with those in getDisplayModes.

        This method is optional.

        By default since they have the same names nothing needs to be done,
        and it just returns the input `mode`.

        Parameters
        ----------
        str
            A string defining a display mode such as
            `'Flat Lines'`, `'Shaded'`, `'Wireframe'`, `'Points'`.
        """
        return mode

    def onChanged(self, vobj, prop):
        """Run when a view property is changed.

        Override this method to handle the behavior
        of the view provider depending on changes that occur to its properties
        such as line color, line width, point color, point size,
        draw style, shape color, transparency, and others.

        This method  updates the texture and pattern if
        the properties `TextureImage`, `Pattern`, `ShapeAppearance`,
        and `PatternSize` change.

        Parameters
        ----------
        vobj : the view provider of the scripted object.
            This is `obj.ViewObject`.

        prop : str
            Name of the property that was modified.
        """
        # treatment of patterns and image textures
        if prop in ("TextureImage", "Pattern", "ShapeAppearance"):
            if hasattr(self.Object, "Shape") and self.Object.Shape.Faces:
                path = None
                if hasattr(vobj, "TextureImage"):
                    if vobj.TextureImage:
                        path = vobj.TextureImage
                if not path:
                    if hasattr(vobj, "Pattern"):
                        if str(vobj.Pattern) in utils.svg_patterns():
                            path = utils.svg_patterns()[vobj.Pattern][1]
                        else:
                            path = "None"
                if path and vobj.RootNode:
                    switch = vobj.SwitchNode
                    if switch is not None and switch.getChildren().getLength() > 0:
                        flat_lines_node = switch.getChild(0)
                        # Get the Shaded sub-node of the Flat Lines node. The name in
                        # Std_SceneInspector is "Shaded", but the actual name is "FlatRoot")
                        shaded_node = gui_utils.find_coin_node_by_name(flat_lines_node, "FlatRoot")
                        if shaded_node is not None:
                            i = QtCore.QFileInfo(path)
                            if self.texture:
                                shaded_node.removeChild(self.texture)
                                self.texture = None
                            if self.texcoords:
                                shaded_node.removeChild(self.texcoords)
                                self.texcoords = None
                            if i.exists():
                                size = None
                                if ".SVG" in path.upper():
                                    size = params.get_param("HatchPatternResolution")
                                    if not size:
                                        size = 128
                                im = gui_utils.load_texture(path, size)
                                if im:
                                    self.texture = coin.SoTexture2()
                                    self.texture.image = im
                                    shaded_node.insertChild(self.texture, 1)
                                    if size:
                                        s = 1
                                        if hasattr(vobj, "PatternSize"):
                                            if vobj.PatternSize:
                                                s = vobj.PatternSize
                                        self.texcoords = coin.SoTextureCoordinatePlane()
                                        self.texcoords.directionS.setValue(s, 0, 0)
                                        self.texcoords.directionT.setValue(0, s, 0)
                                        shaded_node.insertChild(self.texcoords, 2)
        elif prop == "PatternSize":
            if hasattr(self, "texcoords"):
                if self.texcoords:
                    s = 1
                    if vobj.PatternSize:
                        s = vobj.PatternSize
                    vS = App.Vector(self.texcoords.directionS.getValue().getValue())
                    vT = App.Vector(self.texcoords.directionT.getValue().getValue())
                    vS.Length = s
                    vT.Length = s
                    self.texcoords.directionS.setValue(vS.x, vS.y, vS.z)
                    self.texcoords.directionT.setValue(vT.x, vT.y, vT.z)
        return

    def _update_pattern_size(self, vobj):
        """Update the pattern size. Helper method in onChanged."""
        if hasattr(self, "texcoords") and self.texcoords:
            s = 1
            if vobj.PatternSize:
                s = vobj.PatternSize
            vS = App.Vector(self.texcoords.directionS.getValue().getValue())
            vT = App.Vector(self.texcoords.directionT.getValue().getValue())
            vS.Length = s
            vT.Length = s
            self.texcoords.directionS.setValue(vS.x, vS.y, vS.z)
            self.texcoords.directionT.setValue(vT.x, vT.y, vT.z)

    def execute(self, vobj):
        """Run when the object is created or recomputed.

        Override this method to produce effects when the object
        is newly created, and whenever the document is recomputed.

        By default it does nothing.

        Parameters
        ----------
        vobj : the view provider of the scripted object.
            This is `obj.ViewObject`.
        """
        return

    def setEdit(self, vobj, mode):
        """Enter the edit mode of the object.

        Parameters
        ----------
        vobj : the view provider of the scripted object.
            This is `obj.ViewObject`.

        mode : int
            0, 1, 2 or 3. See the `Std_UserEditMode` command.

        Returns
        -------
        bool or None
            `False`: NOT USED HERE.
                     Do not enter edit mode (unsetEdit is not called).
            `True` : Handled edit mode.
                     Handled modes are 0 and 3, and only for certain objects.
                     The unsetEdit function should then also return `True`.
            `None` : Unhandled edit mode.
                     Handling is left to Part::FeaturePython code.
                     The unsetEdit function should then also return `None`.
        """
        if mode == 1 or mode == 2:
            return None

        # Fillet, Point, Shape2DView and PanelCut objects rely on a doubleClicked
        # function which takes precedence over all double-click edit modes. This
        # is a workaround as calling Gui.runCommand("Std_TransformManip") from
        # setEdit does not work properly. The object then seems to be put into
        # edit mode twice (?) and Esc will not cancel the command.

        # Facebinder, ShapeString, PanelSheet and Profile objects have their own
        # setEdit and unsetEdit.

        if utils.get_type(vobj.Object) in (
            "Wire",
            "Circle",
            "Ellipse",
            "Rectangle",
            "Polygon",
            "BSpline",
            "BezCurve",
        ):
            if not "Draft_Edit" in Gui.listCommands():
                self.wb_before_edit = Gui.activeWorkbench()
                Gui.activateWorkbench("DraftWorkbench")
            Gui.runCommand("Draft_Edit")
            return True

        return None

    def unsetEdit(self, vobj, mode):
        """Terminate the edit mode of the object.

        See setEdit.
        """
        if mode == 1 or mode == 2:
            return None

        if utils.get_type(vobj.Object) in (
            "Wire",
            "Circle",
            "Ellipse",
            "Rectangle",
            "Polygon",
            "BSpline",
            "BezCurve",
        ):
            if hasattr(App, "activeDraftCommand") and App.activeDraftCommand:
                App.activeDraftCommand.finish()
            Gui.Control.closeDialog()
            if hasattr(self, "wb_before_edit"):
                Gui.activateWorkbench(self.wb_before_edit.name())
                delattr(self, "wb_before_edit")
            return True

        return None

    def setupContextMenu(self, vobj, menu):
        tp = utils.get_type(self.Object)

        if tp in (
            "Wire",
            "Circle",
            "Ellipse",
            "Rectangle",
            "Polygon",
            "BSpline",
            "BezCurve",
            "Facebinder",
            "ShapeString",
            "PanelSheet",
            "Profile",
        ):
            action_edit = QtGui.QAction(translate("draft", "Edit"), menu)
            QtCore.QObject.connect(action_edit, QtCore.SIGNAL("triggered()"), self.edit)
            menu.addAction(action_edit)

        if tp == "Wire":
            action_flatten = QtGui.QAction(translate("draft", "Flatten"), menu)
            QtCore.QObject.connect(
                action_flatten, QtCore.SIGNAL("triggered()"), self.flatten
            )  # The flatten function is defined in view_wire.py.
            menu.addAction(action_flatten)

        # The default Part::FeaturePython context menu contains a `Set colors`
        # option. This option makes no sense for objects without a face or that
        # can only have a single face.
        # To override the default menu this function must return `True`.

        if tp in (
            "Wire",
            "Circle",
            "Ellipse",
            "Rectangle",
            "Polygon",
            "BSpline",
            "BezCurve",
            "Fillet",
            "Point",
            "Shape2DView",
            "PanelCut",
            "PanelSheet",
            "Profile",
        ):
            return True

    def edit(self):
        Gui.ActiveDocument.setEdit(self.Object, 0)

    def getIcon(self):
        """Return the path to the icon used by the view provider.

        The path can be a full path in the system, or a relative path
        inside the compiled resource file.
        It can also be a string that defines the icon in XPM format.

        Override this method to provide a specific icon
        for the object in the tree view.

        By default it returns the path to the `Draft_Draft.svg` icon.

        Returns
        -------
        str
            `':/icons/Draft_Draft.svg'`
        """
        tp = utils.get_type(self.Object)
        if tp in ("Line", "Wire", "Polyline"):
            return ":/icons/Draft_N-Linear.svg"
        if tp in ("Rectangle", "Polygon"):
            return ":/icons/Draft_N-Polygon.svg"
        if tp in ("Circle", "Ellipse", "BSpline", "BezCurve", "Fillet"):
            return ":/icons/Draft_N-Curve.svg"
        if tp in ("ShapeString"):
            return ":/icons/Draft_ShapeString_tree.svg"
        if hasattr(self.Object, "AutoUpdate") and not self.Object.AutoUpdate:
            import TechDrawGui

            return ":/icons/TechDraw_TreePageUnsync.svg"
        return ":/icons/Draft_Draft.svg"

    def claimChildren(self):
        """Return objects that will be placed under it in the tree view.

        Override this method to return a list with objects
        that will appear under this object in the tree view.
        That is, this object becomes the `parent`,
        and all those under it are the `children`.

        By default the returned list is composed of objects from
        `Object.Base`, `Object.Objects`, `Object.Components`,
        and `Object.Group`, if they exist.

        Returns
        -------
        list
            List of objects.
        """
        objs = []
        if hasattr(self.Object, "Base"):
            objs.append(self.Object.Base)
        if hasattr(self.Object, "Objects"):
            objs.extend(self.Object.Objects)
        if hasattr(self.Object, "Components"):
            objs.extend(self.Object.Components)
        if hasattr(self.Object, "Group"):
            objs.extend(self.Object.Group)
        return objs


# Alias for compatibility with v0.18 and earlier
_ViewProviderDraft = ViewProviderDraft


class ViewProviderDraftAlt(ViewProviderDraft):
    """A view provider that doesn't absorb its base object in the tree view.

    The `claimChildren` method is overridden to return an empty list.

    The `doubleClicked` method is defined.

    Only used by the `Shape2DView` object.
    """

    def __init__(self, vobj):
        super(ViewProviderDraftAlt, self).__init__(vobj)

    def doubleClicked(self, vobj):
        # See setEdit in ViewProviderDraft.
        Gui.runCommand("Std_TransformManip")
        return True

    def claimChildren(self):
        return []

    def updateData(self, obj, prop):
        super().updateData(obj, prop)
        if prop == "AutoUpdate":
            obj.ViewObject.signalChangeIcon()


# Alias for compatibility with v0.18 and earlier
_ViewProviderDraftAlt = ViewProviderDraftAlt


class ViewProviderDraftPart(ViewProviderDraftAlt):
    """A view provider that displays a Part icon instead of a Draft icon.

    The `getIcon` method is overridden to provide `Part_3D_object.svg`.

    Only used by the `Block` object.
    """

    def __init__(self, vobj):
        super(ViewProviderDraftPart, self).__init__(vobj)

    def getIcon(self):
        return ":/icons/Part_3D_object.svg"

    def setEdit(self, vobj, mode):
        return None

    def unsetEdit(self, vobj, mode):
        return None


# Alias for compatibility with v0.18 and earlier
_ViewProviderDraftPart = ViewProviderDraftPart

## @}
