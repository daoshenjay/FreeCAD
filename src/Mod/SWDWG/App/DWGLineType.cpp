//! Shared DWG linetype definition object - implementation.
//!
//! See DWGLineType.h for the design.  The importer creates one instance per
//! non-continuous LINETYPE table entry and links every layer group that uses
//! it through the dynamic DWGLineTypeRef property.  Editing Pattern / Scale
//! here refreshes all linked groups at once.

#include "PreCompiled.h"

#include "DWGLineType.h"

#include <App/Document.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

using namespace SWDWG;

namespace
{

//! Parse the compact "12.7,-6.35,0" dash list into LineTypeDash entries.
//! Whitespace, commas and semicolons separate the values.
//! Complex elements are prefixed: "s<length>:<code>" for a shape,
//! "t<length>:<text>" for an embedded text run.
std::vector<LineTypeDash> parseDashList(const std::string& text)
{
    std::vector<LineTypeDash> dashes;
    const char* p = text.c_str();
    while (p && *p) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == ';') {
            ++p;
        }
        if (!*p) {
            break;
        }
        LineTypeDash d;
        if (*p == 's' || *p == 'S' || *p == 't' || *p == 'T') {
            // Complex element.
            const bool shape = (*p == 's' || *p == 'S');
            ++p;
            char* end = nullptr;
            d.length = std::strtod(p, &end);
            if (end == p) {
                break;  // malformed -> stop
            }
            d.shapeFlag = shape ? 2 : 4;
            p = end;
            if (*p == ':') {
                ++p;
                long code = std::strtol(p, &end, 10);
                if (end != p) {
                    d.complexShapecode = (int)code;
                    p = end;
                    // "s<length>:<code>@<shapefile>": the .shx file the
                    // shape lives in, kept for the L2 shape restore.
                    if (shape && *p == '@') {
                        ++p;
                        const char* s0 = p;
                        while (*p && *p != ',' && *p != ';' && *p != '#') {
                            ++p;
                        }
                        d.shapefile.assign(s0, p);
                    }
                }
                else {
                    d.complexShapecode = 0;
                    // "t<length>:<text>[@<shapefile>]" form: the text follows
                    // directly, optionally trailed by the TrueType font file
                    // name (arial.ttf) that the TTF branch needs.
                    const char* t0 = p;
                    while (*p && *p != ',' && *p != ';' && *p != '#'
                           && *p != '@') {
                        ++p;
                    }
                    d.text.assign(t0, p);
                    if (*p == '@') {
                        ++p;
                        const char* s0 = p;
                        while (*p && *p != ',' && *p != ';' && *p != '#') {
                            ++p;
                        }
                        d.shapefile.assign(s0, p);
                    }
                }
            }
            else if (!shape) {
                // Text without a code: the rest of the token is the text,
                // optionally followed by "@<shapefile>".
                const char* t0 = p;
                while (*p && *p != ',' && *p != ';' && *p != '#'
                       && *p != '@') {
                    ++p;
                }
                d.text.assign(t0, p);
                if (*p == '@') {
                    ++p;
                    const char* s0 = p;
                    while (*p && *p != ',' && *p != ';' && *p != '#') {
                        ++p;
                    }
                    d.shapefile.assign(s0, p);
                }
            }
            // Symbol placement metadata appended by formatDashList():
            // '#<scale>;<rotation>;<xOffset>;<yOffset>'.  The shape scale S,
            // the rotation (radians) and the two offsets are part of the DWG
            // LTYPE dash record (LibreDWG Dwg_LTYPE_dash); without them the
            // Pattern string round-trip would drop the exact placement that
            // addSymbolGeometry() needs (e.g. BATTING's second BAT rotated
            // by 180 deg and offset to the other side of the line).
            if (*p == '#') {
                ++p;
                char* e2 = nullptr;
                const double sc = std::strtod(p, &e2);
                if (e2 != p) {
                    d.scale = sc;
                    p = e2;
                }
                if (*p == ';') {
                    ++p;
                }
                const double ro = std::strtod(p, &e2);
                if (e2 != p) {
                    d.rotation = ro;
                    p = e2;
                }
                if (*p == ';') {
                    ++p;
                }
                const double xo = std::strtod(p, &e2);
                if (e2 != p) {
                    d.xOffset = xo;
                    p = e2;
                }
                if (*p == ';') {
                    ++p;
                }
                const double yo = std::strtod(p, &e2);
                if (e2 != p) {
                    d.yOffset = yo;
                    p = e2;
                }
                // Optional 5th field: the STYLE width factor, emitted by
                // formatDashList() so embedded text keeps its horizontal
                // stretch across the Pattern round-trip.
                if (*p == ';') {
                    ++p;
                    const double wf = std::strtod(p, &e2);
                    if (e2 != p && wf > 0.0) {
                        d.widthFactor = wf;
                        p = e2;
                    }
                }
            }
        }
        else {
            char* end = nullptr;
            const double v = std::strtod(p, &end);
            if (end == p) {
                break;  // not a number -> stop
            }
            d.length = v;
            p = end;
        }
        dashes.push_back(d);
    }
    return dashes;
}

}  // namespace

PROPERTY_SOURCE(SWDWG::DWGLineType, App::DocumentObject)

