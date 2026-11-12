# SPDX-License-Identifier: LGPL-2.1-or-later

# FreeCAD init script of the SWDWG module
# (c) 2001 Juergen Riegel LGPL

import FreeCAD

# Register the ".dwg" file type.  That is ALL this file needs to do.
#
# NOTE: do NOT "import SWDWG" here.  This script runs from
# InitApplications() in src/App/FreeCADInit.py while FreeCAD is still
# initialising; at that moment the native SWDWG.pyd cannot be loaded yet
# (FreeCADApp.dll is still being set up), so the import fails with
#     ImportError: DLL load failed while importing SWDWG
# and that exception aborts this script *before* addImportType() runs,
# leaving ".dwg" with no importer at all.
#
# FreeCAD loads the module by itself when a DWG is actually opened:
#   File > Open / Import  -> freecad/module_io.py
#                            importlib.import_module("SWDWG")
#                            SWDWG.open(filename) / SWDWG.insert(filename, docname)
#   command line / macro  -> src/App/Application.cpp (processFiles)
#                            import SWDWG; SWDWG.open(filename)
#
# Both entry points are implemented natively in AppSWDWG.cpp:
#   open(filename)            -> new document
#   insert(filename, docname) -> into an existing document
FreeCAD.addImportType("Autodesk DWG (*.dwg *.DWG)", "SWDWG")

# ============================ 导出功能 BEGIN ============================
# 导出功能：DWG 导出由本模块自己注册，不再借用 Draft 的 "Autodesk DWG 2D"。
#
# 这里只注册 Python 适配模块 export_dwg.py（FreeCAD 的导出注册表只认模块名，
# 不认 C++ 命令），它里面的 export() 调用 SWDWG.writeDWG()。真正的写在
# App/DWGCompose.cpp + App/DWGWriter.cpp，用 LibreDWG 直接写 DWG，不经 DXF。
#
# 注意：与上面的 import 同理，这里不能 import SWDWG（模块还没加载好）；
# addExportType 只是登记模块名，等真正导出时才 import。
FreeCAD.addExportType("Autodesk DWG (*.dwg)", "export_dwg")
# ============================ 导出功能 END ==============================
