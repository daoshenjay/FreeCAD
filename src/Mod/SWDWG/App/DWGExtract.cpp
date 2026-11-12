//! LibreDWG object -> Entity parsing.
//!
//! One case per DWG entity type in extractEntity().  This is the first
//! place to look when a specific entity type (LINE, HATCH, SPLINE, ...)
//! comes in wrong: the raw LibreDWG fields are mapped here.

// NOTE: <Windows.h> must be included BEFORE DWGInternal.h.  The OCC headers
// pulled in by DWGInternal.h include <windows.h> themselves with
// WIN32_LEAN_AND_MEAN + NOMINMAX defined, which would skip the include guard
// and hide the conversion helpers (MultiByteToWideChar, ...) we use here.
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX  // keep std::min/std::max from Windows macros
#  endif
#  include <Windows.h>
#endif

#include "DWGInternal.h"
#include "DWGText.h"
#include "FontResolver.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Base/Console.h>

namespace SWDWG
{

std::vector<std::string> splitUtf8Chars(const std::string& text)
{
    if (text.empty()) {
        return {" "};
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) {
            len = 1;
        }
        else if ((c & 0xE0) == 0xC0) {
            len = 2;
        }
        else if ((c & 0xF0) == 0xE0) {
            len = 3;
        }
        else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        else {
            ++i;
            continue;
        }
        if (i + len > text.size()) {
            break;
        }
        out.emplace_back(text.substr(i, len));
        i += len;
    }
    if (out.empty()) {
        out.emplace_back(" ");
    }
    return out;
}

const char* typeLabel(EntType t)
{
    switch (t) {
        case EntType::LINE:
            return "Line";
        case EntType::CIRCLE:
            return "Circle";
        case EntType::ARC:
            return "Arc";
        case EntType::ELLIPSE:
            return "Ellipse";
        case EntType::POINT:
            return "Point";
        case EntType::INSERT:
            return "Insert";
        case EntType::LWPOLYLINE:
            return "LwpPolyline";
        case EntType::TEXT:
            return "Text";
        case EntType::MTEXT:
            return "MText";
        case EntType::SPLINE:
            return "Spline";
        case EntType::SOLID:
            return "Solid";
        case EntType::FACE3D:
            return "Face3D";
        case EntType::HATCH:
            return "Hatch";
        case EntType::DIMENSION:
            return "Dimension";
        case EntType::LEADER:
            return "Leader";
        default:
            return "Unknown";
    }
}

//! AutoCAD Color Index (ACI) -> packed 0xRRGGBB.
//! Uses the exact palette shipped with LibreDWG (dwg_rgb_palette_index),
//! which mirrors the AutoCAD colour dialog.  A hand-rolled HSV approximation
//! was previously used here; it deviated for the light ACI shades (10-249),
//! so layer colours did not match the source drawing.
unsigned long aciRgb(int idx)
{
    if (idx < 0) {
        idx = -idx;  // negative index = layer turned off (still has a color)
    }
    if (idx <= 0 || idx > 255) {
        return 0xFFFFFF;  // ByLayer / ByBlock fallback: AutoCAD colour 7
    }
    return dwg_rgb_palette_index(idx);
}

//! Pick a CJK-capable font family name.  Coin3D's FreeType font backend on
//! Windows resolves GDI/Qt-style font family names through the system font
//! database, just like FreeCAD's own ViewProviderAnnotation.  STFangsong
//! (华文仿宋) is the default engineering-drawing font used by this project.
std::string findCjkFont()
{
#ifdef _WIN32
    static const char* candidates[] = {
        "STFangsong",        // 华文仿宋, preferred engineering-drawing font
        "FangSong",          // 仿宋 (ZhongYi), classic Chinese serif
        "SimSun",            // 宋体
        "Microsoft YaHei",   // 微软雅黑
        "Noto Sans CJK SC",  // common open-source fallback
        "WenQuanYi Micro Hei",
    };
    return candidates[0];
#endif
    return "Arial";
}

//! Pick a TrueType font name for DWG text.  AutoCAD ships .shx fonts which
//! have no TrueType equivalent; use a CJK-capable system font whenever the
//! text contains non-ASCII characters (most Chinese drawings do).
std::string dwgFontName(const std::string& text)
{
    for (unsigned char c : text) {
        if (c >= 0x80) {
            return findCjkFont();
        }
    }
    return "Arial";
}

//! Map an AutoCAD text style font (usually .shx files) to a TrueType family
//! name for rendering.  Priority: the style's big font for CJK text, then the
//! style font file, then the ASCII/CJK fallback.
//!
//! The engine decision (TTF vs SHX vs big-font) lives in FontResolver.cpp;
//! this legacy helper keeps the same signature for the existing callers that
//! render through the TrueType family name (complex-linetype text, the
//! SoAsciiText view provider) and just returns the mapped TTF family.
std::string mapTextFont(const std::string& fontFile,
                        const std::string& bigFontFile,
                        const std::string& utf8Text)
{
    return resolveTextFont(fontFile, bigFontFile, utf8Text).ttfFamily;
}

//! Return true if the given byte string is valid UTF-8.
static bool isValidUtf8(const std::string& s)
{
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        if ((c & 0x80) == 0) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        }
        else {
            return false;
        }
        if (i + extra >= s.size()) {
            return false;
        }
        for (size_t j = 1; j <= extra; ++j) {
            if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

//! True when the UTF-8 reading of \a s holds a CJK / Kana / Hangul character.
//!
//! Callers must have established that \a s is valid UTF-8 first.  The test
//! exists because a legacy codepage byte string whose pairs *happen* to form
//! valid UTF-8 decodes to U+0080..U+07FF at best (two-byte sequences), never to
//! an ideograph - getting a CJK character out of the UTF-8 reading therefore
//! means the bytes really are UTF-8.  Fullwidth and punctuation ranges are
//! deliberately left out: they add nothing for the name tables this protects
//! and would widen the coincidence window.
static bool hasCjkChar(const std::string& s)
{
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned long cp = c;
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            n = 2;
        }
        else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            n = 3;
        }
        else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            n = 4;
        }
        for (size_t j = 1; j < n && i + j < s.size(); ++j) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3F);
        }
        if ((cp >= 0x3400 && cp <= 0x9FFF)      // CJK ideographs
            || (cp >= 0x3040 && cp <= 0x30FF)   // Hiragana / Katakana
            || (cp >= 0xAC00 && cp <= 0xD7AF)) {  // Hangul syllables
            return true;
        }
        i += n;
    }
    return false;
}

