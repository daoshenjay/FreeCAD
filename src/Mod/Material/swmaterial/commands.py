# SPDX-License-Identifier: LGPL-2.1-or-later

"""Gui commands of the material dock (shown on the Material workbench)."""

import FreeCAD as App
import FreeCADGui as Gui

from . import core
from .paths import icon


def _panel():
    from . import panel

    return panel.get_panel()


class _MaterialCommand:
    """Common scaffolding for the workbench commands."""

    icon = "SwMaterialApply.svg"
    menu = "Apply material"
    tooltip = "Apply the selected material to the selected objects"
    accel = ""

    def GetResources(self):
        resources = {"Pixmap": icon(self.icon), "MenuText": self.menu, "ToolTip": self.tooltip}
        if self.accel:
            resources["Accel"] = self.accel
        return resources

    def IsActive(self):
        return Gui.ActiveDocument is not None

    def Activated(self):
        raise NotImplementedError


class ShowPanelCommand(_MaterialCommand):
    icon = "SwMaterialPanel.svg"
    menu = "Material browser"
    tooltip = "Show or hide the material browser"
    accel = "Ctrl+Shift+M"

    def IsActive(self):
        return True

    def Activated(self):
        from . import panel

        panel.toggle_panel()


class ApplyCommand(_MaterialCommand):
    def IsActive(self):
        return bool(Gui.Selection.getSelection())

    def Activated(self):
        panel = _panel()
        panel.show()
        panel.apply_to_objects()


class ApplyFacesCommand(_MaterialCommand):
    icon = "SwMaterialFaces.svg"
    menu = "Apply to selected faces"
    tooltip = "Apply the current material to the picked faces (all faces when nothing is picked)"

    def IsActive(self):
        return bool(Gui.Selection.getSelection())

    def Activated(self):
        panel = _panel()
        panel.show()
        panel.apply_to_selected_faces()


class PaletteCommand(_MaterialCommand):
    icon = "SwMaterialFaces.svg"
    menu = "Shade each face"
    tooltip = "Give every face its own shade of the current material"

    def IsActive(self):
        return bool(Gui.Selection.getSelection())

    def Activated(self):
        panel = _panel()
        panel.show()
        panel.apply_palette()


class ClearCommand(_MaterialCommand):
    icon = "SwMaterialClear.svg"
    menu = "Reset material"
    tooltip = "Reset the appearance of the selected objects"

    def IsActive(self):
        return bool(Gui.Selection.getSelection())

    def Activated(self):
        targets = core.filter_targets(Gui.Selection.getSelection())
        done = sum(1 for obj in targets if core.clear_material(obj))
        Gui.updateGui()
        App.Console.PrintMessage("SwMaterial: reset {} object(s)\n".format(done))


class TextureCommand(_MaterialCommand):
    icon = "SwMaterialTexture.svg"
    menu = "Texture library…"
    tooltip = "Browse the built-in textures (or pick one from disk) and apply it"

    def IsActive(self):
        return True

    def Activated(self):
        from . import panel

        panel.show_texture_tab()


class TextureRemoveCommand(_MaterialCommand):
    icon = "SwMaterialTextureOff.svg"
    menu = "Remove texture"
    tooltip = "Remove the image texture from the selected objects"

    def IsActive(self):
        return bool(Gui.Selection.getSelection())

    def Activated(self):
        from . import texture

        targets = core.filter_targets(Gui.Selection.getSelection())
        done = sum(1 for obj in targets if texture.remove_texture(obj))
        Gui.updateGui()
        App.Console.PrintMessage("SwMaterial: removed {} texture(s)\n".format(done))


def register():
    """Register all commands with the Gui (idempotent)."""
    existing = set(Gui.listCommands())
    for name, command in (
        ("SwMaterial_ShowPanel", ShowPanelCommand()),
        ("SwMaterial_Apply", ApplyCommand()),
        ("SwMaterial_ApplyFaces", ApplyFacesCommand()),
        ("SwMaterial_Palette", PaletteCommand()),
        ("SwMaterial_Clear", ClearCommand()),
        ("SwMaterial_Texture", TextureCommand()),
        ("SwMaterial_TextureRemove", TextureRemoveCommand()),
    ):
        if name not in existing:
            Gui.addCommand(name, command)

