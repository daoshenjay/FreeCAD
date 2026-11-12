//! DIMSTYLE (dimension style table) parsing.
//!
//! Scans every DIMSTYLE object of the DWG and maps the LibreDWG fields to
//! the dependency-free DimStyle struct.  Text and MTEXT entities that carry
//! a dimension-style override use this to resolve the default arrow size,
//! text height, extension-line offsets, colours and so on.

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX  // keep std::min/std::max from Windows macros
#  endif
#  include <Windows.h>
#endif

#include "DWGDimstyle.h"
#include "DWGInternal.h"

#include <Base/Console.h>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>

namespace SWDWG
{

std::map<std::string, DimStyle> SWDWG::parseDimStyles(const Dwg_Data* dwg,
                                                      int codepage)
{
    std::map<std::string, DimStyle> out;
    if (!dwg) {
        return out;
    }
    for (unsigned long i = 0; i < dwg->num_objects; ++i) {
        const Dwg_Object* obj = &dwg->object[i];
        if (!obj || obj->supertype != DWG_SUPERTYPE_OBJECT
            || obj->fixedtype != DWG_TYPE_DIMSTYLE || !obj->tio.object) {
            continue;
        }
        const Dwg_Object_DIMSTYLE* ds = obj->tio.object->tio.DIMSTYLE;
        if (!ds) {
            continue;
        }
        DimStyle st;
        if (ds->name) {
            st.name = decodeDwgString(ds->name, codepage);
        }
        if (ds->DIMSCALE > 0.0) {
            st.scale = ds->DIMSCALE;
        }
        if (ds->DIMASZ > 0.0) {
            st.arrowSize = ds->DIMASZ;
        }
        if (ds->DIMTSZ > 0.0) {
            st.tickSize = ds->DIMTSZ;  // non-zero => ticks replace arrows
        }
        if (ds->DIMTXT > 0.0) {
            st.textHeight = ds->DIMTXT;
        }
        if (ds->DIMEXO >= 0.0) {
            st.extOffset = ds->DIMEXO;
        }
        if (ds->DIMEXE >= 0.0) {
            st.extLength = ds->DIMEXE;
        }
        if (ds->DIMGAP >= 0.0) {
            st.textGap = ds->DIMGAP;
        }
        if (ds->DIMTVP != 0.0) {
            st.textOffset = ds->DIMTVP;
        }
        if (ds->DIMRND > 0.0) {
            st.roundOff = ds->DIMRND;
        }
        st.supExt1 = (ds->DIMSE1 != 0);
        st.supExt2 = (ds->DIMSE2 != 0);
        st.forceLine = (ds->DIMTOFL != 0);
        st.textInside = (ds->DIMTIX != 0);
        st.textPos = static_cast<int>(ds->DIMTAD);
        st.precision = static_cast<int>(ds->DIMDEC);
        st.zeroSuppression = static_cast<int>(ds->DIMZIN);
        if (ds->DIMLUNIT != 0) {
            st.unitType = static_cast<int>(ds->DIMLUNIT);
        }
        // DIMCLR* is a Dwg_Color: either a truecolor rgb value or an ACI
        // colour index (0 BYBLOCK, 256 BYLAYER, negative = layer off).
        // Encoding used by the DimStyle field: 0 = inherit from entity /
        // layer, 0x01000000 | rgb = truecolor, 1..255 = ACI index.
        auto colorVal = [](const Dwg_Color& c) -> unsigned long {
            if (c.method == DWG_COLOR_METHOD_TRUECOLOR) {
                // Some DWG writers store an ACI index with method=TRUECOLOR
                // and the index value in the rgb field.  Values 1..255 must be
                // interpreted as ACI indices, not as near-black RGB.
                unsigned long rgb = c.rgb & 0xFFFFFFUL;
                if (rgb > 0UL && rgb <= 255UL) {
                    return rgb;
                }
                return 0x01000000UL | rgb;
            }
            if (c.method == DWG_COLOR_METHOD_ACI) {
                int idx = static_cast<int>(c.index);
                if (idx > 0 && idx < 256) {
                    return static_cast<unsigned long>(idx) & 0xFFUL;
                }
                return 0UL;  // BYBLOCK/BYLAYER/layer off -> inherit
            }
            if (c.method == DWG_COLOR_METHOD_BYLAYER
                || c.method == DWG_COLOR_METHOD_BYBLOCK
                || c.method == DWG_COLOR_METHOD_NONE
                || c.method == DWG_COLOR_METHOD_VOID) {
                return 0UL;  // inherit from entity / layer
            }
            // Fallback for unexpected method values: use rgb or index if valid.
            if (c.rgb != 0) {
                return 0x01000000UL | (c.rgb & 0xFFFFFFUL);
            }
            int idx = static_cast<int>(c.index);
            if (idx > 0 && idx < 256) {
                return static_cast<unsigned long>(idx) & 0xFFUL;
            }
            return 0UL;
        };
        st.dimColor = colorVal(ds->DIMCLRD);
        st.extColor = colorVal(ds->DIMCLRE);
        st.textColor = colorVal(ds->DIMCLRT);
        // DIMPOST: optional "<>" placeholder separates prefix and suffix.
        if (ds->DIMPOST && *ds->DIMPOST) {
            std::string post = decodeDwgString(ds->DIMPOST, codepage);
            size_t ph = post.find("<>");
            if (ph != std::string::npos) {
                st.prefix = post.substr(0, ph);
                st.suffix = post.substr(ph + 2);
            }
            else if (!post.empty()) {
                st.suffix = post;  // no placeholder: trailing text only
            }
        }
        // DIMTXSTY: name of the text style used for the dimension text.
        if (ds->DIMTXSTY) {
            Dwg_Object* tobj =
                dwg_ref_object(const_cast<Dwg_Data*>(dwg), ds->DIMTXSTY);
            if (tobj) {
                int err = 0;
                char* n = dwg_obj_table_get_name(tobj, &err);
                if (n) {
                    st.textStyle = decodeDwgString(n, codepage);
                }
            }
        }
        out[st.name.empty() ? "Standard" : st.name] = std::move(st);
    }
    for (const auto& kv : out) {
        const DimStyle& s = kv.second;
        SWDWG_LOG(
            "[SWDWG DIMSTYLE] name='%s' scale=%.4f arrow=%.4f tick=%.4f "
            "txtH=%.4f prec=%d zin=%d unit=%d dimCol=%lu extCol=%lu txtCol=%lu "
            "txtStyle='%s' post='%s<>%s'\n",
            s.name.c_str(),
            s.scale,
            s.arrowSize,
            s.tickSize,
            s.textHeight,
            s.precision,
            s.zeroSuppression,
            s.unitType,
            s.dimColor,
            s.extColor,
            s.textColor,
            s.textStyle.c_str(),
            s.prefix.c_str(),
            s.suffix.c_str());
    }
    return out;
}

std::string SWDWG::formatDimValue(double value, const DimStyle& style)
{
    double v = value;
    if (style.roundOff > 0.0) {
        v = std::round(v / style.roundOff) * style.roundOff;
    }
    int prec = style.precision;
    if (prec < 0) {
        prec = 0;
    }
    else if (prec > 8) {
        prec = 8;
    }
    std::ostringstream oss;
    if (style.unitType == 1) {
        oss << std::scientific << std::setprecision(prec) << v;
    }
    else {
        oss << std::fixed << std::setprecision(prec) << v;
    }
    std::string s = oss.str();

    // DIMZIN zero suppression (AutoCAD semantics):
    //   bit4 (value 4/12) = suppress leading zeros  ->  0.50 becomes .50
    //   bit8 (value 8/12) = suppress trailing zeros ->  50.00 becomes 50
    //   value 0 or 3 also suppresses the "precisely zero inches" part of a
    //   decimal value, which is exactly a whole number (50.00 -> 50).
    const int zin = style.zeroSuppression;
    const bool suppressTrailing =
        (zin == 0 || zin == 3 || (zin & 8) != 0);
    const bool suppressLeading = (zin & 4) != 0;
    // Never touch scientific notation (e.g. "5.00e+01").
    const bool isScientific =
        s.find('e') != std::string::npos || s.find('E') != std::string::npos;
    if (suppressTrailing && !isScientific
        && s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
    }
    if (suppressLeading && s.size() > 1 && s[0] == '0' && s[1] == '.') {
        s.erase(0, 1);
    }
    return style.prefix + s + style.suffix;
}

}  // namespace SWDWG