//! Decode a raw DWG string to UTF-8.  Uses the codepage declared in the DWG
//! header when available, otherwise falls back to common CJK encodings.
//! ASCII strings pass through unchanged; on non-Windows platforms the raw bytes
//! are kept.
std::string decodeDwgString(const std::string& raw, int dwgCodepage)
{
    if (raw.empty()) {
        return raw;
    }

    bool hasHigh = false;
    for (unsigned char c : raw) {
        if (c >= 0x80) {
            hasHigh = true;
            break;
        }
    }
    if (!hasHigh) {
        return raw;  // pure ASCII
    }

    // Helper: decode raw bytes with a Windows codepage and return UTF-8.
    auto decode = [](const std::string& in, int cp) -> std::string {
#ifdef _WIN32
        int wlen = MultiByteToWideChar(
            cp, MB_ERR_INVALID_CHARS, in.c_str(), (int)in.size(), nullptr, 0);
        if (wlen <= 0) {
            return {};
        }
        std::wstring wstr(wlen, L'\0');
        if (MultiByteToWideChar(cp,
                                MB_ERR_INVALID_CHARS,
                                in.c_str(),
                                (int)in.size(),
                                wstr.data(),
                                wlen)
            != wlen) {
            return {};
        }
        int ulen = WideCharToMultiByte(
            CP_UTF8, 0, wstr.c_str(), wlen, nullptr, 0, nullptr, nullptr);
        if (ulen <= 0) {
            return {};
        }
        std::string out(ulen, '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, wstr.c_str(), wlen, out.data(), ulen, nullptr, nullptr);
        return out;
#else
        (void)in;
        (void)cp;
        return {};
#endif
    };

    // Helper: score how "reasonable" a decoded UTF-8 string looks.
    // Higher scores mean more ASCII/CJK/punctuation and fewer controls/PUA.
    auto scoreText = [](const std::string& utf8) -> int {
#ifdef _WIN32
        int wlen = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        if (wlen <= 0) {
            return -1000;
        }
        std::wstring wstr(wlen, L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), (int)utf8.size(), wstr.data(), wlen);
        int score = 0;
        for (wchar_t wc : wstr) {
            if (wc == L' ' || wc == L'\t' || wc == L'\n' || wc == L'\r') {
                score += 1;
            }
            else if (wc >= 0x20 && wc <= 0x7E) {
                score += 2;  // ASCII printable
            }
            else if ((wc >= 0x4E00 && wc <= 0x9FFF)
                     || (wc >= 0x3400 && wc <= 0x4DBF)) {
                score += 3;  // CJK ideographs
            }
            else if (wc >= 0x3040 && wc <= 0x309F) {
                score += 3;  // Hiragana
            }
            else if (wc >= 0x30A0 && wc <= 0x30FF) {
                score += 3;  // Katakana
            }
            else if (wc >= 0xAC00 && wc <= 0xD7AF) {
                score += 3;  // Hangul syllables
            }
            else if ((wc >= 0x3000 && wc <= 0x303F)
                     || (wc >= 0xFF00 && wc <= 0xFFEF)) {
                score += 1;  // CJK/fullwidth punctuation
            }
            else if (wc < 0x20) {
                score -= 5;  // control characters
            }
            else if (wc >= 0xE000 && wc <= 0xF8FF) {
                score -= 3;  // private use area
            }
        }
        return score;
#else
        (void)utf8;
        return 0;
#endif
    };

    // 1) The file may hold UTF-8 even when the header declares a legacy
    //    codepage, because a drawing's strings come from wherever its content
    //    came from.  Measured on one bridge plan: of 192 layer names, 19 are
    //    stored as UTF-8 and 140 as GBK, and decoding the UTF-8 ones with the
    //    declared 936 turned them into mojibake ("0-护栏" -> "0-銶ゆ窗",
    //    "施工范围" -> "鏈旂伐鑼冨洿") - 14 of the 40 layers that carry geometry.
    //    The header codepage is a good default, not a guarantee.
    //
    //    See hasCjkChar() for why a CJK character in the UTF-8 reading is
    //    enough to settle it; the check is narrow on purpose so that genuine
    //    codepage text (and Latin-1 text such as an accented "Beton") keeps
    //    going through the declared codepage below.
    if (isValidUtf8(raw) && hasCjkChar(raw)) {
        return raw;
    }

    // 2) Trust the DWG header codepage if it is declared and not UTF-8.
    if (dwgCodepage > 0 && dwgCodepage != CP_UTF8) {
        std::string out = decode(raw, dwgCodepage);
        if (!out.empty()) {
            return out;
        }
    }

    // 3) Score UTF-8 and the common East-Asian codepages, then pick the
    //    best candidate.  This prevents valid UTF-8 CJK strings from being
    //    mis-decoded as GBK/Big5, while still recovering GBK strings that
    //    happen to be syntactically valid UTF-8 but decode to garbage.
    std::string best;
    int bestScore = -10000;
    auto consider = [&](const std::string& candidate) {
        if (candidate.empty()) {
            return;
        }
        int s = scoreText(candidate);
        if (s > bestScore) {
            bestScore = s;
            best = candidate;
        }
    };

    if (isValidUtf8(raw)) {
        consider(raw);  // valid UTF-8 is always a candidate
    }

    static const int fallbackCPs[] = {936, 950, 932, 949};
    for (int cp : fallbackCPs) {
        consider(decode(raw, cp));
    }

    if (!best.empty() && bestScore > 0) {
        return best;
    }

    // 4) Last resort: keep the original bytes.
    return raw;
}

