# SPDX-License-Identifier: LGPL-2.1-or-later

"""SwGlb commands."""

import os

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets


def _selected():
    from . import export

    return [obj for obj in export.expand(list(Gui.Selection.getSelection())) if getattr(obj, "Shape", None) is not None]


class ExportGlb:
    """Export the selection (or everything visible) to a .glb with textures."""

    def GetResources(self):
        return {
            "MenuText": "导出 GLB（带贴图）…",
            "ToolTip": "导出选中对象；没选中时导出所有可见实体。贴图/材质会一起写入",
            "Pixmap": os.path.join(
                App.getResourceDir(), "Mod/SwGlb/Resources/icons/SwGlbExport.svg"
            ),
        }

    def IsActive(self):
        return App.ActiveDocument is not None

    def Activated(self):
        from . import export

        objects = _selected() or export.visible_objects()
        if not objects:
            QtWidgets.QMessageBox.information(
                None, "SwGlb", "文档里没有可导出的实体。"
            )
            return
        suggested = os.path.join(
            os.path.expanduser("~"), (App.ActiveDocument.Label or "export") + ".glb"
        )
        path, _filter = QtWidgets.QFileDialog.getSaveFileName(
            None, "导出 GLB", suggested, "glTF 二进制 (*.glb);;glTF (*.gltf)"
        )
        if not path:
            return
        if not path.lower().endswith((".glb", ".gltf")):
            path += ".glb"
        try:
            QtWidgets.QApplication.setOverrideCursor(
                QtGui.QCursor(QtCore.Qt.CursorShape.WaitCursor)
            )
        except Exception:
            QtWidgets.QApplication.setOverrideCursor(QtGui.QCursor(QtCore.Qt.WaitCursor))
        try:
            stats, size = export.export_objects(objects, path)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(None, "SwGlb", "导出失败：{}".format(exc))
            return
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()
        message = "已导出 {} 个对象、{} 个三角形\n材质 {} 个（其中贴图 {} 个）\n{}\n{:.1f} KB".format(
            stats["objects"], stats["triangles"], stats["materials"],
            stats["textures"], path, size / 1024.0,
        )
        App.Console.PrintMessage("SwGlb: " + message.replace("\n", " | ") + "\n")
        if not stats["materials"]:
            App.Console.PrintWarning("SwGlb: 没有找到任何材质或贴图，GLB 里只有几何\n")
        QtWidgets.QMessageBox.information(None, "SwGlb", message)


class ImportGlb:
    """Import a .glb/.gltf into a new document named after the file.

    FreeCAD's own import always creates/uses a document called "Unnamed" when
    none is open, which makes the model tree confusing.  Doing it here keeps
    the document name equal to the file name.
    """

    def GetResources(self):
        return {
            "MenuText": "导入 GLB（按文件名建文档）…",
            "ToolTip": "选择 .glb/.gltf，新建以文件名命名的文档并导入",
            "Pixmap": os.path.join(
                App.getResourceDir(), "Mod/SwGlb/Resources/icons/SwGlbImport.svg"
            ),
        }

    def IsActive(self):
        return True

    def Activated(self):
        path, _filter = QtWidgets.QFileDialog.getOpenFileName(
            None, "导入 GLB", "", "glTF (*.glb *.gltf);;All files (*)"
        )
        if not path:
            return
        name = os.path.splitext(os.path.basename(path))[0]

        document = None
        try:
            from . import importer

            document, objects, attached = importer.import_glb(path)
        except Exception as exc:
            QtWidgets.QMessageBox.critical(None, "SwGlb", "导入失败：{}".format(exc))
            if document is not None:
                try:
                    App.closeDocument(document.Name)
                except Exception:
                    pass
            return
        detail = ""
        if not attached:
            try:
                from . import reader

                detail = "\n文件信息：" + str(reader.inspect(path))
            except Exception:
                detail = ""
            App.Console.PrintWarning("SwGlb: 该文件没有可用的贴图（无材质/无 UV/图片缺失）%s\n" % detail)
        QtWidgets.QMessageBox.information(
            None,
            "SwGlb",
            "已导入到文档「{}」：{} 个对象，{} 张贴图{}".format(
                document.Label, objects, attached, detail
            ),
        )


def register():
    Gui.addCommand("SwGlb_Export", ExportGlb())
    Gui.addCommand("SwGlb_Import", ImportGlb())








