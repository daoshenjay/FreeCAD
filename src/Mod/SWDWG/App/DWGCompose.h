//! FreeCAD objects -> DWG entity records (the export counterpart of DWGShape).
//!
//! 导出功能：本文件为 DWG 导出新增。
//!
//! Import pipeline:  DWG -> LibreDWG -> Entity (DWGExtract) -> OCC shape (DWGShape)
//! Export pipeline:  FreeCAD object -> Entity (this file) -> dwg_add_* (DWGWriter)
//!
//! `Entity` (DWGTypes.h) is the pivot: the importer fills it from LibreDWG, the
//! writer fills it from the document, so both directions share one data model.
//!
//! Mapping:
//!   SWDWG::DWGLineGroup   -> layer + colour/line weight/linetype + its raw
//!                            geometry (DWGSourceShape) as line work
//!   SWDWG::DWGText        -> TEXT / MTEXT with the imported text attributes
//!   SWDWG::DWGDimension   -> dimension line + extension lines + arrow triangles
//!                            + the formatted text (flattened, see below)
//!   SWDWG::DWGSurfaceMesh -> 3DFACE per mesh face
//!   any object with Shape -> its edges as LINE / ARC / CIRCLE / ELLIPSE
//!
//! Note: DWGDimension only keeps the *flattened* display geometry (the importer
//! resolved DIMENSION + DIMSTYLE once), so a dimension is written back as its
//! visible line work and text.  The association of a real DIMENSION entity is
//! not reconstructed; the appearance is preserved.

#pragma once

#include "DWGWriter.h"

#include <App/Document.h>
#include <App/DocumentObject.h>

#include <string>
#include <vector>

namespace SWDWG
{

class SWDWGExport DWGCompose
{
public:
    //! Collect the exportable geometry of `objects` (empty = every object of
    //! `doc`) into `entities` and the layers they need.
    //!
    //! Returns the number of objects that could not be exported (no geometry /
    //! unsupported type).  `error` is filled only for fatal problems.
    static int compose(App::Document* doc,
                       const std::vector<App::DocumentObject*>& objects,
                       std::vector<Entity>& entities,
                       std::vector<DWGOutLayer>& layers,
                       std::vector<DWGOutLinetype>& linetypes,
                       std::string& error);
};

}  // namespace SWDWG
