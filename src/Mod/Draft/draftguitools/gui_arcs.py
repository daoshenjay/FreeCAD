# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   (c) 2009, 2010 Yorik van Havre <yorik@uncreated.net>                  *
# *   (c) 2009, 2010 Ken Cline <cline@frii.com>                             *
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
"""Provides GUI tools to create circular arc objects."""

## @package gui_arcs
# \ingroup draftguitools
# \brief Provides GUI tools to create circular arc objects.

## \addtogroup draftguitools
# @{
import math
from PySide.QtCore import QT_TRANSLATE_NOOP

import FreeCAD as App
import FreeCADGui as Gui
import DraftVecUtils
from FreeCAD import Units as U
from draftgeoutils import circles as geo_circles
from draftgeoutils import circles as geo_circles_incomplete
from draftgeoutils import geometry as geo_geometry
from draftguitools import gui_base
from draftguitools import gui_base_original
from draftguitools import gui_tool_utils
from draftguitools import gui_trackers as trackers
from draftutils import gui_utils
from draftutils import params
from draftutils import utils
from draftutils.messages import _err, _log, _toolmsg
from draftutils.translate import translate


class Arc(gui_base_original.Creator):
    """Gui command for the Circular Arc tool."""

    def __init__(self):
        super().__init__()
        self.closedCircle = False
        self.featureName = "Arc"

    def GetResources(self):
        """Set icon, menu and tooltip."""
        return {
            "Pixmap": "Draft_Arc",
            "Accel": "A, R",
            "MenuText": QT_TRANSLATE_NOOP("Draft_Arc", "Arc"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Draft_Arc", "Creates a circular arc from a center point and a radius"
            ),
        }

    def Activated(self):
        """Execute when the command is called."""
        super().Activated(name=self.featureName)
        if self.ui:
            self.step = 0
            self.center = None
            self.rad = None
            self.angle = 0  # angle inscribed by arc
            self.firstangle = 0
            self.tangents = []
            self.tanpoints = []
            if self.featureName == "Arc":
                self.ui.arcUi()
            else:
                self.ui.circleUi()
            self.altdown = False
            self.ui.sourceCmd = self
            self.linetrack = trackers.lineTracker(dotted=True)
            self.arctrack = trackers.arcTracker()
            self.call = self.view.addEventCallback("SoEvent", self.action)
            self._log_snap_grid_state()
            _toolmsg(translate("draft", "Pick center point"))

    def _log_snap_grid_state(self):
        """PrintLog: snap/grid prefs that can move points onto the grid."""
        try:
            always = params.get_param("alwaysSnap")
        except Exception:
            always = "?"
        try:
            spacing = params.get_grid_param("gridSpacing")
        except Exception:
            spacing = "?"
        grid_on = False
        snap_r = None
        if hasattr(Gui, "Snapper"):
            try:
                grid_on = Gui.Snapper.isEnabled("Grid")
            except Exception:
                grid_on = False
            try:
                pos = (0, 0)
                snap_r = Gui.Snapper.getScreenDist(params.get_param("snapRange"), pos)
            except Exception:
                snap_r = None
        _log(
            "Draft snap state: alwaysSnap={0} GridEnabled={1} gridSpacing={2} "
            "snapRangePx={3} snapRadiusWorld@0={4}".format(
                always,
                grid_on,
                spacing,
                params.get_param("snapRange") if hasattr(params, "get_param") else "?",
                snap_r,
            )
        )

    def finish(self, cont=False):
        """Terminate the operation.

        Parameters
        ----------
        cont: bool or None, optional
            Restart (continue) the command if `True`, or if `None` and
            `ui.continueMode` is `True`.
        """
        self.end_callbacks(self.call)
        if self.ui:
            self.linetrack.finalize()
            self.arctrack.finalize()
        super().finish()
        if cont or (cont is None and self.ui and self.ui.continueMode):
            self.Activated()

    def updateAngle(self, angle):
        """Update the angle with the new value."""
        # previous absolute angle
        lastangle = self.firstangle + self.angle
        if lastangle <= -2 * math.pi:
            lastangle += 2 * math.pi
        if lastangle >= 2 * math.pi:
            lastangle -= 2 * math.pi
        # compute delta = change in angle:
        d0 = angle - lastangle
        d1 = d0 + 2 * math.pi
        d2 = d0 - 2 * math.pi
        if abs(d0) < min(abs(d1), abs(d2)):
            delta = d0
        elif abs(d1) < abs(d2):
            delta = d1
        else:
            delta = d2
        newangle = self.angle + delta
        # normalize angle, preserving direction
        if newangle >= 2 * math.pi:
            newangle -= 2 * math.pi
        if newangle <= -2 * math.pi:
            newangle += 2 * math.pi
        self.angle = newangle

    def action(self, arg):
        """Handle the 3D scene events.

        This is installed as an EventCallback in the Inventor view.

        Parameters
        ----------
        arg: dict
            Dictionary with strings that indicates the type of event received
            from the 3D view.
        """
        if arg["Type"] == "SoKeyboardEvent":
            if arg["Key"] == "ESCAPE":
                self.finish()
        elif not self.ui.mouse:
            pass
        elif arg["Type"] == "SoLocation2Event":  # mouse movement detection
            self.point, ctrlPoint, info = gui_tool_utils.getPoint(self, arg)
            # this is to make sure radius is what you see on screen
            if self.center and DraftVecUtils.dist(self.point, self.center) > 0:
                viewdelta = DraftVecUtils.project(self.point.sub(self.center), self.wp.axis)
                if not DraftVecUtils.isNull(viewdelta):
                    self.point = self.point.add(viewdelta.negative())
            if self.step == 0:  # choose center
                if gui_tool_utils.hasMod(arg, gui_tool_utils.get_mod_alt_key()):
                    if not self.altdown:
                        self.altdown = True
                        self.ui.switchUi(True)
                    else:
                        if self.altdown:
                            self.altdown = False
                            self.ui.switchUi(False)
            elif self.step == 1:  # choose radius
                if len(self.tangents) == 2:
                    cir = geo_circles_incomplete.circleFrom2tan1pt(
                        self.tangents[0], self.tangents[1], self.point
                    )
                    _c = geo_circles.findClosestCircle(self.point, cir)
                    self.center = _c.Center
                    self.arctrack.setCenter(self.center)
                elif self.tangents and self.tanpoints:
                    cir = geo_circles_incomplete.circleFrom1tan2pt(
                        self.tangents[0], self.tanpoints[0], self.point
                    )
                    _c = geo_circles.findClosestCircle(self.point, cir)
                    self.center = _c.Center
                    self.arctrack.setCenter(self.center)
                if gui_tool_utils.hasMod(arg, gui_tool_utils.get_mod_alt_key()):
                    if not self.altdown:
                        self.altdown = True
                    if info:
                        ob = self.doc.getObject(info["Object"])
                        num = int(info["Component"].lstrip("Edge")) - 1
                        ed = ob.Shape.Edges[num]
                        if len(self.tangents) == 2:
                            cir = geo_circles_incomplete.circleFrom3tan(
                                self.tangents[0], self.tangents[1], ed
                            )
                            cl = geo_circles.findClosestCircle(self.point, cir)
                            self.center = cl.Center
                            self.rad = cl.Radius
                            self.arctrack.setCenter(self.center)
                        else:
                            self.rad = self.center.add(
                                geo_geometry.findDistance(self.center, ed).sub(self.center)
                            ).Length
                    else:
                        self.rad = DraftVecUtils.dist(self.point, self.center)
                else:
                    if self.altdown:
                        self.altdown = False
                    self.rad = DraftVecUtils.dist(self.point, self.center)
                self.ui.setRadiusValue(self.rad, "Length")
                self.arctrack.setRadius(self.rad)
                self.linetrack.p1(self.center)
                self.linetrack.p2(self.point)
                self.linetrack.on()
            elif self.step == 2:  # choose first angle
                currentrad = DraftVecUtils.dist(self.point, self.center)
                if currentrad != 0:
                    angle = DraftVecUtils.angle(
                        self.wp.u, self.point.sub(self.center), self.wp.axis
                    )
                else:
                    angle = 0
                self.linetrack.p2(
                    DraftVecUtils.scaleTo(self.point.sub(self.center), self.rad).add(self.center)
                )
                self.ui.setRadiusValue(math.degrees(angle), unit="Angle")
                self.firstangle = angle
            else:
                # choose second angle
                currentrad = DraftVecUtils.dist(self.point, self.center)
                if currentrad != 0:
                    angle = DraftVecUtils.angle(
                        self.wp.u, self.point.sub(self.center), self.wp.axis
                    )
                else:
                    angle = 0
                self.linetrack.p2(
                    DraftVecUtils.scaleTo(self.point.sub(self.center), self.rad).add(self.center)
                )
                self.updateAngle(angle)
                self.ui.setRadiusValue(math.degrees(self.angle), unit="Angle")
                self.arctrack.setApertureAngle(self.angle)

            gui_tool_utils.redraw3DView()

        elif arg["Type"] == "SoMouseButtonEvent":  # mouse click
            if arg["State"] == "DOWN" and arg["Button"] == "BUTTON1":
                # Always sample this click; the last mousemove may be stale
                # (and is skipped entirely when support was preselected).
                if self.step == 0 and not self.support:
                    gui_tool_utils.getSupport(arg)
                self.point, ctrlPoint, info = gui_tool_utils.getPoint(self, arg)
                if self.point:
                    if self.step == 0:  # choose center
                        if gui_tool_utils.hasMod(arg, gui_tool_utils.get_mod_alt_key()):
                            snapped = self.view.getObjectInfo(
                                (arg["Position"][0], arg["Position"][1])
                            )
                            if snapped:
                                ob = self.doc.getObject(snapped["Object"])
                                num = int(snapped["Component"].lstrip("Edge")) - 1
                                ed = ob.Shape.Edges[num]
                                self.tangents.append(ed)
                                if len(self.tangents) == 2:
                                    self.arctrack.on()
                                    self.ui.radiusUi()
                                    self.step = 1
                                    self.ui.setNextFocus()
                                    self.linetrack.on()
                                    _toolmsg(translate("draft", "Pick radius"))
                        else:
                            if len(self.tangents) == 1:
                                self.tanpoints.append(self.point)
                            else:
                                self.center = self.point
                                self.node = [self.point]
                                self.arctrack.setCenter(self.center)
                                self.linetrack.p1(self.center)
                                self.linetrack.p2(self.point)
                                self._log_center_f32(arg)
                            self.arctrack.on()
                            self.ui.radiusUi()
                            self.step = 1
                            self.ui.setNextFocus()
                            self.linetrack.on()
                            _toolmsg(translate("draft", "Pick radius"))
                            if self.planetrack:
                                self.planetrack.set(self.point)
                    elif self.step == 1:  # choose radius
                        if self.center and DraftVecUtils.dist(self.point, self.center) > 0:
                            viewdelta = DraftVecUtils.project(
                                self.point.sub(self.center), self.wp.axis
                            )
                            if not DraftVecUtils.isNull(viewdelta):
                                self.point = self.point.add(viewdelta.negative())
                        if not self.tangents:
                            self.rad = DraftVecUtils.dist(self.point, self.center)
                            self.ui.setRadiusValue(self.rad, "Length")
                            self.arctrack.setRadius(self.rad)
                            self.linetrack.p1(self.center)
                            self.linetrack.p2(self.point)
                            self.linetrack.on()
                        if self.closedCircle:
                            self.drawArc()
                        else:
                            self.ui.labelRadius.setText(translate("draft", "Start angle"))
                            self.ui.radiusValue.setToolTip(translate("draft", "Start angle"))
                            self.ui.radiusValue.setText(U.Quantity(0, U.Angle).UserString)
                            self.linetrack.p1(self.center)
                            self.linetrack.on()
                            self.step = 2
                            _toolmsg(translate("draft", "Pick start angle"))
                    elif self.step == 2:  # choose first angle
                        self._update_first_angle_from_point()
                        self.ui.labelRadius.setText(translate("draft", "Aperture angle"))
                        self.ui.radiusValue.setToolTip(translate("draft", "Aperture angle"))
                        ang_offset = DraftVecUtils.angle(
                            self.wp.u, self.arctrack.getDeviation(), self.wp.axis
                        )
                        self.arctrack.setStartAngle(self.firstangle - ang_offset)
                        self.step = 3
                        _toolmsg(translate("draft", "Pick aperture"))
                    else:  # choose second angle
                        self._update_aperture_from_point()
                        self.step = 4
                        self.drawArc()
                    self.update_hints()

    def _log_center_f32(self, arg):
        """Log double center vs Coin float32 transform (PrintLog only)."""
        center = App.Vector(self.center)
        origin = trackers.get_origin()
        f32 = self.arctrack.centerF32()
        delta = (center - f32).Length
        local = trackers.to_local(center)
        f32ox = trackers._f32(origin.x)
        f32oy = trackers._f32(origin.y)
        f32oz = trackers._f32(origin.z)
        local_f32 = App.Vector(
            trackers._f32(center.x - f32ox),
            trackers._f32(center.y - f32oy),
            trackers._f32(center.z - f32oz),
        )
        # float32 ULP at |local| is approximately |local| * 2**-23
        mag = max(abs(local.x), abs(local.y), abs(local.z), 1.0)
        ulp = mag * (2.0**-23)
        snap_info = getattr(Gui.Snapper, "snapInfo", None) if hasattr(Gui, "Snapper") else None
        raw = None
        c_raw = float("nan")
        try:
            raw = App.Vector(self.view.getPoint(arg["Position"]))
            c_raw = (center - raw).Length
        except Exception:
            raw = None
            c_raw = float("nan")
        always = params.get_param("alwaysSnap")
        grid_on = False
        snap_r = None
        if hasattr(Gui, "Snapper"):
            try:
                grid_on = Gui.Snapper.isEnabled("Grid")
                snap_r = Gui.Snapper.getScreenDist(
                    params.get_param("snapRange"), tuple(arg["Position"])
                )
            except Exception:
                pass
        try:
            spacing = params.get_grid_param("gridSpacing")
        except Exception:
            spacing = "?"
        _log(
            "Draft center click: pos={0} O=({1:.6f}, {2:.6f}, {3:.6f}) "
            "C=({4:.6f}, {5:.6f}, {6:.6f}) rendered=({7:.6f}, {8:.6f}, {9:.6f}) "
            "|C-rendered|={10:.6g} local=({11:.6f}, {12:.6f}, {13:.6f}) "
            "local_f32_err={14:.6g} ulp/2~={15:.6g} snap={16} support={17} "
            "raw={18} |C-raw|={19:.6g} alwaysSnap={20} Grid={21} "
            "gridSpacing={22} snapRadius={23}".format(
                arg.get("Position"),
                origin.x,
                origin.y,
                origin.z,
                center.x,
                center.y,
                center.z,
                f32.x,
                f32.y,
                f32.z,
                delta,
                local.x,
                local.y,
                local.z,
                (local - local_f32).Length,
                0.5 * ulp,
                bool(snap_info),
                bool(self.support),
                raw,
                c_raw,
                always,
                grid_on,
                spacing,
                snap_r,
            )
        )

    def _update_first_angle_from_point(self):
        """Refresh first angle from the current click sample."""
        if not self.center:
            return
        currentrad = DraftVecUtils.dist(self.point, self.center)
        if currentrad != 0:
            angle = DraftVecUtils.angle(self.wp.u, self.point.sub(self.center), self.wp.axis)
        else:
            angle = 0
        self.linetrack.p2(
            DraftVecUtils.scaleTo(self.point.sub(self.center), self.rad).add(self.center)
        )
        self.ui.setRadiusValue(math.degrees(angle), unit="Angle")
        self.firstangle = angle

    def _update_aperture_from_point(self):
        """Refresh aperture angle from the current click sample."""
        if not self.center:
            return
        currentrad = DraftVecUtils.dist(self.point, self.center)
        if currentrad != 0:
            angle = DraftVecUtils.angle(self.wp.u, self.point.sub(self.center), self.wp.axis)
        else:
            angle = 0
        self.linetrack.p2(
            DraftVecUtils.scaleTo(self.point.sub(self.center), self.rad).add(self.center)
        )
        self.updateAngle(angle)
        self.ui.setRadiusValue(math.degrees(self.angle), unit="Angle")
        self.arctrack.setApertureAngle(self.angle)

    def drawArc(self):
        """Actually draw the arc object."""
        rot, sup, pts, fil = self.getStrings()
        c = self.center
        _log(
            "Draft drawArc: closed={0} center=({1:.6f}, {2:.6f}, {3:.6f}) "
            "rad={4} support={5} first={6:.6f} aperture={7:.6f}".format(
                self.closedCircle,
                c.x if c else float("nan"),
                c.y if c else float("nan"),
                c.z if c else float("nan"),
                self.rad,
                sup,
                math.degrees(self.firstangle),
                math.degrees(self.angle),
            )
        )
        if self.closedCircle:
            try:
                # The command to run is built as a series of text strings
                # to be committed through the `draftutils.todo.ToDo` class.
                Gui.addModule("Draft")
                if params.get_param("UsePartPrimitives"):
                    # Insert a Part::Primitive object
                    _base = DraftVecUtils.toString(self.center)
                    _cmd = "FreeCAD.ActiveDocument."
                    _cmd += 'addObject("Part::Circle", "Circle")'
                    _cmd_list = [
                        "circle = " + _cmd,
                        "circle.Radius = " + str(self.rad),
                        "pl = FreeCAD.Placement()",
                        "pl.Rotation.Q = " + rot,
                        "pl.Base = " + _base,
                        "circle.Placement = pl",
                        "Draft.autogroup(circle)",
                        "Draft.select(circle)",
                        "FreeCAD.ActiveDocument.recompute()",
                    ]
                    self.commit(translate("draft", "Create Circle (Part)"), _cmd_list)
                else:
                    # Insert a Draft circle
                    _base = DraftVecUtils.toString(self.center)
                    _cmd = "Draft.make_circle"
                    _cmd += "("
                    _cmd += "radius=" + str(self.rad) + ", "
                    _cmd += "placement=pl, "
                    _cmd += "face=" + fil + ", "
                    _cmd += "support=" + sup
                    _cmd += ")"
                    _cmd_list = [
                        "pl=FreeCAD.Placement()",
                        "pl.Rotation.Q=" + rot,
                        "pl.Base=" + _base,
                        "circle = " + _cmd,
                        "Draft.autogroup(circle)",
                        "FreeCAD.ActiveDocument.recompute()",
                    ]
                    self.commit(translate("draft", "Create Circle"), _cmd_list)
            except Exception:
                _err("Draft: error delaying commit")
        else:
            # Not a closed circle, therefore a circular arc
            sta = math.degrees(self.firstangle)
            end = math.degrees(self.firstangle + self.angle)
            if end < sta:
                sta, end = end, sta
            sta %= 360
            end %= 360
            _log("Draft arc angles: sta={0:.6f} end={1:.6f} equal={2}".format(sta, end, sta == end))

            try:
                Gui.addModule("Draft")
                if params.get_param("UsePartPrimitives"):
                    # Insert a Part::Primitive object
                    _base = DraftVecUtils.toString(self.center)
                    _cmd = "FreeCAD.ActiveDocument."
                    _cmd += 'addObject("Part::Circle", "Circle")'
                    _cmd_list = [
                        "circle = " + _cmd,
                        "circle.Radius = " + str(self.rad),
                        "circle.Angle1 = " + str(sta),
                        "circle.Angle2 = " + str(end),
                        "pl = FreeCAD.Placement()",
                        "pl.Rotation.Q = " + rot,
                        "pl.Base = " + _base,
                        "circle.Placement = pl",
                        "Draft.autogroup(circle)",
                        "Draft.select(circle)",
                        "FreeCAD.ActiveDocument.recompute()",
                    ]
                    self.commit(translate("draft", "Create Arc (Part)"), _cmd_list)
                else:
                    # Insert a Draft circle
                    _base = DraftVecUtils.toString(self.center)
                    _cmd = "Draft.make_circle"
                    _cmd += "("
                    _cmd += "radius=" + str(self.rad) + ", "
                    _cmd += "placement=pl, "
                    _cmd += "face=" + fil + ", "
                    _cmd += "startangle=" + str(sta) + ", "
                    _cmd += "endangle=" + str(end) + ", "
                    _cmd += "support=" + sup
                    _cmd += ")"
                    _cmd_list = [
                        "pl = FreeCAD.Placement()",
                        "pl.Rotation.Q = " + rot,
                        "pl.Base = " + _base,
                        "circle = " + _cmd,
                        "Draft.autogroup(circle)",
                        "FreeCAD.ActiveDocument.recompute()",
                    ]
                    self.commit(translate("draft", "Create Arc"), _cmd_list)
            except Exception:
                _err("Draft: error delaying commit")

        # Finalize full circle or circular arc
        self.finish(cont=None)

    def numericInput(self, numx, numy, numz):
        """Validate the entry fields in the user interface.

        This function is called by the toolbar or taskpanel interface
        when valid x, y, and z have been entered in the input fields.
        """
        self.center = App.Vector(numx, numy, numz)
        self.node = [self.center]
        self.arctrack.setCenter(self.center)
        self.arctrack.on()
        self.ui.radiusUi()
        self.step = 1
        self.ui.setNextFocus()
        _toolmsg(translate("draft", "Pick radius"))
        self.update_hints()

    def numericRadius(self, rad):
        """Validate the entry radius in the user interface.

        This function is called by the toolbar or taskpanel interface
        when a valid radius has been entered in the input field.
        """
        if self.step == 1:
            self.rad = rad
            if len(self.tangents) == 2:
                cir = geo_circles_incomplete.circleFrom2tan1rad(
                    self.tangents[0], self.tangents[1], rad
                )
                if self.center:
                    _c = geo_circles.findClosestCircle(self.center, cir)
                    self.center = _c.Center
                else:
                    self.center = cir[-1].Center
            elif self.tangents and self.tanpoints:
                cir = geo_circles_incomplete.circleFrom1tan1pt1rad(
                    self.tangents[0], self.tanpoints[0], rad
                )
                if self.center:
                    _c = geo_circles.findClosestCircle(self.center, cir)
                    self.center = _c.Center
                else:
                    self.center = cir[-1].Center
            if self.closedCircle:
                self.drawArc()
            else:
                self.step = 2
                self.arctrack.setCenter(self.center)
                self.ui.labelRadius.setText(translate("draft", "Start angle"))
                self.ui.radiusValue.setToolTip(translate("draft", "Start angle"))
                self.linetrack.p1(self.center)
                self.linetrack.on()
                self.ui.radiusValue.setText("")
                self.ui.radiusValue.setFocus()
                _toolmsg(translate("draft", "Pick start angle"))
        elif self.step == 2:
            self.ui.labelRadius.setText(translate("draft", "Aperture angle"))
            self.ui.radiusValue.setToolTip(translate("draft", "Aperture angle"))
            self.firstangle = math.radians(rad)
            ang_offset = DraftVecUtils.angle(self.wp.u, self.arctrack.getDeviation(), self.wp.axis)
            self.arctrack.setStartAngle(self.firstangle - ang_offset)
            self.step = 3
            self.ui.radiusValue.setText("")
            self.ui.radiusValue.setFocus()
            _toolmsg(translate("draft", "Pick aperture angle"))
        else:
            self.updateAngle(rad)
            self.angle = math.radians(rad)
            self.step = 4
            self.drawArc()
        self.update_hints()

    def get_hints(self):
        if self.step == 0:
            hints = [Gui.InputHint(translate("draft", "%1 pick center"), Gui.UserInput.MouseLeft)]
        elif self.step == 1:
            hints = [Gui.InputHint(translate("draft", "%1 pick radius"), Gui.UserInput.MouseLeft)]
        elif self.step == 2:
            hints = [
                Gui.InputHint(translate("draft", "%1 pick start angle"), Gui.UserInput.MouseLeft)
            ]
        else:
            hints = [Gui.InputHint(translate("draft", "%1 pick aperture"), Gui.UserInput.MouseLeft)]
        return (
            hints
            + gui_tool_utils._get_hint_xyz_constrain()
            + gui_tool_utils._get_hint_mod_constrain()
            + gui_tool_utils._get_hint_mod_snap()
        )


