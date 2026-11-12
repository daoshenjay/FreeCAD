//! Shared DWG linetype definition (mapping level L1 / L2 resource object).
//!
//! A DWG LINETYPE table entry is a drawing-wide resource: every entity that
//! uses, say, "DASHED" must change together when the DASHED definition
//! changes.  This object holds exactly that shared definition (name + dash
//! pattern + scale).  During import the importer creates one DWGLineType per
//! non-continuous linetype and links each layer-group Part::Feature to it
//! through the dynamic DWGLineTypeRef property.
//!
//! Editing Pattern or ScaleOverride on this object automatically refreshes
//! every linked group (its quantized DWGLinePattern is re-derived and the
//! document recomputes), so all lines using the same linestyle update
//! uniformly.  The GUI layer re-applies the view-provider styles on the next
//! recompute.
//!
//! Note that Scale and ScaleOverride are deliberately different things.  The
//! pattern element lengths are scaled per *line*, by the group's own
//! DWGLineScale (= LTSCALE x that entity's linetype scale), because entities
//! sharing one linetype routinely carry different per-entity scales.  A shared
//! definition therefore cannot own "the" effective scale; ScaleOverride is a
//! single multiplier applied on top of each line's own scale.
//!
//! Unlike DWGText / DWGDimension this is a pure data object: it carries no
//! geometry of its own, so it uses the default (invisible) view provider.

#pragma once

#include <App/DocumentObject.h>
#include <App/PropertyStandard.h>

#include <string>
#include <vector>

#include "DWGTypes.h"
#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! Format a dash list into the compact "12.7,-6.35,0" text form used by the
//! DWGLineType::Pattern property (positive = dash, negative = space, 0 = dot).
SWDWGExport std::string formatDashList(const std::vector<LineTypeDash>& dashes);

//! Shared definition of one DWG linetype.
class SWDWGExport DWGLineType: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(SWDWG::DWGLineType);

public:
    /// Constructor
    DWGLineType();
    ~DWGLineType() override;

    /// LINETYPE table entry name (e.g. "DASHED").
    App::PropertyString Name;
    /// Dash list "12.7,-6.35,0": positive = dash, negative = space, 0 = dot.
    App::PropertyString Pattern;
    /// LTSCALE of the drawing the definition came from (informative only).
    ///
    /// Kept for reference / diagnostics.  It is NOT used to render anything:
    /// the pattern lengths are scaled by each group's own DWGLineScale, which
    /// already contains LTSCALE times that entity's linetype scale.
    App::PropertyFloat Scale;
    /// Shared multiplier applied on top of every linked group's own scale.
    ///
    /// 1.0 (the default) keeps each line exactly as imported.  Editing it
    /// rescales every linked line at once, each relative to its own scale, so
    /// lines whose per-entity linetype scale differs stay in proportion.
    App::PropertyFloat ScaleOverride;
    /// Imported object kind (read-only: "LINETYPE", tree grouping key).
    App::PropertyString EntityKind;

    /// Parsed dash list (rebuilt whenever Pattern changes).
    const std::vector<LineTypeDash>& dashes() const
    {
        return m_dashes;
    }

    /// Stored pattern length (DWG LINETYPE pattern_len, drawing units).
    /// AutoCAD sizes the pattern period from this value; it can differ from
    /// the raw |dash| sum by rounding, and using it keeps a line of exactly
    /// N x pattern_len a whole number of periods.
    double patternLen() const
    {
        return m_patternLen;
    }
    void setPatternLen(double v)
    {
        m_patternLen = v;
    }

    /// returns the type name of the ViewProvider (none: pure data object)
    const char* getViewProviderName() const override
    {
        return "";
    }

protected:
    void onChanged(const App::Property* prop) override;

private:
    void rebuildDashes();
    //! Re-derive the L1 display data (DWGLinePattern / DWGLineScale) of every
    //! document object linked to this linetype, then recompute the document
    //! so the GUI re-applies the updated styles.
    void updateLinkedFeatures();

    std::vector<LineTypeDash> m_dashes;
    double m_patternLen = 0.0;
    bool m_bUpdating = false;
};

}  // namespace SWDWG
