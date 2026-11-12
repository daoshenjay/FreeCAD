//! Dimension style (DIMSTYLE) helpers.
//!
//! The parsing itself needs the LibreDWG headers and lives in
//! DWGDimstyle.cpp (declared in DWGInternal.h).  This header only exposes the
//! dependency-free value formatter so that the Gui module can reuse it
//! without pulling in libredwg.

#pragma once

#include "DWGTypes.h"

#include <string>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! Format a measured world-unit value (e.g. 12.5) as a display string that
//! follows the style's unit type (DIMLUNIT), precision (DIMDEC) and
//! DIMPOST prefix/suffix (e.g. "12.50 mm").
SWDWGExport std::string formatDimValue(double value, const DimStyle& style);

}  // namespace SWDWG
