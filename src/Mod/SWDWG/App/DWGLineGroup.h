//! L2 linetype-aware layer group feature.
//!
//! Each DWG style key (layer + color + linetype + weight) is imported as one
//! DWGLineGroup instead of a plain Part::Feature.
//!
//! Geometry bookkeeping:
//!  - DWGSourceShape is the raw (un-cut) geometry and the only persisted copy.
//!    Its type is PropertyShapeBinary, i.e. it lands in the .FCStd as *binary*
//!    BRep - measured 1.8x faster to write and 26% smaller than the ASCII
//!    ".brp" that Part::PropertyPartShape produces, and about 2x faster to
//!    read back.  It is also what the display view provider and SWDTM's
//!    contour extraction read (by property name - that contract is kept).
//!  - Shape is the derived mirror that Part tools, measurement and scripts
//!    use.  It is marked transient, because persisting it as well would write
//!    the very same BRep a second time (that doubled the file, the save time
//!    and the peak memory); it is refilled from DWGSourceShape when a document
//!    is opened.
//!
//! On every recompute execute() re-cuts the raw geometry with the linked
//! DWGLineType definition (pattern x effective scale) into world-unit ink
//! segments, so the displayed geometry itself reflects the AutoCAD linetype
//! (dash / dot / dash-dot).
//!
//! Because the cut happens in execute(), editing the shared DWGLineType
//! (Pattern / Scale) touches the referencing groups and recomputes them,
//! which rebuilds the geometry of every line using that linetype at once
//! (see DWGLineType::updateLinkedFeatures and SWDWGGui's recompute hook).

#pragma once

#include "DWGShapeFile.h"
#include "DWGTypes.h"
#include "PropertyShapeBinary.h"

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/Part/App/PartFeature.h>

#include <TopoDS_Shape.hxx>

#include "../SWDWGGlobal.h"

#include <memory>
#include <vector>

namespace SWDWG
{

//! Load (and cache) a parsed .shx shape file by its DWG STYLE file name.
//! Resolves the file through the standard AutoCAD / Bentley support paths plus
//! the FreeCAD home directory; returns null when the file is unavailable.
//! Implemented in DWGLineGroup.cpp; shared with the importer so it can size
//! zero-length shape dashes (LineTypeDash::symWidth) from the shape body.
std::shared_ptr<const ShapeFile> loadShapeFile(const std::string& name);

//! Cut every free edge of \a shape with the linetype pattern (\a dashes x
//! \a scale).  \a patternLen is the DWG-stored pattern length in drawing
//! units (0 = unknown): it lets the cutter snap a line of exactly
//! N x pattern_len to a whole number of periods despite float rounding in
//! the dash widths.  Returns a new compound, or the input unchanged when the
//! shape is not cuttable.
SWDWGExport TopoDS_Shape expandShapeByPattern(
    const TopoDS_Shape& shape,
    const std::vector<LineTypeDash>& dashes,
    double scale,
    double patternLen = 0.0);

class SWDWGExport DWGLineGroup : public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::DWGLineGroup);

public:
    DWGLineGroup();
    ~DWGLineGroup() override;

    /// Raw geometry (compound / wire / edge) as built by the importer, and the
    /// only geometry that is persisted - as binary BRep (see
    /// PropertyShapeBinary).  Read by the display view provider, by execute()
    /// and by SWDTM's contour extraction (by property name).  A materialising
    /// cut never overwrites it, so the cut can always be undone / re-applied.
    PropertyShapeBinary DWGSourceShape;
    /// DWG layer name.
    App::PropertyString Layer;
    /// Imported DWG entity kind (read-only, tree grouping key).
    App::PropertyString EntityKind;
    /// Resolved colour (entity > layer).
    App::PropertyColor DWGColor;
    /// Line weight in mm.
    App::PropertyFloat DWGLineWidth;
    /// Resolved linetype name (inheritance chain end).
    App::PropertyString DWGLineType;
    /// L1 quantized 16-bit pattern (diagnostics / fallback).
    App::PropertyInteger DWGLinePattern;
    /// Effective scale = LTSCALE x entity ltype scale (import-time value).
    App::PropertyFloat DWGLineScale;
    /// Shared linetype definition; when set the geometry is cut (L2),
    /// otherwise (CONTINUOUS / unknown) the raw shape is used unchanged.
    App::PropertyLink DWGLineTypeRef;
    /// Direction-2 / P3: when true, execute() really cuts the dashes into
    /// Shape (large geometry - use on demand for export / measurement /
    /// printing).  Default false keeps the raw geometry (light; the display
    /// view provider strokes the dashes from DWGSourceShape instead).
    App::PropertyBool DashesMaterialized;

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;

    /// Marks the derived Shape mirror transient right before the property list
    /// is written, so only DWGSourceShape (binary BRep) is persisted.  It is
    /// done here instead of in the constructor on purpose: a property that is
    /// already transient is skipped by PropertyContainer::Restore() for files
    /// that carry no status for it - which is exactly how documents written by
    /// older builds look, and they must keep loading their geometry.
    void beforeSave() const override;

    /// Shape is transient (DWGSourceShape carries the persisted geometry), so
    /// it is refilled here when a document is opened - the view providers, the
    /// Part tools and SWDTM all read geometry right after opening.  Documents
    /// written by older builds only carry Shape (or a DWGSourceShape whose
    /// type name no longer matches), so the fill runs in whichever direction
    /// the file is missing data.
    void onDocumentRestored() override;

    /// returns the type name of the ViewProvider.  Every line group uses the
    /// dedicated lightweight provider (SWDWG::ViewProviderDWGLineGroup),
    /// which draws the raw source edges and, for groups with a linetype
    /// definition (L2), strokes the dashes itself from DWGSourceShape +
    /// pattern - so the heavy cut geometry is never materialised / persisted.
    /// (VP selection happens once at attach time, before DWGLineTypeRef is
    /// populated during import, so it must not depend on that link.)
    const char* getViewProviderName() const override
    {
        return "SWDWG::ViewProviderDWGLineGroup";
    }
};

}  // namespace SWDWG