//! Resolve a TEXT/MTEXT style handle reference: read the STYLE table record
//! name and copy the style's font files into \a ta.  Style-table defaults
//! (width factor, oblique, generation) are applied only for values the
//! entity does not carry itself (MTEXT has no such fields of its own).
static void applyTextStyle(const Dwg_Data* dwg, Dwg_Object_Ref* styleRef,
                           Entity::TextAttrs& ta, int codepage,
                           const std::map<std::string, TextStyle>* styles)
{
    if (styleRef) {
        Dwg_Object* sobj =
            dwg_ref_object(const_cast<Dwg_Data*>(dwg), styleRef);
        SWDWG_LOG(
            "SWDWG: applyTextStyle ref.obj=%s abs=%d -> sobj=%s fixedtype=%d "
            "(STYLE=%d) supertype=%d\n",
            styleRef->obj ? "set" : "NULL",
            static_cast<int>(styleRef->absolute_ref),
            sobj ? "resolved" : "NULL",
            sobj ? (int)sobj->fixedtype : -1,
            (int)DWG_TYPE_STYLE,
            sobj ? (int)sobj->supertype : -1);
        if (sobj && sobj->supertype == DWG_SUPERTYPE_OBJECT
            && sobj->fixedtype == DWG_TYPE_STYLE) {
            const Dwg_Object_STYLE* st = sobj->tio.object->tio.STYLE;
            if (st && st->name) {
                ta.styleName = decodeDwgString(st->name, codepage);
                if (ta.fontFile.empty() && st->font_file) {
                    ta.fontFile = st->font_file;
                }
                if (ta.bigFontFile.empty() && st->bigfont_file) {
                    ta.bigFontFile = st->bigfont_file;
                }
                // DXF STYLE group 70 is bit-coded: 0x04 = vertical text,
                // 0x01 = "this entry describes a shape".  Testing 0x01 - as
                // this used to - never recognised a vertical style and could
                // instead mark a shape-font style vertical.
                ta.isVertical = (st->is_vertical != 0)
                    || ((st->flag & 0x04) != 0);
                if (ta.widthFactor <= 0.0 && st->width_factor > 0.0) {
                    ta.widthFactor = st->width_factor;
                }
                // Fallback: some drawings use an @-prefixed font file to mean
                // vertical text, even when the vertical flag is not set.
                if (!ta.isVertical && st->font_file) {
                    std::string ff = st->font_file;
                    std::transform(ff.begin(), ff.end(), ff.begin(), ::tolower);
                    if (!ff.empty() && ff[0] == '@') {
                        ta.isVertical = true;
                    }
                }
                SWDWG_LOG(
                    "SWDWG: applyTextStyle direct style='%s' is_vertical=%d "
                    "flag=%d font_file='%s' bigfont='%s'\n",
                    ta.styleName.c_str(),
                    ta.isVertical ? 1 : 0,
                    static_cast<int>(st->flag),
                    st->font_file ? st->font_file : "(null)",
                    st->bigfont_file ? st->bigfont_file : "(null)");
            }
        }
    }
    // Inherit the style table defaults for attributes the entity lacks.
    if (styles && !ta.styleName.empty()) {
        auto it = styles->find(ta.styleName);
        if (it != styles->end()) {
            const TextStyle& s = it->second;
            if (ta.fontFile.empty()) {
                ta.fontFile = s.fontFile;
            }
            if (ta.bigFontFile.empty()) {
                ta.bigFontFile = s.bigFontFile;
            }
            if (ta.widthFactor <= 0.0) {
                ta.widthFactor = s.widthFactor;
            }
            if (s.oblique != 0.0) {
                ta.oblique = s.oblique;
            }
            if (s.generation != 0) {
                ta.generation = s.generation;
            }
            if (!ta.isVertical && s.isVertical) {
                ta.isVertical = true;
            }
            SWDWG_LOG(
                "SWDWG: applyTextStyle map style='%s' isVertical=%d font='%s' "
                "bigfont='%s' wf=%.4f oblique=%.4f gen=%d\n",
                s.name.c_str(),
                ta.isVertical ? 1 : 0,
                s.fontFile.c_str(),
                s.bigFontFile.c_str(),
                ta.widthFactor,
                ta.oblique,
                ta.generation);
            if (s.isVertical) {
                ta.isVertical = true;
            }
            if (!ta.isVertical && !s.fontFile.empty()
                && s.fontFile[0] == '@') {
                ta.isVertical = true;
            }
            SWDWG_LOG(
                "SWDWG: applyTextStyle inherited style='%s' is_vertical=%d font_file='%s'\n",
                ta.styleName.c_str(),
                ta.isVertical ? 1 : 0,
                s.fontFile.c_str());
        }
    }
}

