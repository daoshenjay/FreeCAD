# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   (c) 2009 Yorik van Havre <yorik@uncreated.net>                        *
# *   (c) 2010 Ken Cline <cline@frii.com>                                   *
# *   (c) 2020 Eliud Cabrera Castillo <e.cabrera-castillo@tum.de>           *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful,            *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with FreeCAD; if not, write to the Free Software        *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************
"""Provides the base classes for most old Draft Gui Commands.

This class is used by Gui Commands to set up some properties
of the DraftToolBar, the Snapper, and the working plane.
"""

## @package gui_base_original
# \ingroup draftguitools
# \brief Provides the base classes for most old Draft Gui Commands.

## \addtogroup draftguitools
# @{
from PySide import QtCore

import FreeCAD as App
import FreeCADGui as Gui
import DraftVecUtils
import WorkingPlane
from draftguitools import gui_tool_utils
from draftguitools import gui_trackers as trackers
from draftutils import gui_utils
from draftutils import params
from draftutils import todo
from draftutils import utils
from draftutils.messages import _log, _msg, _toolmsg


class DraftTool:
    """The base class of all Draft Tools.

    This is the original class that was defined in `DraftTools.py`
    before any re-organization of the code.

    This class is subclassed by `Creator` and `Modifier`
    to set up a few additional properties of these two types.

    This class connects with the `draftToolBar` and `Snapper` classes
    that are installed in the `FreeCADGui` namespace in order to set up some
    properties of the running tools such as the task panel, the snapping
    functions, and the grid trackers.

    It also connects with the `WorkingPlane` class.

    This class is intended to be replaced by newer classes inside the
    `gui_base` module, in particular, `GuiCommandBase`.
    """

    def __init__(self):
        self.commitList = []

    def IsActive(self):
        """Return True when this command should be available."""
        return bool(gui_utils.get_3d_view())

    def Activated(self, name="None", is_subtool=False):
        """Execute when the command is called.

        If an active Gui Command exists, it will call the `finish` method
        of it to terminate it.

        If no active Gui Command exists, it will proceed with configuration
        of the tool. The child class that subclasses this class
        then should continue with its own Activated method.

        Parameters
        ----------
        name: str, optional
            It defaults to `'None'`.
            It is the `featureName` of the object, to know what is being run.

        is_subtool: bool, optional
            It defaults to `False`.
            This is set to `True` when we want to modify an object
            by using the mechanism of a `subtool`, introduced
            through the `Draft_SubelementHighlight` command.
            That is, first we run `Draft_SubelementHighlight`
            then we can use `Draft_Move` while setting `is_subtool` to `True`.
        """
        if App.activeDraftCommand and not is_subtool:
            App.activeDraftCommand.finish()
        App.activeDraftCommand = self

        # The Part module is first initialized when using any Gui Command
        # for the first time.
        global Part
        import Part

        self.call = None
        self.commitList = []
        self.constrain = None
        self.doc = App.ActiveDocument
        self.extendedCopy = False
        self.featureName = name
        self.node = []
        self.obj = None
        self.point = None
        self.pos = []
        self.support = None
        self.ui = Gui.draftToolBar
        self.ui.mouse = True  # reset mouse movement
        self.ui.sourceCmd = self
        self.view = gui_utils.get_3d_view()
        self.wp = WorkingPlane.get_working_plane()

        self.planetrack = None
        if params.get_param("showPlaneTracker"):
            self.planetrack = trackers.PlaneTracker()
        self._origin_sensor = None
        origin = self._tracker_origin()
        if origin is not None:
            trackers.set_origin(origin)
        self._attach_origin_sensor()
        if hasattr(Gui, "Snapper"):
            Gui.Snapper.setTrackers()

        _toolmsg("{}".format(16 * "-"))
        _toolmsg("GuiCommand: {}".format(self.featureName))

        # update hints after the tool is fully initialized
        QtCore.QTimer.singleShot(0, self.update_hints)

    def _tracker_origin(self):
        """Point at the viewport centre on the focal plane (world double).

        This point is the float32 anchor shared by all Draft trackers and
        previews.  It only has to sit next to the camera *and* the visible
        scene, on the view ray, so that float32 subtraction stays exact.
        It must not be projected onto the working plane: that would pin the
        normal component to the plane while the scene sits at a different
        height, leaving float32(world - origin) huge and the preview
        quantised out of the viewport.

        Returns None only when no anchor at all can be determined, so the
        previous origin is kept instead of falling back to an empty
        (0, 0, 0) anchor.
        """
        view = self.view
        if view is None:
            return None
        pt = None
        try:
            size = view.getSize()
            focal = view.getPointOnFocalPlane(int(size[0] / 2), int(size[1] / 2))
            cand = App.Vector(focal.x, focal.y, focal.z)
            # Coin returns an all-zero vector when the camera lookup fails.
            if cand.Length != 0:
                pt = cand
        except Exception as exc:
            _log("Draft _tracker_origin focal plane failed: {0}".format(exc))
        if pt is None:
            # Fall back to the camera position: an anchor at (0, 0, 0) is
            # useless when the scene sits far from the world origin, so a
            # stale or empty anchor must never be returned.
            try:
                cam = view.getCameraNode()
                pos = cam.position.getValue()
                pt = App.Vector(pos[0], pos[1], pos[2])
            except Exception as exc:
                _log("Draft _tracker_origin camera fallback failed: {0}".format(exc))
                return None
        return pt

    def _origin_threshold(self):
        """How far the viewport center must move before refreshing O (Sketcher-like)."""
        size = 0.0
        view = self.view
        if view is not None:
            try:
                sz = view.getSize()
                p0 = view.getPointOnFocalPlane(0, 0)
                p1 = view.getPointOnFocalPlane(int(sz[0]), int(sz[1]))
                a = App.Vector(p0.x, p0.y, p0.z)
                b = App.Vector(p1.x, p1.y, p1.z)
                # The focal-plane corner distance already is the physical
                # viewport size, so no working-plane projection is needed.
                size = (b - a).Length
            except Exception:
                size = 0.0
        if size > 0:
            return max(1.0, 0.01 * size)
        cur = trackers.get_origin()
        mag = max(abs(cur.x), abs(cur.y), abs(cur.z), 1.0)
        return max(1.0, mag * 1e-6)

    def _attach_origin_sensor(self):
        """Follow camera motion so O tracks the viewport while the command runs."""
        self._detach_origin_sensor()
        view = self.view
        if view is None:
            return
        try:
            cam = view.getCameraNode()
        except Exception as exc:
            _log("Draft origin camera sensor unavailable: {0}".format(exc))
            return
        if cam is None:
            return
        from pivy import coin

        self._origin_sensor = coin.SoNodeSensor(self._origin_sensor_cb, None)
        self._origin_sensor.attach(cam)

    def _detach_origin_sensor(self):
        sensor = getattr(self, "_origin_sensor", None)
        if sensor is None:
            return
        try:
            sensor.detach()
        except Exception:
            pass
        self._origin_sensor = None

    def _origin_sensor_cb(self, data, sensor):
        origin = self._tracker_origin()
        if origin is None:
            return
        if (origin - trackers.get_origin()).Length > self._origin_threshold():
            trackers.set_origin(origin)

    def update_hints(self):
        Gui.HintManager.show(*self.get_hints())

    def get_hints(self):
        return []

    def end_callbacks(self, call):
        try:
            self.view.removeEventCallback("SoEvent", call)
            gui_utils.end_all_events()
        except RuntimeError:
            # the view has been deleted already
            pass
        call = None

    def finish(self, cont=False):
        """Finish the current command.

        These are general cleaning tasks that are performed
        when terminating all commands.

        These include setting the node list to empty,
        setting to `None` the active command,
        turning off the graphical interface (task panel),
        finishing the plane tracker, restoring the working plane,
        turning off the snapper.

        If a callback is installed in the 3D view, the callback is removed,
        and set to `None`.

        If the commit list is non-empty it will commit the instructions on
        the list with `draftutils.todo.ToDo.delayCommit`,
        and the list will be set to empty.
        """
        self.node = []
        App.activeDraftCommand = None
        if self.ui:
            self.ui.offUi()
            self.ui.sourceCmd = None
        if hasattr(Gui, "Snapper"):
            Gui.Snapper.off()
        if self.planetrack:
            self.planetrack.finalize()
        self._detach_origin_sensor()
        trackers.set_origin(App.Vector(0, 0, 0))
        self.wp._restore()
        if self.commitList:
            last_cmd = self.commitList[-1][1][-1]
            if last_cmd.find("recompute") >= 0:
                self.commitList[-1] = (self.commitList[-1][0], self.commitList[-1][1][:-1])
                todo.ToDo.delayCommit(self.commitList)
                todo.ToDo.delayAfter(Gui.doCommand, last_cmd)
            else:
                todo.ToDo.delayCommit(self.commitList)
        self.commitList = []

        QtCore.QTimer.singleShot(0, Gui.HintManager.hide)

    def commit(self, name, func):
        """Store actions in the commit list to be run later.

        Parameters
        ----------
        name: str
            An arbitrary string that indicates the name of the operation
            to run.

        func: list of str
            Each element of the list is a string that will be run by
            `Gui.doCommand`.

            See the complete information in the `draftutils.todo.ToDo` class.
        """
        self.commitList.append((name, func))

    def getStrings(self, addrot=None):
        """Return useful strings that will be used to build commands.

        Returns
        -------
        str, str, str, str
            A tuple of four strings that represent useful information.

            * the current working plane rotation quaternion as a string
            * the support object if available as a string
            * the list of nodes inside the `node` attribute as a string
            * the string `'True'` or `'False'` depending on the make face
              setting
        """
        # Current plane rotation as a string
        qr = self.wp.get_placement().Rotation.Q
        qr = "({0}, {1}, {2}, {3})".format(qr[0], qr[1], qr[2], qr[3])

        # Support object
        if self.support and params.get_param("useSupport"):
            sup = "FreeCAD.ActiveDocument.getObject"
            sup += '("{}")'.format(self.support.Name)
        else:
            sup = "None"

        # Contents of self.node
        points = "["
        for n in self.node:
            if len(points) > 1:
                points += ", "
            points += DraftVecUtils.toString(n)
        points += "]"

        # Make face
        if self.ui:
            make_face = str(self.ui.makeFaceMode)
        else:
            make_face = "True"

        return qr, sup, points, make_face


