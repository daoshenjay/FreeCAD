"""Glue that puts the SwMaterial dock into the C++ Material workbench.

The former ``SwMaterial`` workbench no longer exists: this module registers its
commands once, adds them to the ``Material`` workbench menu and tool bar, and
opens the material dock whenever that workbench is entered.

The code lives in a real module on purpose.  FreeCAD executes ``InitGui.py``
files with ``exec(code)`` inside a function, so anything defined at the top
level of such a file ends up in a throw-away local namespace: functions and
classes defined there get ``__globals__`` pointing at ``FreeCADGuiInit`` and
cannot see their own module-level helpers once the file finishes executing.
"""

import FreeCADGui as Gui

COMMANDS = [
    "SwMaterial_ShowPanel",
    "SwMaterial_Apply",
    "SwMaterial_ApplyFaces",
    "SwMaterial_Texture",
    "SwMaterial_TextureRemove",
    "SwMaterial_Palette",
    "SwMaterial_Clear",
]

MENU_ITEM = "Material_Edit"
TOOLBAR_NAME = "Material"

_MAX_HOOK_RETRIES = 40
_HOOK_RETRY_MS = 250

_activation_hook_installed = False
_hook_retries = 0


def _is_material_workbench():
    workbench = Gui.activeWorkbench()
    if workbench is None:
        return False
    # The Material workbench is ``MatGui::Workbench``: its Python wrapper class
    # is the one defined in ``Mod/Material/InitGui.py``.
    return workbench.__class__.__name__ == "MaterialWorkbench"


def on_workbench_activated(*_):
    """Open the material dock when the Material workbench is entered."""
    if not _is_material_workbench():
        return
    from . import panel

    panel.show_panel()


def install_activation_hook():
    """Connect to ``MainWindow.workbenchActivated`` once the window exists."""
    global _activation_hook_installed, _hook_retries

    if _activation_hook_installed:
        return
    main_window = Gui.getMainWindow()
    if main_window is None:
        if _hook_retries < _MAX_HOOK_RETRIES:
            _hook_retries += 1
            try:
                from PySide import QtCore
            except ImportError:
                return
            QtCore.QTimer.singleShot(_HOOK_RETRY_MS, install_activation_hook)
        return
    try:
        main_window.workbenchActivated.connect(on_workbench_activated)
    except AttributeError:
        # Builds without that signal: the dock stays reachable through the
        # menu and tool bar commands.
        pass
    _activation_hook_installed = True


class WorkbenchManipulator:
    """Extend the Material workbench's own menu and tool bar.

    ``Gui.addWorkbenchManipulator`` runs these methods after the C++ workbench
    has built its bars, so the items can be located by command name.  On every
    other workbench those names do not exist, so nothing is added there.
    """

    def modifyMenuBar(self):
        install_activation_hook()
        entries = [{"append": "Separator", "menuItem": MENU_ITEM}]
        entries.extend({"append": command, "menuItem": MENU_ITEM} for command in COMMANDS)
        return entries

    def modifyToolBars(self):
        install_activation_hook()
        return [{"append": command, "toolBar": TOOLBAR_NAME} for command in COMMANDS]


def install():
    """Register the commands, the texture observer and the workbench hooks."""
    from . import commands, texture

    commands.register()
    # Installed at start-up (as the old workbench did) so that texture layers
    # are rebuilt when a saved file is opened in this session.
    texture.install_observer()
    Gui.addWorkbenchManipulator(WorkbenchManipulator())
    install_activation_hook()
