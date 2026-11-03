# SPDX-License-Identifier: LGPL-2.1-or-later

"""SwMaterial workbench: assign materials to solids.

Note: FreeCAD execs this file inside a loader function, so class-body
expressions only see the loader's globals (``FreeCAD``/``FreeCADGui``).  That
is why the icon path is built from ``FreeCAD.getResourceDir()`` and the
workbench package is imported lazily inside the methods.
"""


def _install_observers():
    """Register the texture observer at start-up, not only on activation.

    FreeCAD imports every workbench's ``InitGui`` when it starts, so doing it
    here means textures are rebuilt even when the user opens a saved file
    without ever switching to this workbench in that session.
    """
    import sys

    try:
        import swmaterial.texture as texture
    except Exception:
        sys.path.append(FreeCAD.getResourceDir() + "Mod/SwMaterial")
        import swmaterial.texture as texture
    texture.install_observer()


try:
    _install_observers()
except Exception:
    pass

class SwMaterialWorkbench(FreeCADGui.Workbench):
    """Assign / browse / reset material appearances."""

    Icon = (
        FreeCAD.getResourceDir()
        + "Mod/SwMaterial/Resources/icons/SwMaterialWorkbench.svg"
    )
    MenuText = "SwMaterial"
    ToolTip = "Browse materials and assign them to solids"

    def Initialize(self):
        import swmaterial.commands as commands
        import swmaterial.texture as texture

        commands.register()
        texture.install_observer()

        tools = [
            "SwMaterial_ShowPanel",
            "SwMaterial_Apply",
            "SwMaterial_ApplyFaces",
            "SwMaterial_Texture",
            "SwMaterial_TextureRemove",
            "SwMaterial_Palette",
            "SwMaterial_Clear",
        ]
        self.appendToolbar("SwMaterial", tools)
        self.appendMenu(["&SwMaterial", "SwMaterial"], tools)

    def Activated(self):
        from swmaterial import panel

        panel.show_panel()

    def Deactivated(self):
        pass

    def GetClassName(self):
        return "Gui::PythonWorkbench"


FreeCADGui.addWorkbench(SwMaterialWorkbench())