class Creator(DraftTool):
    """A generic Creator tool, used by creation tools such as line or arc.

    It inherits `DraftTool`, which sets up the majority of the behavior
    of this class.
    """

    def Activated(self, name="None"):
        """Execute when the command is called.

        Parameters
        ----------
        name: str, optional
            It defaults to `'None'`.
            It is the `featureName` of the object, to know what is being run.
        """
        super().Activated(name)
        # call _save to sync with _restore called in finish method
        self.wp._save()
        self.support = gui_tool_utils.get_support()


class Modifier(DraftTool):
    """A generic Modifier tool, used by modification tools such as move.

    After initializing the parent class, it sets the `copymode` attribute
    to `False`.

    It inherits `DraftTool`, which sets up the majority of the behavior
    of this class.

    Modifier tools share a two-phase input flow: first the user picks the
    object(s) to operate on, then the user supplies one or more reference
    points (base point, angle, distance, mirror line, etc.). The
    ``selection_done`` flag distinguishes the two phases so the base class
    can provide the selection-phase hint by default; subclasses override
    ``get_action_hints`` to describe their action-phase steps.

    Subclasses that only operate on a single object set
    ``multi_object_selection`` to ``False`` so the selection hint is phrased
    in the singular.
    """

    multi_object_selection = True

    def __init__(self):
        super().__init__()
        self.copymode = False
        self.selection_done = False

    def Activated(self, name="None", is_subtool=False):
        super().Activated(name, is_subtool)
        # call _save to sync with _restore called in finish method
        self.wp._save()
        self.selection_done = False

    def get_hints(self):
        if not self.selection_done:
            return gui_tool_utils._get_hint_select_object(self.multi_object_selection)
        return self.get_action_hints()

    def get_action_hints(self):
        return []


## @}
