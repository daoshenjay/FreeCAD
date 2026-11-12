//! DIMENSION entity support.
//!
//! The LibreDWG -> DimensionData extraction lives in DWGDimension.cpp (it
//! needs the libredwg headers, like every parsing TU).  This header exposes
//! the two parts that are free of that dependency so that the Gui module can
//! use them without pulling in libredwg:
//!   - buildDimensionGeometry(): pure geometry (dimension line, extension
//!     lines, arrow heads, text anchor) computed from the plain
//!     DimensionData / DimStyle structs;
//!   - the DWGDimension document object that carries the resolved display
//!     data (line/arrow points, text, colours) into the view provider.

#pragma once

#include <App/Annotation.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>

#include <string>
#include <vector>

#include "DWGTypes.h"
#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! Resolved display geometry of one DIMENSION entity, in world coordinates.
//! Lines are emitted as consecutive point pairs (segment i = points 2i and
//! 2i+1); arrow heads as triangles (points 3k..3k+2 each).  dimLines and
//! extLines are separated so the view provider can colour the dimension line
//! (DIMCLRD) differently from the extension lines (DIMCLRE).
struct DimGeom
{
    std::vector<Vec3> dimLines;  //!< dimension line, arcs, leaders (DIMCLRD)
    std::vector<Vec3> extLines;  //!< extension lines (DIMCLRE)
    std::vector<Vec3> arrows;    //!< arrow head triangles (DIMCLRD)
    Vec3 textPos;                //!< center anchor of the dimension text
    double textRot = 0.0;        //!< radians
    std::string text;            //!< formatted measurement text ("" = hidden)
};

//! Compute the full display geometry of a DIMENSION entity from its raw
//! defining points and the resolved DIMSTYLE.  Pure math: no FreeCAD and no
//! LibreDWG involvement, so it can be unit-tested in isolation.
SWDWGExport bool buildDimensionGeometry(const DimensionData& dim,
                                        const DimStyle& style, DimGeom& out);

//! A DIMENSION entity of a DWG drawing, flattened for display.
//!
//! The import pipeline resolves the raw DIMENSION data + DIMSTYLE through
//! buildDimensionGeometry() once and stores the resulting world-space
//! segments / arrow triangles / text anchor as plain properties.  The view
//! provider (SWDWG::ViewProviderDWGDimension) renders exactly these
//! properties, so the geometry never drifts between the App and Gui layers.
class SWDWGExport DWGDimension: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::DWGDimension);

public:
    /// Constructor
    DWGDimension();
    ~DWGDimension() override;

    /// DIMENSION sub-type name: "LINEAR", "ALIGNED", "ANG2LN", "ANG3PT",
    /// "DIAMETER", "RADIUS" or "ORDINATE".
    App::PropertyString DimType;
    /// Dimension line / arc / leader segments, two consecutive points per
    /// segment.  Rendered with DimColor (DIMCLRD).
    App::PropertyVectorList DimLines;
    /// Extension line segments, two consecutive points per segment.
    /// Rendered with ExtColor (DIMCLRE).
    App::PropertyVectorList ExtLines;
    /// Arrow head triangles, three consecutive points per triangle.
    /// Rendered with DimColor.
    App::PropertyVectorList ArrowTriangles;
    /// Left-baseline anchor of the dimension text (world coordinates).
    App::PropertyVector TextPosition;
    /// Text rotation about Z (radians).
    App::PropertyAngle TextRotation;
    /// Formatted measurement text (e.g. "12.50" or "R15").
    App::PropertyString DimText;
    /// Text height in world units (DIMTXT x DIMSCALE).
    App::PropertyFloat TextHeight;
    /// Raw measured value (world units) before formatting.
    App::PropertyFloat Measurement;
    /// Colour of dimension line + arrow heads.
    App::PropertyColor DimColor;
    /// Colour of extension lines.
    App::PropertyColor ExtColor;
    /// Colour of the dimension text.
    App::PropertyColor TextColor;
    /// DWG layer name the entity lives on.
    App::PropertyString Layer;
    /// Imported DWG entity kind (read-only, tree grouping key).
    App::PropertyString EntityKind;
    /// Referenced DIMSTYLE table entry name.
    App::PropertyString DimStyleName;
    /// Name of the text style used for the dimension text (DIMTXSTY).
    App::PropertyString TextStyleName;
    /// Width factor (horizontal stretch) of the dimension text.
    App::PropertyFloat TextWidthFactor;
    /// STYLE font file name of the dimension text (e.g. "txt.shx"), empty
    /// when the text style has no .shx font.
    App::PropertyString FontFile;
    /// STYLE big-font (CJK) file name of the dimension text (e.g.
    /// "fsdb.shx"), empty when none.
    App::PropertyString BigFontFile;
    /// DWG codepage used to map the dimension text to font shape numbers.
    App::PropertyInteger Codepage;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "SWDWG::ViewProviderDWGDimension";
    }

    //! Enter / leave a bulk-create window (importer filling a fresh object).
    //! While active the view provider defers rebuildGeometry(), so a
    //! dimension with ~15 properties does not rebuild its geometry / stroked
    //! text once per property.  Nested use is counted.
    static void enterBulkCreate();
    static void leaveBulkCreate();
    static bool isBulkCreating();

protected:
    void onChanged(const App::Property* prop) override;

private:
    bool m_bRecomputing = false;
};

}  // namespace SWDWG
