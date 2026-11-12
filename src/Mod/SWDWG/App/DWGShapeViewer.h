//! SHX shape / font file visualiser.
//!
//! Lets the user pick any compiled AutoCAD shape file (.shx) - a font such as
//! txt.shx / simplex.shx or a symbol library such as ltypeshp.shx - and draws
//! every shape it contains on a grid, one Part::Feature per glyph (when the
//! file is small) or one combined feature (large symbol files), so the outline
//! can be inspected / measured / zoomed inside FreeCAD.
//!
//! Reuses the pure-C++ parser in DWGShapeFile.h/.cpp (same code the L2 linetype
//! cutter uses).  Font .shx bodies are authored in the 8-unit cell convention
//! (baseline at y = 0, caps to y = 7); symbol files use arbitrary units.  Every
//! shape is scaled independently to a common display height so proportions are
//! easy to compare.

#pragma once

#include <string>

#include "../SWDWGGlobal.h"

namespace App
{
class Document;
}

namespace SWDWG
{

//! Draw every shape of \a path into \a doc on a grid.
//! \return true on success; on failure \a error holds a user-facing message.
//! Shapes are added as Part::Feature objects (one per glyph for files with up
//! to 400 shapes, otherwise a single compound feature to keep the object count
//! bounded).  The caller is responsible for a recompute / view update.
SWDWGExport bool viewShxFont(App::Document* doc, const std::string& path,
                             std::string& error);

}  // namespace SWDWG
