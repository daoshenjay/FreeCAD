//! Demo document object for Coin3D SoMultipleInstance instancing.
//!
//! The drawing / linetype code frequently needs to place the SAME small
//! glyph / symbol geometry at many places (hatch arrows, complex-linetype
//! symbols, dimension ticks, ...).  Building an independent scene-graph
//! branch per occurrence is O(N) nodes; SoMultipleInstance renders one
//! template geometry N times from an array of instance matrices, which is
//! both faster to build and faster to draw.
//!
//! This type is a *test harness*: it holds a simple regular grid of
//! instance placements and lets the accompanying view provider
//! (ViewProviderMultiInstanceTest) exercise the instancing path.  It shows
//! how an App object + ViewProvider pair can feed a matrix array into an
//! SoMultipleInstance node.

#pragma once

#include <App/DocumentObject.h>
#include <App/PropertyStandard.h>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! A grid of identical marker instances (rows x cols), rendered through
//! one SoMultipleInstance in the view provider.
class SWDWGExport MultiInstanceTest: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::MultiInstanceTest);

public:
    MultiInstanceTest();
    ~MultiInstanceTest() override;

    /// Number of rows of the instance grid.
    App::PropertyInteger Rows;
    /// Number of columns of the instance grid.
    App::PropertyInteger Cols;
    /// Distance between adjacent instances (world units).
    App::PropertyFloat Spacing;
    /// Template selector: 0 = diamond outline, 1 = plus sign,
    /// 2 = eight-sided disc outline.  (SoMultipleInstance reuses the same
    /// template for every instance; changing this swaps the template.)
    App::PropertyInteger Template;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "SWDWG::ViewProviderMultiInstanceTest";
    }

protected:
    void onChanged(const App::Property* prop) override;
};

}  // namespace SWDWG
