# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2009 Yorik van Havre <yorik@uncreated.net>              *
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
"""Initialization file of the workbench, non-GUI."""

import FreeCAD as App

translate = FreeCAD.Qt.translate

# add Import/Export types
App.addImportType("Autodesk DXF 2D (*.dxf *.DXF)", "importDXF")
App.addImportType("SVG as geometry (*.svg *.SVG)", "importSVG")
App.addImportType("Open CAD Format (*.oca *.OCA *.gcad *.GCAD)", "importOCA")
App.addImportType("Common airfoil data (*.dat *.DAT)", "importAirfoilDAT")
App.addExportType("Autodesk DXF 2D (*.dxf)", "importDXF")
App.addTranslatableExportType(translate("FileFormat", "Flattened SVG"), ["svg"], "importSVG")
App.addExportType("Open CAD (*.oca)", "importOCA")
# DWG: both import and export are registered by Mod/SWDWG/Init.py (native,
# LibreDWG based).  Draft's own *.dwg entries are deliberately NOT registered
# any more:
#   * two import filters claiming "dwg" make FreeCAD show the "Select Module"
#     dialog on every open / drag-drop / command line call;
#   * the export filter would be a second, non working DWG entry next to the
#     native writer.
# Draft's importDWG module is still there and can be called from Python.
# App.addImportType("Autodesk DWG 2D (*.dwg *.DWG)", "importDWG")
# App.addExportType("Autodesk DWG 2D (*.dwg)", "importDWG")

App.__unit_test__ += ["TestDraft"]