DWGLineType::DWGLineType()
{
    ADD_PROPERTY_TYPE(Name,
                      ("CONTINUOUS"),
                      "DWG",
                      App::Prop_None,
                      "LINETYPE table entry name");
    ADD_PROPERTY_TYPE(Pattern,
                      (""),
                      "DWG",
                      App::Prop_None,
                      "Dash list '12.7,-6.35,0': positive = dash, "
                      "negative = space, 0 = dot");
    ADD_PROPERTY_TYPE(Scale,
                      (1.0),
                      "DWG",
                      App::Prop_None,
                      "LTSCALE of the source drawing (informative: each line's "
                      "own DWGLineScale already includes it)");
    ADD_PROPERTY_TYPE(ScaleOverride,
                      (1.0),
                      "DWG",
                      App::Prop_None,
                      "Shared multiplier on each linked line's own linetype "
                      "scale (1.0 = keep every line as imported)");
    ADD_PROPERTY_TYPE(EntityKind,
                      ("LINETYPE"),
                      "DWG",
                      App::Prop_ReadOnly,
                      "Imported object kind (tree grouping key)");
}

DWGLineType::~DWGLineType() = default;

void DWGLineType::rebuildDashes()
{
    const std::string& pat = Pattern.getValue();
    m_dashes = parseDashList(pat);
    // Debug: dump the exact Pattern text and the parsed dash list.  The L2
    // cutter reads this list, so any mismatch with the LINETYPE table (see
    // "[SWDWG LINETYPE]") shows up here.
    SWDWG_LOG(
        "[SWDWG LT] name='%s' pattern='%s' n=%zu\n",
        Name.getValue(), pat.c_str(), m_dashes.size());
    for (std::size_t i = 0; i < m_dashes.size(); ++i) {
        const LineTypeDash& d = m_dashes[i];
        SWDWG_LOG(
            "  [%zu] len=%.6g shapeFlag=%d code=%d text='%s' "
            "scale=%.4g rot=%.4g off=(%.4g,%.4g)\n",
            i, d.length, d.shapeFlag, d.complexShapecode, d.text.c_str(),
            d.scale, d.rotation, d.xOffset, d.yOffset);
    }
}

void DWGLineType::onChanged(const App::Property* prop)
{
    if (prop == &Pattern) {
        rebuildDashes();
    }
    if (m_bUpdating) {
        App::DocumentObject::onChanged(prop);
        return;
    }
    if (prop == &Pattern || prop == &ScaleOverride) {
        updateLinkedFeatures();
    }
    App::DocumentObject::onChanged(prop);
}

void DWGLineType::updateLinkedFeatures()
{
    App::Document* doc = getDocument();
    if (!doc || isRestoring()) {
        return;  // linked groups are not fully loaded yet
    }
    m_bUpdating = true;

    // Re-derive the L1 display data from the edited definition.
    const unsigned short pattern = quantizeLinePattern(m_dashes);
    // Every layer group referencing this linetype inherits the change:
    // refresh its quantized pattern, then recompute so the GUI re-applies the
    // view-provider styles (see SWDWGGui).
    //
    // DWGLineScale is deliberately NOT written here.  It is the group's own
    // import-time scale (LTSCALE x that entity's linetype scale), which differs
    // between the groups sharing one definition; pushing this object's value
    // into all of them destroyed that per-line information and is what made
    // linked lines render 25x too fine or 20x too coarse.  Editing
    // ScaleOverride reaches the renderers on the recompute below, because they
    // resolve the effective scale per group via effectiveLineScale().
    //
    // No links yet (e.g. while the importer is still creating the objects)
    // -> nothing to do and, importantly, no document recompute to trigger.
    bool any = false;
    std::set<App::DocumentObject*> links;
    doc->getLinksTo(links, this, 0);
    for (App::DocumentObject* obj : links) {
        if (!obj) {
            continue;
        }
        if (auto* pp = dynamic_cast<App::PropertyInteger*>(
                obj->getPropertyByName("DWGLinePattern"))) {
            pp->setValue((int)pattern);
            any = true;
        }
        obj->touch();
    }

    m_bUpdating = false;
    if (any) {
        doc->recompute();
    }
}

//! Format a dash list into the compact "12.7,-6.35,0" text form.
//! Complex elements keep their kind: "s<length>:<code>" for shapes and
//! "t<length>:<text>" for embedded text, so the L2 cutter can treat them
//! as ink instead of a gap.
std::string SWDWG::formatDashList(const std::vector<LineTypeDash>& dashes)
{
    std::ostringstream os;
    for (size_t i = 0; i < dashes.size(); ++i) {
        if (i) {
            os << ',';
        }
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.8g", dashes[i].length);
        const LineTypeDash& d = dashes[i];
        if (d.shapeFlag == 2) {
            os << 's' << buf << ':' << d.complexShapecode;
            if (!d.shapefile.empty()) {
                os << '@' << d.shapefile;
            }
            // Placement metadata: '#<scale>;<rotation>;<xOffset>;<yOffset>'.
            // parseDashList() reads it back so the L2 cutter can restore the
            // exact symbol (scale S, rotation, insertion offsets) instead of
            // drawing every occurrence at the line point with no rotation.
            os << '#' << d.scale << ';' << d.rotation << ';'
               << d.xOffset << ';' << d.yOffset << ';' << d.widthFactor;
        }
        else if (d.shapeFlag == 4) {
            os << 't' << buf << ':';
            for (char ch : d.text) {
                if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r'
                    || ch == '#') {
                    os << '_';  // keep the text token single-line
                }
                else {
                    os << ch;
                }
            }
            // The TrueType font file name (e.g. "arial.ttf") must survive the
            // Pattern round-trip or the TTF branch of addTextGlyphs() sees an
            // empty shapefile and falls back to the placeholder box.
            if (!d.shapefile.empty()) {
                os << '@' << d.shapefile;
            }
            os << '#' << d.scale << ';' << d.rotation << ';'
               << d.xOffset << ';' << d.yOffset << ';' << d.widthFactor;
        }
        else {
            os << buf;
        }
    }
    return os.str();
}
