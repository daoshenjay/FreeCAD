# SPDX-License-Identifier: LGPL-2.1-or-later

# SWDWG gui init module
# (c) 2001 Juergen Riegel LGPL
#
# ---------------------------------------------------------------------------
# SWDWG is a *library* module.
#
# It deliberately does NOT call Gui.addWorkbench(), so it never appears in the
# workbench selector, and it ships no menus or toolbars of its own.  If a
# consuming workbench ever wants to expose the commands, it can do so through
# its own setupToolBars() / setupMenuBar(), for example:
#
#     class MyWorkbench(FreeCADGui.Workbench):
#         def Initialize(self):
#             import SWDWGGui
#
#         def setupToolBars(self):
#             return self.appendToolbar("DWG", ["SWDWG_Import"])
#
#         def setupMenuBar(self):
#             return self.appendMenu("DWG", ["SWDWG_Import"])
#
# The normal entry points are the file-format registry installed by Init.py
# (File > Open / Import / Export) and the commands themselves, which stay
# registered for macros and the Python console:
#
#     Gui.runCommand("SWDWG_Import")
#     Gui.runCommand("SWDWG_Export")
#     Gui.runCommand("SWDWG_ViewSHX")        # SHX font inspector (developer tool)
#     Gui.runCommand("SWDWG_MultiInstance")  # SoMultipleInstance demo (developer tool)
#
# Importing the Gui extension module at STARTUP is required, not optional.
# Doing it here -- while FreeCAD imports every module's InitGui.py -- registers
# the view providers (SWDWG::ViewProviderDWGText, ViewProviderDWGDimension,
# ViewProviderDWGLineGroup, ViewProviderDWGSurfaceMesh, ...) with FreeCAD's type
# system before any saved .fcstd is opened.  Without it, opening a DWG-based
# .fcstd fails every text/dimension object with
#   Invalid view provider type 'SWDWG::ViewProviderDWGText'
#
# Note: SWDWG still registers the *.dwg import type from Init.py
# (FreeCAD.addImportType), so it also shows up in the File > Open filter list.
# That is a file-format capability, not a workbench/menu entry -- remove that
# call in Init.py if even the file dialog entry must stay hidden.
# ---------------------------------------------------------------------------

import SWDWGGui  # noqa: F401 - registers the commands and the view providers
