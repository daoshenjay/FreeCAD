//! DWG export (writer) for the SWDWG module.
//!
//! This is the counterpart of the import pipeline: the importer turns a DWG
//! into FreeCAD objects (`DWGExtract` -> `Entity` -> `DWGShape`), the writer
//! turns `Entity` records back into a DWG through LibreDWG's `dwg_add_*` API.
//! No DXF file is produced at any point.
//!
//! 导出功能：本文件为 DWG 导出新增，结构对齐 DWGImporter / DWGTypes：
//!
//!   App/DWGCompose.cpp   FreeCAD 对象  -> Entity（反向于 DWGShape.cpp）
//!   App/DWGWriter.cpp    Entity        -> dwg_add_* -> .dwg（本文件的实现）
//!
//! Only AutoCAD 2018 (AC1032) is written; `dwg.header.version` is forced to
//! R_2018 before the document skeleton is created.

#pragma once

#include "DWGTypes.h"
#include "../SWDWGGlobal.h"

#include <string>
#include <vector>

namespace SWDWG
{

//! One output layer.  Entities reference it by name; the layer carries the
//! colour / line weight that the entities inherit (entities are written as
//! BYLAYER, exactly like the importer reads them back).
struct DWGOutLayer
{
    std::string name = "0";
    //! AutoCAD Color Index of the layer (7 = white on black / black on white).
    int colorIndex = 7;
    //! Line weight in 1/100 mm; -1 keeps LibreDWG's default.
    int lineWeight = -1;
    //! LTYPE name; only "CONTINUOUS" (and BYLAYER) is guaranteed to exist.
    std::string ltype = "CONTINUOUS";
    bool off = false;
    bool frozen = false;
};

// ============================ 导出功能 BEGIN ============================
//! 一种输出线型（LTYPE）：名称 + 虚线图案。
//! DWG 里的线型是表记录，实体只存"线型句柄"；不写这张表，虚线在 CAD 里就
//! 会退化成实线（源图的虚线圆导出来变实线）。
struct DWGOutLinetype
{
    std::string name = "CONTINUOUS";
    std::string description;
    //! 图案总长（各段长度绝对值之和），DWG 的 pattern_len。
    double patternLength = 0.0;
    //! 图案：正 = 划线，负 = 空白，0 = 点（与 DWG 的 dashes[] 一致）。
    std::vector<double> dashes;
};
// ============================ 导出功能 END ==============================

//! Counters of one export run (reported by the GUI command).
struct DWGWriteStats
{
    int entities = 0;  //!< entities written into the DWG
    int layers = 0;    //!< LAYER table records created
    int skipped = 0;   //!< input entities that could not be written
};

//! Write `entities` (world coordinates) into `filename` as an AutoCAD 2018 DWG.
//!
//! `layers` are created first; an entity whose layer is not listed is put on
//! layer "0".  The model-space extents and the `*ACTIVE` viewport are derived
//! from the written geometry so that a CAD opens the file on the drawing
//! (LibreDWG itself writes empty extents / a degenerate viewport otherwise).
//!
//! Returns true on success.  On failure `error` holds a human readable message.
class SWDWGExport DWGWriter
{
public:
    static bool writeDWG(const std::string& filename,
                         const std::vector<Entity>& entities,
                         const std::vector<DWGOutLayer>& layers,
                         const std::vector<DWGOutLinetype>& linetypes,
                         std::string& error, DWGWriteStats& stats);

    //! RGB (0xRRGGBB) -> nearest AutoCAD Color Index.  Inverse of aciRgb()
    //! (declared in DWGInternal.h) which the importer uses.
    static int rgbToAci(unsigned long rgb);
};

}  // namespace SWDWG
