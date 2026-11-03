# SPDX-License-Identifier: LGPL-2.1-or-later

"""SwGlb workbench: export GLB with textures.

FreeCAD execs this file inside a loader function, so the icon path is built from
``FreeCAD.getResourceDir()`` and the package is imported lazily.
"""


class SwGlbWorkbench(FreeCADGui.Workbench):
    """Export selected solids (with textures) to glTF/GLB."""

    Icon = FreeCAD.getResourceDir() + "Mod/SwGlb/Resources/icons/SwGlbWorkbench.svg"
    MenuText = "SwGlb"
    ToolTip = "导出带贴图的 GLB（glTF 2.0）"

    def Initialize(self):
        import swglb.commands as commands

        commands.register()
        tools = ["SwGlb_Import", "SwGlb_Export"]
        self.appendToolbar("SwGlb", tools)
        self.appendMenu(["&SwGlb", "SwGlb"], tools)

    def Activated(self):
        pass

    def Deactivated(self):
        pass

    def GetClassName(self):
        return "Gui::PythonWorkbench"


FreeCADGui.addWorkbench(SwGlbWorkbench())


# Take over the system's glTF import (registered by Import/Init.py to the
# ImportGui module): File > Open, drag & drop and the Import dialog all
# resolve through the import registry, so this single redirect covers them.
# The filter string must match the registered one character by character.
FreeCAD.changeImportModule("glTF (*.gltf *.GLTF *.glb *.GLB)", "ImportGui", "swglb.importer")

# Same for the export side: File > Export and the object context menu resolve
# through the export registry and call swglb.export.export(objects, filename).
FreeCAD.changeExportModule("glTF (*.gltf *.glb)", "ImportGui", "swglb.export")






