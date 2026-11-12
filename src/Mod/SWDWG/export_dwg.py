# SPDX-License-Identifier: LGPL-2.1-or-later
"""DWG export entry point for FreeCAD's export registry.

导出功能：本文件为 DWG 导出新增。

FreeCAD's ``File > Export`` resolves the exporter through
``Application::exportTo()``: it imports this module and calls
``export(objectslist, filename)``.  The actual work is done by the SWDWG C++
writer (``SWDWG.writeDWG`` -> ``DWGCompose`` + ``DWGWriter``), which uses
LibreDWG's writer API directly - there is no DXF intermediate.

``SWDWG/Init.py`` registers this module with ``addExportType("Autodesk DWG
(*.dwg)", "export_dwg")``; the filter belongs to SWDWG itself, Draft no longer
registers a DWG export entry.
"""

import FreeCAD as App


def export(objectslist, filename):  # noqa: A001 - name required by FreeCAD
    """Write ``objectslist`` into ``filename`` as an AutoCAD 2018 DWG.

    导出功能：``objectslist`` 为空时导出当前文档全部对象；成功返回文件名，
    失败时由 SWDWG 抛出异常，FreeCAD 会弹出 "Export failed"。

    导出功能：如果传入的对象里没有可导出的几何（例如 File > Export 时选中的是
    Part 容器 / Origin / 轴网），自动退回导出整个文档，避免生成空文件。
    """
    import SWDWG

    objects = [obj for obj in (objectslist or []) if obj is not None]
    stats = SWDWG.writeDWG(filename, objects)
    if not stats.get("entities") and objects:
        App.Console.PrintWarning(
            "SWDWG: 选中对象没有可导出的几何，改为导出整个文档\n"
        )
        stats = SWDWG.writeDWG(filename, [])
    if not stats.get("entities"):
        raise RuntimeError("SWDWG: 当前文档没有可导出的几何")

    App.Console.PrintMessage(
        "SWDWG: 已导出 {} 个实体 / {} 个图层 -> {}（AutoCAD 2018）{}\n".format(
            stats.get("entities", 0),
            stats.get("layers", 0),
            filename,
            "，跳过 {} 个对象".format(stats["skipped"]) if stats.get("skipped") else "",
        )
    )
    return filename