bool extractEntity(const Dwg_Data* dwg, const Dwg_Object* obj, Entity& e,
                   const PolyVertMap& polyVerts, int codepage,
                   const std::map<std::string, TextStyle>* textStyles)
{
    if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity) {
        return false;
    }
    const Dwg_Object_Entity* ent = obj->tio.entity;
    if (!ent) {
        return false;
    }

    switch (obj->fixedtype) {
        case DWG_TYPE_LINE: {
            const Dwg_Entity_LINE* L = ent->tio.LINE;
            e.type = EntType::LINE;
            e.pts = {
                Vec3{L->start.x, L->start.y, L->start.z},
                Vec3{L->end.x, L->end.y, L->end.z},
            };
            break;
        }
        case DWG_TYPE_CIRCLE: {
            const Dwg_Entity_CIRCLE* C = ent->tio.CIRCLE;
            e.type = EntType::CIRCLE;
            e.center = Vec3{C->center.x, C->center.y, C->center.z};
            e.radius = C->radius;
            e.extrusion = Vec3{C->extrusion.x, C->extrusion.y, C->extrusion.z};
            break;
        }
        case DWG_TYPE_ARC: {
            const Dwg_Entity_ARC* A = ent->tio.ARC;
            e.type = EntType::ARC;
            e.center = Vec3{A->center.x, A->center.y, A->center.z};
            e.radius = A->radius;
            e.startAngle = A->start_angle;
            e.endAngle = A->end_angle;
            e.extrusion = Vec3{A->extrusion.x, A->extrusion.y, A->extrusion.z};
            break;
        }
        case DWG_TYPE_ELLIPSE: {
            const Dwg_Entity_ELLIPSE* E = ent->tio.ELLIPSE;
            e.type = EntType::ELLIPSE;
            e.center = Vec3{E->center.x, E->center.y, E->center.z};
            e.smAxis = Vec3{E->sm_axis.x, E->sm_axis.y, E->sm_axis.z};
            e.axisRatio = E->axis_ratio;
            e.startAngle = E->start_angle;
            e.endAngle = E->end_angle;
            e.extrusion = Vec3{E->extrusion.x, E->extrusion.y, E->extrusion.z};
            break;
        }
        case DWG_TYPE_POINT: {
            const Dwg_Entity_POINT* P = ent->tio.POINT;
            e.type = EntType::POINT;
            e.pts = {Vec3{P->x, P->y, P->z}};
            break;
        }
        case DWG_TYPE_INSERT: {
            const Dwg_Entity_INSERT* I = ent->tio.INSERT;
            e.type = EntType::INSERT;
            e.insPt = Vec3{I->ins_pt.x, I->ins_pt.y, I->ins_pt.z};
            e.scale = Vec3{I->scale.x, I->scale.y, I->scale.z};
            e.rotation = I->rotation;
            // The insertion point and rotation live in the entity's OCS (DXF
            // 210-230), not in world coordinates.  A reference that went through
            // AutoCAD MIRROR carries (0,0,-1) and its stored numbers are
            // mirrored: dropping the field placed exactly those references on
            // the wrong side (measured on the bridge plan: the pile family is
            // inserted in mirrored pairs at 350 deg / 0 deg, and half of them
            // landed ~67 m away, see docs/31).
            e.extrusion = Vec3{I->extrusion.x, I->extrusion.y, I->extrusion.z};
            if (I->block_header) {
                Dwg_Object* bobj = dwg_ref_object(const_cast<Dwg_Data*>(dwg),
                                                  I->block_header);
                if (bobj) {
                    int err = 0;
                    char* n = dwg_obj_table_get_name(bobj, &err);
                    if (n) {
                        e.blockName = decodeDwgString(n, codepage);
                    }
                }
            }
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            const Dwg_Entity_LWPOLYLINE* W = ent->tio.LWPOLYLINE;
            e.type = EntType::LWPOLYLINE;
            e.closed = (W->flag & 0x200) != 0;
            e.elevation = W->elevation;
            e.extrusion = Vec3{W->extrusion.x, W->extrusion.y, W->extrusion.z};
            // LWPOLYLINE stores only x/y per vertex; the whole run's height
            // is the entity elevation (DXF 38).  Bake it into Z now or every
            // contour / 2D polyline would be dropped to Z=0.
            for (unsigned long k = 0; k < W->num_points; ++k) {
                e.pts.push_back(Vec3{W->points[k].x, W->points[k].y,
                                     W->elevation});
            }
            for (unsigned long k = 0; k < W->num_bulges; ++k) {
                e.bulges.push_back(W->bulges[k]);
            }
            break;
        }
        case DWG_TYPE_POLYLINE_2D: {
            const Dwg_Entity_POLYLINE_2D* P = ent->tio.POLYLINE_2D;
            e.type = EntType::LWPOLYLINE;
            e.closed = (P->flag & 0x1) != 0;
            // DXF 70: bit1 = curve fit, bit2 = spline fit -> smooth spline.
            e.fitSpline = (P->flag & 0x6) != 0;
            e.elevation = P->elevation;
            e.extrusion = Vec3{P->extrusion.x, P->extrusion.y, P->extrusion.z};
            auto it = polyVerts.find(obj);
            if (it != polyVerts.end()) {
                for (const PolyVert& v : it->second) {
                    e.pts.push_back(v.p);
                    e.bulges.push_back(v.bulge);
                }
            }
            if (e.pts.empty()) {
                return false;
            }
            // A true 2D POLYLINE lies in one plane: the per-vertex Z read from
            // the old VERTEX_2D records is only a (often unreliable) copy of
            // the entity elevation - some writers leave it 0 on some vertices,
            // which used to leave those vertices at Z=0 and "drew" spurious
            // near-vertical walls inside one polyline.  The authoritative
            // height is the POLYLINE's own elevation (DXF 30): force it onto
            // every vertex.  Polylines flagged 3D (flag bit 8) keep their own
            // per-vertex Z and are never touched here.
            if (!(P->flag & 0x8) && e.elevation != 0.0) {
                for (Vec3& p : e.pts) {
                    p.z = e.elevation;
                }
            }
            break;
        }
        case DWG_TYPE_POLYLINE_3D: {
            const Dwg_Entity_POLYLINE_3D* P = ent->tio.POLYLINE_3D;
            e.type = EntType::LWPOLYLINE;
            e.closed = (P->flag & 0x1) != 0;
            e.fitSpline = (P->flag & 0x6) != 0;
            auto it = polyVerts.find(obj);
            if (it != polyVerts.end()) {
                for (const PolyVert& v : it->second) {
                    e.pts.push_back(v.p);
                    e.bulges.push_back(0.0);
                }
            }
            if (e.pts.empty()) {
                return false;
            }
            break;
        }
        case DWG_TYPE_TEXT: {
            const Dwg_Entity_TEXT* T = ent->tio.TEXT;
            e.type = EntType::TEXT;
            // TEXT stores no per-point Z: the anchor height IS the entity
            // elevation (DXF 30).  Take it unconditionally - some writers
            // (block texts included) carry a valid elevation without setting
            // the dataflags presence bit, which used to flatten them to 0.
            const double el = T->elevation;
            // Aligned(3) / Fit(5): ins_pt and alignment_pt are the two
            // baseline endpoints.  The rotation follows the connecting line
            // and, for Fit, the height is derived so the text fills the
            // span.  The anchor (rotation centre) is the first point.
            const bool twoPoint = (T->horiz_alignment == 3
                                   || T->horiz_alignment == 5);
            Vec3 pos;
            if (twoPoint) {
                pos = Vec3{T->ins_pt.x, T->ins_pt.y, el};
                e.textAttrs.alignPt2 =
                    Vec3{T->alignment_pt.x, T->alignment_pt.y, el};
                e.textAttrs.hasAlignPt2 = true;
            }
            else if (T->horiz_alignment != 0 || T->vert_alignment != 0) {
                // Non-left/baseline alignments: alignment_pt is the anchor
                // (the point the text is positioned relative to).
                pos = Vec3{T->alignment_pt.x, T->alignment_pt.y, el};
            }
            else {
                // Left/baseline: the insertion point is the left baseline.
                pos = Vec3{T->ins_pt.x, T->ins_pt.y, el};
            }
            e.pts = {pos};
            // ===================== 导出功能 BEGIN =====================
            // 同时保留原始插入点（DXF 组码 10）。设了对齐方式时它是文字框左下角
            // 的基线点，导出按左/基线写回这个点即可复现原位置。
            e.insPt = Vec3{T->ins_pt.x, T->ins_pt.y, el};
            // ===================== 导出功能 END =======================
            e.textHeight = T->height;
            // DWG stores angles as radians; the importer keeps radians.
            e.rotation = T->rotation;
            e.textHAlign = T->horiz_alignment;
            e.textVAlign = T->vert_alignment;
            if (T->text_value) {
                e.text = T->text_value;
            }
            // Text metrics carried by the entity (always written in DWG).
            e.textAttrs.widthFactor =
                (T->width_factor > 0.0) ? T->width_factor : 1.0;
            e.textAttrs.oblique = T->oblique_angle;
            e.textAttrs.generation = static_cast<int>(T->generation);
            e.textAttrs.normal =
                Vec3{T->extrusion.x, T->extrusion.y, T->extrusion.z};
            applyTextStyle(dwg, T->style, e.textAttrs, codepage, textStyles);
            SWDWG_LOG(
                "SWDWG: TEXT '%s' after applyTextStyle isVertical=%d "
                "styleName='%s' rot=%.4f\n",
                decodeDwgString(e.text, codepage).c_str(),
                e.textAttrs.isVertical ? 1 : 0,
                e.textAttrs.styleName.c_str(),
                T->rotation);
            break;
        }
        case DWG_TYPE_ATTRIB: {
            // Block-instance attribute text (tag = e.g. equipment number).
            // ATTRIB behaves exactly like TEXT for display: insertion point,
            // height/rotation/style are all stored in the owning space, so no
            // extra block transform is needed.  We import them as TEXT so they
            // share the whole annotation pipeline (layout / alignment / SHX).
            const Dwg_Entity_ATTRIB* A = ent->tio.ATTRIB;
            e.type = EntType::TEXT;
            // Invisible attributes are hidden unless ATTDISP is forced on;
            // skip them to match what AutoCAD normally shows.
            if (A->flags & 0x1) {
                return false;
            }
            const double el = A->elevation;  // see the TEXT case above
            const bool twoPoint = (A->horiz_alignment == 3
                                   || A->horiz_alignment == 5);
            Vec3 pos;
            if (twoPoint) {
                pos = Vec3{A->ins_pt.x, A->ins_pt.y, el};
                e.textAttrs.alignPt2 =
                    Vec3{A->alignment_pt.x, A->alignment_pt.y, el};
                e.textAttrs.hasAlignPt2 = true;
            }
            else if (A->horiz_alignment != 0 || A->vert_alignment != 0) {
                pos = Vec3{A->alignment_pt.x, A->alignment_pt.y, el};
            }
            else {
                pos = Vec3{A->ins_pt.x, A->ins_pt.y, el};
            }
            e.pts = {pos};
            e.textHeight = A->height;
            e.rotation = A->rotation;
            e.textHAlign = A->horiz_alignment;
            e.textVAlign = A->vert_alignment;
            if (A->text_value) {
                e.text = A->text_value;
            }
            e.textAttrs.widthFactor =
                (A->width_factor > 0.0) ? A->width_factor : 1.0;
            e.textAttrs.oblique = A->oblique_angle;
            e.textAttrs.generation = static_cast<int>(A->generation);
            e.textAttrs.normal =
                Vec3{A->extrusion.x, A->extrusion.y, A->extrusion.z};
            applyTextStyle(dwg, A->style, e.textAttrs, codepage, textStyles);
            SWDWG_LOG(
                "SWDWG: ATTRIB '%s' (tag='%s') -> TEXT style='%s'\n",
                decodeDwgString(e.text, codepage).c_str(),
                A->tag ? decodeDwgString(A->tag, codepage).c_str() : "",
                e.textAttrs.styleName.c_str());
            break;
        }
        case DWG_TYPE_MTEXT: {
            const Dwg_Entity_MTEXT* M = ent->tio.MTEXT;
            e.type = EntType::MTEXT;
            e.pts = {Vec3{M->ins_pt.x, M->ins_pt.y, M->ins_pt.z}};
            e.textHeight = M->text_height;
            e.rotation = std::atan2((double)M->x_axis_dir.y, (double)M->x_axis_dir.x);
            e.mtextAttachment = static_cast<int>(M->attachment);
            e.mtextWidth = M->extents_width;
            e.mtextHeight = M->extents_height;
            // Layout rectangle (the width the text is formatted against),
            // line spacing and plane normal.
            e.textAttrs.mtextRectW = M->rect_width;
            e.textAttrs.mtextRectH = M->rect_height;
            e.textAttrs.lsStyle = static_cast<int>(M->linespace_style);
            e.textAttrs.lsFactor =
                (M->linespace_factor > 0.0) ? M->linespace_factor : 1.0;
            e.textAttrs.normal =
                Vec3{M->extrusion.x, M->extrusion.y, M->extrusion.z};
            // MTEXT vertical flag lives in flow_dir (DXF 72):
            //   1 = left-to-right (horizontal), 3 = top-to-bottom (vertical),
            //   5 = by style (inherited from the text style).
            // applyTextStyle resolves 5 from the STYLE; explicit 1/3 win over
            // the style, so the override must run afterwards.
            const int flowDir = static_cast<int>(M->flow_dir);
            if (M->text) {
                e.text = M->text;
            }
            const std::string textLog = decodeDwgString(e.text, codepage);
            SWDWG_LOG(
                "SWDWG: MTEXT '%s' style_ref=%s flow_dir=%d before applyTextStyle\n",
                textLog.c_str(),
                M->style ? "set" : "null",
                flowDir);
            // MTEXT carries no width factor of its own - the STYLE's applies.
            // TextAttrs defaults it to 1.0 and applyTextStyle only fills a
            // non-positive value, so it was never inherited and every MTEXT
            // came out at full width.  The styles of this bridge plan are
            // 0.65 / 0.70 / 0.75, which is exactly why our drawn labels were
            // ~35-50% wider than the extents the file stores (and why the
            // road-name labels did not match their stored boxes).  Clear it so
            // the STYLE value wins, then restore the fallback.
            e.textAttrs.widthFactor = 0.0;
            applyTextStyle(dwg, M->style, e.textAttrs, codepage, textStyles);
            if (e.textAttrs.widthFactor <= 0.0) {
                e.textAttrs.widthFactor = 1.0;  // STYLE without one
            }
            if (flowDir == 3) {
                e.textAttrs.isVertical = true;
            }
            else if (flowDir == 1) {
                e.textAttrs.isVertical = false;
            }
            else if (flowDir == 5) {
                // By-style flow: the STYLE decides (its DXF 70 bit 0x04, read
                // by applyTextStyle above).  The stored extents used to be
                // consulted here as a guess - "tall box means vertical" - but
                // that cache is unreliable (this drawing's stale extents even
                // disagree with the text width) and it turned every narrow-box
                // horizontal MTEXT into a "vertical" one.  Reported as the
                // MTEXT vertical parsing problem: handle 7c78, 230省道,
                // flow_dir=5, style 宋体 without the vertical bit, which is a
                // horizontal MTEXT wrapped one character per line by its
                // 9.4663 wide layout rectangle - now reproduced by wrapping
                // (see wrapMtextLines in the importer).
                SWDWG_LOG(
                    "SWDWG: MTEXT '%s' flow_dir=5 -> isVertical=%d (from STYLE "
                    "'%s', rect_w=%.4f extents=%.4fx%.4f, extents not used)\n",
                    textLog.c_str(),
                    e.textAttrs.isVertical ? 1 : 0,
                    e.textAttrs.styleName.c_str(),
                    M->rect_width, M->extents_width, M->extents_height);
            }
            SWDWG_LOG(
                "SWDWG: MTEXT '%s' after applyTextStyle isVertical=%d styleName='%s' "
                "rect=%.4fx%.4f extents=%.4fx%.4f\n",
                textLog.c_str(),
                e.textAttrs.isVertical ? 1 : 0,
                e.textAttrs.styleName.c_str(),
                M->rect_width, M->rect_height,
                M->extents_width, M->extents_height);
            if (M->text) {
                e.text = M->text;
            }
            break;
        }
        case DWG_TYPE_SPLINE: {
            const Dwg_Entity_SPLINE* S = ent->tio.SPLINE;
            e.type = EntType::SPLINE;
            e.degree = S->degree;
            e.splineClosed = (S->closed_b != 0) || ((S->splineflags & 4) != 0);
            if (S->ctrl_pts && S->num_ctrl_pts) {
                for (BITCODE_BL k = 0; k < S->num_ctrl_pts; ++k) {
                    e.pts.push_back(Vec3{S->ctrl_pts[k].x,
                                         S->ctrl_pts[k].y,
                                         S->ctrl_pts[k].z});
                }
            }
            if (S->fit_pts && S->num_fit_pts) {
                for (BITCODE_BS k = 0; k < S->num_fit_pts; ++k) {
                    e.fitPts.push_back(Vec3{S->fit_pts[k].x,
                                            S->fit_pts[k].y,
                                            S->fit_pts[k].z});
                }
            }
            if (e.pts.empty() && e.fitPts.empty()) {
                // SWDWG_LOG(
                //     "SWDWG: SPLINE entity has no usable points (ctrl=%u, fit=%u)\n",
                //     (unsigned)S->num_ctrl_pts,
                //     (unsigned)S->num_fit_pts);
                return false;
            }
            break;
        }
        case DWG_TYPE_LEADER: {
            const Dwg_Entity_LEADER* L = ent->tio.LEADER;
            e.type = EntType::LEADER;
            e.elevation = L->origin.z;
            e.extrusion = Vec3{L->extrusion.x, L->extrusion.y, L->extrusion.z};
            if (L->points && L->num_points) {
                e.pts.reserve(L->num_points);
                for (BITCODE_BL k = 0; k < L->num_points; ++k) {
                    e.pts.push_back(
                        Vec3{L->points[k].x, L->points[k].y, L->points[k].z});
                }
            }
            e.leaderArrow = (L->arrowhead_on != 0);
            e.leaderArrowSize = L->dimasz;
            e.leaderArrowType = static_cast<int>(L->arrowhead_type);
            e.leaderHook = (L->hookline_on != 0);
            e.leaderHookX = (L->hookline_dir != 0);
            e.leaderPathType = static_cast<int>(L->path_type);
            e.leaderBoxWidth = L->box_width;
            e.leaderBoxHeight = L->box_height;
            e.leaderInsptOffset =
                Vec3{L->inspt_offset.x, L->inspt_offset.y, L->inspt_offset.z};
            e.leaderEndProj =
                Vec3{L->endptproj.x, L->endptproj.y, L->endptproj.z};
            // The arrow length is DIMASZ, and this file version stores it only
            // in the referenced DIMSTYLE (the entity's own copy reads 0).  Keep
            // the name; the importer resolves it against the style table, where
            // DIMSCALE is applied the same way a dimension does it.
            if (L->dimstyle) {
                Dwg_Object* sobj =
                    dwg_ref_object(const_cast<Dwg_Data*>(dwg), L->dimstyle);
                if (sobj) {
                    int err = 0;
                    char* n = dwg_obj_table_get_name(sobj, &err);
                    if (n) {
                        e.dimStyleName = decodeDwgString(n, codepage);
                    }
                }
            }
            if (e.pts.size() < 2) {
                return false;
            }
            break;
        }
        case DWG_TYPE_SOLID: {
            const Dwg_Entity_SOLID* S = ent->tio.SOLID;
            e.type = EntType::SOLID;
            // A SOLID's outline runs corner1, corner2, corner4, corner3 - the
            // record stores the last two corners swapped with respect to the
            // outline (DXF group codes 12/13 are the third and fourth corners
            // of the record but the fourth and third of the drawn boundary).
            // Joining the record order produced bow-ties: the reported "part of
            // the rectangles come out wrong, the four corners cross" is handle
            // 5b51a on layer AUDIT_I_240320151244-75 of the bridge plan, whose
            // stored order self-crosses.  Measured over that file: 286 SOLIDs,
            // 237 of them triangle-form (corner4 == corner3, identical either
            // way) and 49 true quads, of which 42 self-cross in record order
            // and 0 self-cross in outline order.  Emitting outline order keeps
            // the wire/face valid and matches what AutoCAD draws.
            e.pts = {
                Vec3{S->corner1.x, S->corner1.y, S->elevation},
                Vec3{S->corner2.x, S->corner2.y, S->elevation},
                Vec3{S->corner4.x, S->corner4.y, S->elevation},
                Vec3{S->corner3.x, S->corner3.y, S->elevation},
            };
            e.extrusion = Vec3{S->extrusion.x, S->extrusion.y, S->extrusion.z};
            break;
        }
        case DWG_TYPE__3DFACE: {
            const Dwg_Entity__3DFACE* F = ent->tio._3DFACE;
            e.type = EntType::FACE3D;
            e.pts = {
                Vec3{F->corner1.x, F->corner1.y, F->corner1.z},
                Vec3{F->corner2.x, F->corner2.y, F->corner2.z},
                Vec3{F->corner3.x, F->corner3.y, F->corner3.z},
                Vec3{F->corner4.x, F->corner4.y, F->corner4.z},
            };
            break;
        }
        case DWG_TYPE_HATCH: {
            const Dwg_Entity_HATCH* H = ent->tio.HATCH;
            e.type = EntType::HATCH;
            e.elevation = H->elevation;
            e.extrusion = Vec3{H->extrusion.x, H->extrusion.y, H->extrusion.z};
            e.hatchSolid = (H->is_solid_fill != 0);
            e.hatchPatternType = static_cast<int>(H->pattern_type);
            // Gradient fill ("渐变色"): is_solid_fill is 0 and there are no
            // definition lines, so the colour ramp is the only thing that says
            // this hatch is meant to be a fill at all.
            e.hatchGradient = (H->is_gradient_fill != 0);
            e.hatchGradientColors = static_cast<int>(H->num_colors);
            if (H->gradient_name) {
                e.hatchGradientName = decodeDwgString(H->gradient_name, codepage);
            }
            if (H->is_gradient_fill && H->colors && H->num_colors > 0) {
                e.hatchGradientAci = static_cast<int>(H->colors[0].color.index);
                e.hatchGradientRgb =
                    static_cast<unsigned long>(H->colors[0].color.rgb);
            }
            e.hatchAngle = H->angle;
            if (H->scale_spacing > 0.0) {
                e.hatchScale = H->scale_spacing;
            }
            if (H->name) {
                e.hatchPattern = decodeDwgString(H->name, codepage);
            }
            // Boundary loops.  A path is either a polyline path (points +
            // bulges) or an edge (segment) path.
            if (H->paths && H->num_paths) {
                for (BITCODE_BL i = 0; i < H->num_paths; ++i) {
                    const Dwg_HATCH_Path& p = H->paths[i];
                    HatchPath hp;
                    hp.closed = (p.flag & 0x20) == 0;  // 0x20 = is_open
                    // DWG convention: the first boundary path is the outer
                    // loop; any remaining paths are inner holes.  LibreDWG's
                    // path flag does not expose a reliable "outer" bit, so we
                    // rely on path ordering (matches AutoCAD behaviour).
                    hp.isOuter = (i == 0);
                    if ((p.flag & 0x2) != 0 && p.polyline_paths
                        && p.num_segs_or_paths) {
                        // Polyline path: vertices with optional bulges.
                        hp.isPolyline = true;
                        for (BITCODE_BL k = 0; k < p.num_segs_or_paths; ++k) {
                            const Dwg_HATCH_PolylinePath& v =
                                p.polyline_paths[k];
                            hp.polyPts.push_back(
                                Vec3{v.point.x, v.point.y, 0.0});
                            hp.polyBulges.push_back(v.bulge);
                        }
                    }
                    else if (p.segs && p.num_segs_or_paths) {
                        for (BITCODE_BL k = 0; k < p.num_segs_or_paths; ++k) {
                            const Dwg_HATCH_PathSeg& s = p.segs[k];
                            HatchSeg hs;
                            hs.curveType = s.curve_type;
                            switch (s.curve_type) {
                                case 1:  // LINE
                                    hs.p1 = Vec3{s.first_endpoint.x,
                                                 s.first_endpoint.y, 0.0};
                                    hs.p2 = Vec3{s.second_endpoint.x,
                                                 s.second_endpoint.y, 0.0};
                                    break;
                                case 2:  // CIRCULAR ARC
                                    hs.center = Vec3{s.center.x, s.center.y,
                                                     0.0};
                                    hs.radius = s.radius;
                                    hs.startAngle = s.start_angle;
                                    hs.endAngle = s.end_angle;
                                    hs.ccw = (s.is_ccw != 0);
                                    break;
                                case 3:  // ELLIPTICAL ARC
                                    hs.center = Vec3{s.center.x, s.center.y,
                                                     0.0};
                                    hs.endpoint = Vec3{s.endpoint.x,
                                                       s.endpoint.y, 0.0};
                                    hs.ratio = s.minor_major_ratio;
                                    hs.startAngle = s.start_angle;
                                    hs.endAngle = s.end_angle;
                                    hs.ccw = (s.is_ccw != 0);
                                    break;
                                case 4:  // SPLINE
                                    if (s.control_points
                                        && s.num_control_points) {
                                        for (BITCODE_BL c = 0;
                                             c < s.num_control_points; ++c) {
                                            hs.ctrlPts.push_back(Vec3{
                                                s.control_points[c].point.x,
                                                s.control_points[c].point.y,
                                                0.0});
                                        }
                                    }
                                    if (s.fitpts && s.num_fitpts) {
                                        for (BITCODE_BL c = 0;
                                             c < s.num_fitpts; ++c) {
                                            hs.fitPts.push_back(Vec3{
                                                s.fitpts[c].x, s.fitpts[c].y,
                                                0.0});
                                        }
                                    }
                                    break;
                                default:
                                    continue;  // unknown edge type
                            }
                            hp.segs.push_back(std::move(hs));
                        }
                    }
                    if (!hp.segs.empty()
                        || (!hp.polyPts.empty() && hp.isPolyline)) {
                        e.hatchPaths.push_back(std::move(hp));
                    }
                }
            }
            // Pattern definition line families.
            if (H->deflines && H->num_deflines) {
                for (BITCODE_BS i = 0; i < H->num_deflines; ++i) {
                    const Dwg_HATCH_DefLine& d = H->deflines[i];
                    HatchDefLine dl;
                    dl.angle = d.angle;
                    dl.pt0 = Vec3{d.pt0.x, d.pt0.y, 0.0};
                    dl.offset = Vec3{d.offset.x, d.offset.y, 0.0};
                    if (d.dashes && d.num_dashes) {
                        for (BITCODE_BS k = 0; k < d.num_dashes; ++k) {
                            dl.dashes.push_back(d.dashes[k]);
                        }
                    }
                    e.hatchDefLines.push_back(std::move(dl));
                }
            }
            {
                std::ostringstream oss;
                oss << "[SWDWG HATCH] name=" << e.hatchPattern
                    << " solid=" << e.hatchSolid
                    << " type=" << (int)H->pattern_type
                    << " deflines=" << e.hatchDefLines.size()
                    << " paths=" << e.hatchPaths.size()
                    << " scale=" << e.hatchScale
                    << " angle=" << e.hatchAngle;
                for (size_t i = 0; i < e.hatchDefLines.size(); ++i) {
                    const HatchDefLine& dl = e.hatchDefLines[i];
                    oss << "\n  defline[" << i << "] angle=" << dl.angle
                        << " pt0=(" << dl.pt0.x << "," << dl.pt0.y << ")"
                        << " off=(" << dl.offset.x << "," << dl.offset.y
                        << ") dashes=[";
                    for (double dash : dl.dashes) {
                        oss << dash << " ";
                    }
                    oss << "]";
                }
                SWDWG_LOG("%s\n", oss.str().c_str());
            }
            if (e.hatchPaths.empty()) {
                return false;  // nothing to draw
            }
            break;
        }
        case DWG_TYPE_DIMENSION_LINEAR:
        case DWG_TYPE_DIMENSION_ALIGNED:
        case DWG_TYPE_DIMENSION_ANG2LN:
        case DWG_TYPE_DIMENSION_ANG3PT:
        case DWG_TYPE_DIMENSION_DIAMETER:
        case DWG_TYPE_DIMENSION_RADIUS:
        case DWG_TYPE_DIMENSION_ORDINATE: {
            // DIMENSION entities become dedicated display objects (like TEXT
            // / MTEXT); the LibreDWG -> DimensionData mapping lives in
            // DWGDimension.cpp.
            if (!extractDimension(dwg, obj, e.dim, codepage)) {
                return false;
            }
            e.type = EntType::DIMENSION;
            break;
        }
        default:
            return false;  // unsupported geometry type
    }

    // Common entity attributes: layer and color.
    std::string layer = "0";
    if (ent->layer) {
        int err = 0;
        char* n = dwg_obj_table_get_name(ent->layer->obj, &err);
        if (n) {
            layer = decodeDwgString(n, codepage);
        }
    }
    e.layer = layer;
    if (ent->color.method == DWG_COLOR_METHOD_TRUECOLOR) {
        // Some DWG writers store an ACI index with method=TRUECOLOR and the
        // index value in the rgb field.  Treat 1..255 as ACI, not RGB.
        unsigned long rgb = ent->color.rgb & 0xFFFFFFUL;
        if (rgb > 0UL && rgb <= 255UL) {
            e.colorIndex = static_cast<int>(rgb);
            e.rgb = 0;
        }
        else {
            e.colorIndex = 0;
            e.rgb = rgb;
        }
    }
    else if (ent->color.method == DWG_COLOR_METHOD_ACI) {
        e.colorIndex = ent->color.index;
        e.rgb = 0;
    }
    else if (ent->color.rgb != 0) {
        e.colorIndex = 0;
        e.rgb = ent->color.rgb & 0xFFFFFFUL;
    }
    else if (ent->color.index > 0 && ent->color.index < 256) {
        e.colorIndex = ent->color.index;
        e.rgb = 0;
    }
    else {
        e.colorIndex = 256;  // ByLayer
        e.rgb = 0;
    }
    e.lineWeight = ent->linewt;
    e.ltypeScale = (ent->ltype_scale > 0.0) ? ent->ltype_scale : 1.0;
    if (ent->ltype) {
        int err = 0;
        // The linetype field is a reference: resolve it against the object map
        // first, exactly like the LAYER table's own linetype field.  Reading
        // ent->ltype->obj without that left the name empty and every explicitly
        // assigned linetype fell back to ByLayer - measured on the bridge plan,
        // the yellow block content on layer 标注 that carries DASHED explicitly
        // came out as the layer's Continuous.
        Dwg_Object* ltObj =
            dwg_ref_object(const_cast<Dwg_Data*>(dwg), ent->ltype);
        char* n = ltObj ? dwg_obj_table_get_name(ltObj, &err) : nullptr;
        if (n) {
            e.ltype = decodeDwgString(n, codepage);
        }
    }
    return true;
}

}  // namespace SWDWG