Gui.addCommand("Draft_Arc", Arc())


class Arc_3Points(gui_base.GuiCommandBase):
    """GuiCommand for the Draft_Arc_3Points tool."""

    def __init__(self):
        super().__init__(name="Arc_3Points")

    def GetResources(self):
        """Set icon, menu and tooltip."""
        return {
            "Pixmap": "Draft_Arc_3Points",
            "Accel": "A, T",
            "MenuText": QT_TRANSLATE_NOOP("Draft_Arc_3Points", "Arc From 3 Points"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Draft_Arc_3Points", "Creates a circular arc from 3 points"
            ),
        }

    def Activated(self):
        """Execute when the command is called."""
        super().Activated()

        # Reset the values
        self.points = []
        self.normal = None
        self.tracker = trackers.arcTracker()
        self.tracker.autoinvert = False

        # Set up the working plane and launch the Snapper
        # with the indicated callbacks: one for when the user clicks
        # on the 3D view, and another for when the user moves the pointer.
        import WorkingPlane

        WorkingPlane.get_working_plane()

        Gui.Snapper.getPoint(callback=self.getPoint, movecallback=self.drawArc)
        Gui.Snapper.ui.sourceCmd = self
        Gui.Snapper.ui.setTitle(
            title=translate("draft", "Arc From 3 Points"), icon="Draft_Arc_3Points"
        )
        Gui.Snapper.ui.continueCmd.show()
        self.update_hints()

    def getPoint(self, point, info):
        """Get the point by clicking on the 3D view.

        Every time the user clicks on the 3D view this method is run.
        In this case, a point is appended to the list of points,
        and the tracker is updated.
        The object is finally created when three points are picked.

        Parameters
        ----------
        point: Base::Vector
            The point selected in the 3D view.

        info: str
            Some information obtained about the point passed by the Snapper.
        """
        # If there is not point, the command was cancelled
        # so the command exits.
        if not point:
            return None

        # Avoid adding the same point twice
        if point not in self.points:
            self.points.append(point)
            if self.planetrack and len(self.points) == 1:
                self.planetrack.set(point)

        if len(self.points) < 3:
            # If one or two points were picked, set up again the Snapper
            # to get further points, but update the `last` property
            # with the last selected point.
            #
            # When two points are selected then we can turn on
            # the arc tracker to show the preview of the final curve.
            if len(self.points) == 2:
                self.tracker.on()
            Gui.Snapper.getPoint(
                last=self.points[-1], callback=self.getPoint, movecallback=self.drawArc
            )
            Gui.Snapper.ui.sourceCmd = self
            Gui.Snapper.ui.setTitle(
                title=translate("draft", "Arc From 3 Points"), icon="Draft_Arc_3Points"
            )
            Gui.Snapper.ui.continueCmd.show()
            self.update_hints()

        else:
            # If three points were already picked in the 3D view
            # proceed with creating the final object.
            # Draw a simple `Part::Feature` if the parameter is `True`.
            Gui.addModule("Draft")
            Gui.addModule("WorkingPlane")
            _cmd = "Draft.make_arc_3points(["
            _cmd += "FreeCAD." + str(self.points[0])
            _cmd += ", FreeCAD." + str(self.points[1])
            _cmd += ", FreeCAD." + str(self.points[2])
            _cmd += "]"
            _cmd += ", placement=pl"
            _cmd += ", primitive=" + str(params.get_param("UsePartPrimitives"))
            _cmd += ")"
            _cmd_list = [
                "pl = WorkingPlane.get_working_plane().get_placement()",
                "circle = " + _cmd,
                "Draft.autogroup(circle)",
            ]
            if params.get_param("UsePartPrimitives"):
                _cmd_list.append("Draft.select(circle)")
            _cmd_list.append("FreeCAD.ActiveDocument.recompute()")
            self.commit(translate("draft", "Create Arc From 3 Points"), _cmd_list)
            self.finish(cont=None)

    def drawArc(self, point, info):
        """Draw preview arc when we move the pointer in the 3D view.

        It uses the `gui_trackers.arcTracker.setBy3Points` method.

        Parameters
        ----------
        point: Base::Vector
            The dynamic point passed by the callback
            as we move the pointer in the 3D view.

        info: str
            Some information obtained from the point by the Snapper.
        """
        if len(self.points) == 2:
            if point.sub(self.points[1]).Length > 0.001:
                self.tracker.setBy3Points(self.points[0], self.points[1], point)

    def finish(self, cont=False):
        """Terminate the operation.

        Parameters
        ----------
        cont: bool or None, optional
            Restart (continue) the command if `True`, or if `None` and
            `ui.continueMode` is `True`.
        """
        self.tracker.finalize()
        super().finish()
        if cont or (cont is None and Gui.Snapper.ui and Gui.Snapper.ui.continueMode):
            self.Activated()

    def get_hints(self):
        if len(self.points) == 0:
            hints = [
                Gui.InputHint(translate("draft", "%1 pick first point"), Gui.UserInput.MouseLeft)
            ]
        elif len(self.points) == 1:
            hints = [
                Gui.InputHint(translate("draft", "%1 pick second point"), Gui.UserInput.MouseLeft)
            ]
        else:
            hints = [
                Gui.InputHint(translate("draft", "%1 pick third point"), Gui.UserInput.MouseLeft)
            ]
        return (
            hints
            + gui_tool_utils._get_hint_xyz_constrain()
            + gui_tool_utils._get_hint_mod_constrain()
            + gui_tool_utils._get_hint_mod_snap()
        )


Draft_Arc_3Points = Arc_3Points
Gui.addCommand("Draft_Arc_3Points", Arc_3Points())


class ArcGroup:
    """Gui Command group for the Arc tools."""

    def GetResources(self):
        """Set icon, menu and tooltip."""
        return {
            "MenuText": QT_TRANSLATE_NOOP("Draft_ArcTools", "Arc Tools"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Draft_ArcTools", "Tools to create various types of circular arcs"
            ),
        }

    def GetCommands(self):
        """Return a tuple of commands in the group."""
        return ("Draft_Arc", "Draft_Arc_3Points")

    def IsActive(self):
        """Return True when this command should be available."""
        return bool(gui_utils.get_3d_view())


Gui.addCommand("Draft_ArcTools", ArcGroup())

## @}
