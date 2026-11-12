#include "DWGReader.h"
#include "DWGTypes.h"
#include "PathUtil.h"

#include <cstring>
#include <map>

// --- LibreDWG config alignment -------------------------------------------
// The libredwg public headers depend on a few config macros that must match
// the build of libredwg.lib (see build-vs2022/src/config.h). We set the ones
// that affect the public struct layout / text types here so that our ABI
// matches the library exactly.
#ifndef HAVE_WCHAR_H
#  define HAVE_WCHAR_H 1
#endif
#ifndef SIZEOF_WCHAR_T
#  define SIZEOF_WCHAR_T 2
#endif
// --------------------------------------------------------------------------

// libredwg dynamic/static entity accessors are declared in dwg_api.h
extern "C" {
#include "dwg_api.h"
}

using namespace SWDWG;

namespace
{
// Convert a libredwg point to a Python list [x, y, z]
inline Py::List pt3(double x, double y, double z)
{
    Py::List out(3);
    out[0] = Py::Float(x);
    out[1] = Py::Float(y);
    out[2] = Py::Float(z);
    return out;
}
} // namespace

Py::String DWGReader::str(const char* s)
{
    if (!s) {
        return Py::String("");
    }
    // Prefer strict UTF-8 (DWG R2007+ store text as UTF-8).
    PyObject* uni = PyUnicode_DecodeUTF8(s, (Py_ssize_t)std::strlen(s), nullptr);
    if (!uni) {
        PyErr_Clear();
        // Fall back to Latin-1 so every byte maps to one code point and no
        // data is ever lost (legacy single-byte code pages).
        uni = PyUnicode_DecodeLatin1(s, (Py_ssize_t)std::strlen(s), nullptr);
    }
    return Py::String(uni, /*owned=*/true);
}

DWGReader::DWGReader()
    : m_dwg(nullptr)
    , m_result()
{}

DWGReader::~DWGReader()
{
    if (m_dwg) {
        dwg_free(m_dwg);
        m_dwg = nullptr;
    }
}

bool DWGReader::read(const std::string& filename, std::string& error)
{
    m_dwg = new Dwg_Data;
    std::memset(m_dwg, 0, sizeof(Dwg_Data));

    int err = dwg_read_file(SWDWG::toLocalPath(filename).c_str(), m_dwg);
    // dwg_read_file returns 0 on success (DWG_CRC_OK) or a warning/error code.
    // If nothing could be read, treat it as a hard failure.
    if (m_dwg->num_objects == 0) {
        error = "dwg_read_file failed with error code " + std::to_string(err)
            + " (no objects could be read from the file)";
        dwg_free(m_dwg);
        m_dwg = nullptr;
        return false;
    }

    m_objectCount = (size_t)m_dwg->num_objects;

    // Build the result dictionary
    m_result["filename"] = Py::String(filename);
    m_result["version"] = Py::String(dwg_version_type(m_dwg->header.version));

    readHeader();
    readLayers();
    readStyles();
    readDimstyles();
    readLinetypes();
    readBlocks();
    buildEntityBlockMap();
    readEntities();
    readInventory();

    return true;
}

void DWGReader::readHeader()
{
    if (!m_dwg)
        return;
    auto& hv = m_dwg->header_vars;

    m_result["num_objects"] = Py::Long((long)m_dwg->num_objects);

    // Drawing units.  Nothing used to read these, so a drawing that declares
    // metres or inches was treated as if it were millimetres.  The table lives
    // in DWGTypes.h so the App importer and the Gui agree on one definition.
    const int insunits = static_cast<int>(hv.INSUNITS);
    const int measurement = static_cast<int>(hv.MEASUREMENT);
    const int units = effectiveDrawingUnit(insunits);
    m_result["insunits"] = Py::Long(insunits);
    m_result["measurement"] = Py::Long(measurement);
    m_result["lunits"] = Py::Long(static_cast<long>(hv.LUNITS));
    m_result["luprec"] = Py::Long(static_cast<long>(hv.LUPREC));
    m_result["aunits"] = Py::Long(static_cast<long>(hv.AUNITS));
    m_result["auprec"] = Py::Long(static_cast<long>(hv.AUPREC));
    m_result["dimscale"] = Py::Float(hv.DIMSCALE);
    m_result["units"] = str(drawingUnitName(units));
    m_result["units_mm"] = Py::Float(drawingUnitToMm(units));
    // Which value the importer actually scales by: 1.0 for a drawing that
    // declares millimetres, 1000.0 (metres) when $INSUNITS is absent and the
    // civil/road metres default applies, or whatever $INSUNITS names (see
    // effectiveDrawingUnit() and drawingUnitToMm()).
    m_result["unit_scale"] = Py::Float(drawingUnitToMm(units));
}

void DWGReader::readLayers()
{
    Py::List layers;
    if (!m_dwg)
        return;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_OBJECT)
            continue;
        if (obj->fixedtype != DWG_TYPE_LAYER)
            continue;

        Dwg_Object_LAYER* lay = obj->tio.object->tio.LAYER;
        if (!lay || !lay->name)
            continue;

        Py::Dict d;
        d["name"] = str(lay->name);

        // color: Dwg_Color value stored in the table struct
        int color_index = lay->color.index;
        unsigned long rgb = lay->color.rgb;
        d["color"] = Py::Long(color_index);
        d["rgb"] = Py::Long((long)rgb);

        d["off"] = Py::Boolean(lay->off);
        d["frozen"] = Py::Boolean(lay->frozen);
        d["locked"] = Py::Boolean(lay->locked);

        // linetype name
        std::string lt = "Continuous";
        if (lay->ltype) {
            int e = 0;
            char* n = dwg_obj_table_get_name(lay->ltype->obj, &e);
            if (n)
                lt = n;
        }
        d["linetype"] = str(lt.c_str());

        layers.append(d);
    }

    m_result["layers"] = layers;
}

void DWGReader::readStyles()
{
    Py::List styles;
    if (!m_dwg)
        return;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_OBJECT)
            continue;
        if (obj->fixedtype != DWG_TYPE_STYLE)
            continue;

        Dwg_Object_STYLE* st = obj->tio.object->tio.STYLE;
        if (!st || !st->name)
            continue;

        Py::Dict d;
        d["name"] = str(st->name);
        d["height"] = Py::Float(st->text_size);
        d["width_factor"] = Py::Float(st->width_factor);
        d["oblique"] = Py::Float(st->oblique_angle);
        d["font"] = str(st->font_file);
        d["bigfont"] = str(st->bigfont_file);
        d["flag"] = Py::Long((long)st->flag);
        // DXF STYLE group 70: bit 0x04 = vertical text, bit 0x01 = "this entry
        // is a shape" - so 0x04 is the vertical flag, not 0x01 (applyTextStyle
        // used to test 0x01 and therefore never saw a vertical style).
        d["is_vertical"] = Py::Boolean(st->is_vertical != 0
                                       || (st->flag & 0x04) != 0);

        styles.append(d);
    }

    m_result["styles"] = styles;
}

void DWGReader::readDimstyles()
{
    Py::List dimstyles;
    if (!m_dwg)
        return;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_OBJECT)
            continue;
        if (obj->fixedtype != DWG_TYPE_DIMSTYLE)
            continue;

        Dwg_Object_DIMSTYLE* ds = obj->tio.object->tio.DIMSTYLE;
        if (!ds || !ds->name)
            continue;

        Py::Dict d;
        d["name"] = str(ds->name);
        dimstyles.append(d);
    }

    m_result["dimstyles"] = dimstyles;
}

void DWGReader::readLinetypes()
{
    Py::List linetypes;
    if (!m_dwg)
        return;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_OBJECT)
            continue;
        if (obj->fixedtype != DWG_TYPE_LTYPE)
            continue;

        Dwg_Object_LTYPE* lt = obj->tio.object->tio.LTYPE;
        if (!lt || !lt->name)
            continue;

        Py::Dict d;
        d["name"] = str(lt->name);
        d["description"] = str(lt->description);
        d["pattern_len"] = Py::Float(lt->pattern_len);
        d["alignment"] = Py::Long((long)lt->alignment);
        d["numdashes"] = Py::Long((long)lt->numdashes);

        // One dict per dash record.  A plain entry only has a length; a
        // complex one also carries the shape/text flag, the shape code, the
        // .shx it lives in (through its STYLE), S (scale), the rotation and the
        // X/Y offsets.  Exported because the serialized pattern alone is
        // ambiguous: "s0:149@AAA.shx#0.15;0;0;0;0.8,-1.6" cannot be told apart
        // from a shape element of length 0 plus a 0.8 dash without the record -
        // and the two need different fixes.
        Py::List dashes;
        for (int k = 0; k < lt->numdashes && lt->dashes; k++) {
            const Dwg_LTYPE_dash& sd = lt->dashes[k];
            Py::Dict dd;
            dd["length"] = Py::Float(sd.length);
            dd["shape_flag"] = Py::Long((long)sd.shape_flag);
            dd["code"] = Py::Long((long)sd.complex_shapecode);
            dd["text"] = sd.text ? str(sd.text) : str("");
            dd["scale"] = Py::Float(sd.scale);
            dd["rotation"] = Py::Float(sd.rotation);
            dd["x_offset"] = Py::Float(sd.x_offset);
            dd["y_offset"] = Py::Float(sd.y_offset);
            dd["shapefile"] = str("");
            if (sd.style) {
                Dwg_Object* styleObj = dwg_ref_object(m_dwg, sd.style);
                if (styleObj && styleObj->supertype == DWG_SUPERTYPE_OBJECT
                    && styleObj->fixedtype == DWG_TYPE_STYLE) {
                    const Dwg_Object_STYLE* st =
                        styleObj->tio.object->tio.STYLE;
                    if (st && st->font_file) {
                        dd["shapefile"] = str(st->font_file);
                    }
                }
            }
            dashes.append(dd);
        }
        d["dashes"] = dashes;

        linetypes.append(d);
    }

    m_result["linetypes"] = linetypes;
}

void DWGReader::readBlocks()
{
    Py::List blocks;
    if (!m_dwg)
        return;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_OBJECT)
            continue;
        if (obj->fixedtype != DWG_TYPE_BLOCK_HEADER)
            continue;

        Dwg_Object_BLOCK_HEADER* bh = obj->tio.object->tio.BLOCK_HEADER;
        if (!bh)
            continue;

        int e = 0;
        char* name = dwg_obj_table_get_name(obj, &e);
        if (!name)
            continue;

        Py::Dict d;
        d["name"] = str(name);
        d["base_point"] = pt3(bh->base_pt.x, bh->base_pt.y, bh->base_pt.z);
        d["anonymous"] = Py::Boolean(bh->anonymous);
        d["num_owned"] = Py::Long((long)bh->num_owned);

        blocks.append(d);
    }

    m_result["blocks"] = blocks;
}

void DWGReader::buildEntityBlockMap()
{
    m_entityBlock.clear();
    if (!m_dwg)
        return;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_OBJECT)
            continue;
        if (obj->fixedtype != DWG_TYPE_BLOCK_HEADER)
            continue;

        int e = 0;
        char* name = dwg_obj_table_get_name(obj, &e);
        if (!name)
            continue;
        std::string bname = name;

        // Walk all entities owned by this block header.
        const Dwg_Object* ent = get_first_owned_entity(obj);
        while (ent) {
            if (ent->supertype == DWG_SUPERTYPE_ENTITY) {
                m_entityBlock[ent] = bname;
            }
            ent = get_next_owned_entity(obj, ent);
        }
    }

    // Entities not owned by any block belong to the model space.
    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_ENTITY)
            continue;
        if (m_entityBlock.find(obj) == m_entityBlock.end()) {
            m_entityBlock[obj] = "*Model_Space";
        }
    }
}

// ---------------------------------------------------------------------------
//  Entity extraction
// ---------------------------------------------------------------------------

Py::Dict DWGReader::entityCommon(const Dwg_Object* obj)
{
    Py::Dict d;
    const Dwg_Object_Entity* ent = obj->tio.entity;

    // layer
    std::string layer = "0";
    if (ent->layer) {
        int e = 0;
        char* n = dwg_obj_table_get_name(ent->layer->obj, &e);
        if (n)
            layer = n;
    }
    d["layer"] = str(layer.c_str());

    // color (Dwg_Color value, not a pointer)
    int color_index = ent->color.index;
    unsigned long rgb = ent->color.rgb;
    d["color"] = Py::Long(color_index);
    d["rgb"] = Py::Long((long)rgb);

    // linetype
    std::string ltype = "ByLayer";
    if (ent->ltype) {
        int e = 0;
        char* n = dwg_obj_table_get_name(ent->ltype->obj, &e);
        if (n)
            ltype = n;
    }
    d["linetype"] = str(ltype.c_str());

    return d;
}

Py::Object DWGReader::entityGeometry(const Dwg_Object* obj)
{
    const Dwg_Object_Entity* ent = obj->tio.entity;
    Py::Dict g;

    switch (obj->fixedtype) {
        case DWG_TYPE_LINE: {
            const Dwg_Entity_LINE* L = ent->tio.LINE;
            g["start"] = pt3(L->start.x, L->start.y, L->start.z);
            g["end"] = pt3(L->end.x, L->end.y, L->end.z);
            break;
        }
        case DWG_TYPE_CIRCLE: {
            const Dwg_Entity_CIRCLE* C = ent->tio.CIRCLE;
            g["center"] = pt3(C->center.x, C->center.y, C->center.z);
            g["radius"] = Py::Float(C->radius);
            break;
        }
        case DWG_TYPE_ARC: {
            const Dwg_Entity_ARC* A = ent->tio.ARC;
            g["center"] = pt3(A->center.x, A->center.y, A->center.z);
            g["radius"] = Py::Float(A->radius);
            g["start_angle"] = Py::Float(A->start_angle);
            g["end_angle"] = Py::Float(A->end_angle);
            break;
        }
        case DWG_TYPE_ELLIPSE: {
            const Dwg_Entity_ELLIPSE* E = ent->tio.ELLIPSE;
            g["center"] = pt3(E->center.x, E->center.y, E->center.z);
            g["sm_axis"] = pt3(E->sm_axis.x, E->sm_axis.y, E->sm_axis.z);
            g["axis_ratio"] = Py::Float(E->axis_ratio);
            g["start_angle"] = Py::Float(E->start_angle);
            g["end_angle"] = Py::Float(E->end_angle);
            break;
        }
        case DWG_TYPE_POINT: {
            const Dwg_Entity_POINT* P = ent->tio.POINT;
            g["point"] = pt3(P->x, P->y, P->z);
            break;
        }
        case DWG_TYPE_INSERT: {
            const Dwg_Entity_INSERT* I = ent->tio.INSERT;
            g["ins_pt"] = pt3(I->ins_pt.x, I->ins_pt.y, I->ins_pt.z);
            g["scale"] = pt3(I->scale.x, I->scale.y, I->scale.z);
            g["rotation"] = Py::Float(I->rotation);

            // block name
            std::string bname = "";
            if (I->block_header) {
                Dwg_Object* bobj = dwg_ref_object(m_dwg, I->block_header);
                if (bobj) {
                    int e = 0;
                    char* n = dwg_obj_table_get_name(bobj, &e);
                    if (n)
                        bname = n;
                }
            }
            g["block_name"] = str(bname.c_str());
            break;
        }
        case DWG_TYPE_LWPOLYLINE: {
            const Dwg_Entity_LWPOLYLINE* W = ent->tio.LWPOLYLINE;
            Py::List pts;
            for (unsigned long k = 0; k < W->num_points; k++) {
                pts.append(pt3(W->points[k].x, W->points[k].y, 0.0));
            }
            g["points"] = pts;

            Py::List bulges;
            for (unsigned long k = 0; k < W->num_bulges; k++) {
                bulges.append(Py::Float(W->bulges[k]));
            }
            g["bulges"] = bulges;

            g["closed"] = Py::Boolean(W->flag & 0x200);
            g["elevation"] = Py::Float(W->elevation);
            break;
        }
        case DWG_TYPE_TEXT: {
            const Dwg_Entity_TEXT* T = ent->tio.TEXT;
            g["ins_pt"] = pt3(T->ins_pt.x, T->ins_pt.y,
                              T->dataflags & 0x1 ? T->elevation : 0.0);
            g["alignment_pt"] = pt3(T->alignment_pt.x, T->alignment_pt.y, 0.0);
            g["height"] = Py::Float(T->height);
            g["rotation"] = Py::Float(T->rotation);
            g["width_factor"] = Py::Float(T->width_factor);
            g["oblique"] = Py::Float(T->oblique_angle);
            g["text"] = str(T->text_value);
            g["horiz_alignment"] = Py::Long(T->horiz_alignment);
            g["vert_alignment"] = Py::Long(T->vert_alignment);
            g["generation"] = Py::Long(T->generation);
            break;
        }
        case DWG_TYPE_MTEXT: {
            const Dwg_Entity_MTEXT* M = ent->tio.MTEXT;
            g["ins_pt"] = pt3(M->ins_pt.x, M->ins_pt.y, M->ins_pt.z);
            g["text_height"] = Py::Float(M->text_height);
            g["rect_width"] = Py::Float(M->rect_width);
            // Rendered extents box AutoCAD itself stored for this MTEXT: the
            // ground truth for attachment-point placement (the box is centred
            // on ins_pt for attachment 5, right/bottom aligned for 3/9, ...),
            // so a probe can check the layout the importer derives from the
            // font instead of trusting it.
            g["extents_width"] = Py::Float(M->extents_width);
            g["extents_height"] = Py::Float(M->extents_height);
            g["linespace_factor"] = Py::Float(M->linespace_factor);
            g["linespace_style"] = Py::Long(M->linespace_style);
            g["attachment"] = Py::Long(M->attachment);
            g["flow_dir"] = Py::Long(M->flow_dir);
            // DXF 72: 1 = left-to-right, 3 = top-to-bottom, 5 = by style.
            // "By style" means the referenced STYLE's vertical bit (DXF 70
            // 0x04) decides - not the stored extents, whose "tall box means
            // vertical" guess mis-read the narrow-box wrapped road labels of
            // this drawing as vertical.  Both the raw style bit and the
            // resolved answer are exported so a probe can check the rule.
            bool styleVertical = false;
            if (M->style) {
                Dwg_Object* so = dwg_ref_object(m_dwg, M->style);
                if (so && so->supertype == DWG_SUPERTYPE_OBJECT
                    && so->fixedtype == DWG_TYPE_STYLE) {
                    const Dwg_Object_STYLE* st = so->tio.object->tio.STYLE;
                    if (st) {
                        styleVertical = (st->is_vertical != 0)
                            || ((st->flag & 0x04) != 0);
                    }
                }
            }
            g["style_is_vertical"] = Py::Boolean(styleVertical);
            g["is_vertical"] = Py::Boolean(
                M->flow_dir == 3
                || (M->flow_dir == 5 && styleVertical));
            g["rotation"] = Py::Float(
                std::atan2((double)M->x_axis_dir.y, (double)M->x_axis_dir.x));
            g["text"] = str(M->text);
            break;
        }
        case DWG_TYPE_SPLINE: {
            const Dwg_Entity_SPLINE* S = ent->tio.SPLINE;
            g["degree"] = Py::Long(S->degree);
            g["closed"] = Py::Boolean(S->closed_b);
            g["rational"] = Py::Boolean(S->rational);

            Py::List knots;
            for (unsigned long k = 0; k < S->num_knots; k++) {
                knots.append(Py::Float(S->knots[k]));
            }
            g["knots"] = knots;

            Py::List ctrl;
            for (unsigned long k = 0; k < S->num_ctrl_pts; k++) {
                ctrl.append(pt3(S->ctrl_pts[k].x, S->ctrl_pts[k].y, S->ctrl_pts[k].z));
            }
            g["control_points"] = ctrl;

            Py::List fit;
            for (int k = 0; k < S->num_fit_pts; k++) {
                fit.append(pt3(S->fit_pts[k].x, S->fit_pts[k].y, S->fit_pts[k].z));
            }
            g["fit_points"] = fit;
            break;
        }
        case DWG_TYPE_SOLID: {
            const Dwg_Entity_SOLID* S = ent->tio.SOLID;
            Py::List corners;
            corners.append(pt3(S->corner1.x, S->corner1.y, S->elevation));
            corners.append(pt3(S->corner2.x, S->corner2.y, S->elevation));
            corners.append(pt3(S->corner3.x, S->corner3.y, S->elevation));
            corners.append(pt3(S->corner4.x, S->corner4.y, S->elevation));
            g["corners"] = corners;
            break;
        }
        case DWG_TYPE__3DFACE: {
            const Dwg_Entity__3DFACE* F = ent->tio._3DFACE;
            Py::List corners;
            corners.append(pt3(F->corner1.x, F->corner1.y, F->corner1.z));
            corners.append(pt3(F->corner2.x, F->corner2.y, F->corner2.z));
            corners.append(pt3(F->corner3.x, F->corner3.y, F->corner3.z));
            corners.append(pt3(F->corner4.x, F->corner4.y, F->corner4.z));
            g["corners"] = corners;
            g["invisible_flags"] = Py::Long(F->invis_flags);
            break;
        }
        default:
            return Py::None();
    }

    return g;
}

void DWGReader::readEntities()
{
    Py::List entities;
    if (!m_dwg)
        return;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_ENTITY)
            continue;
        if (!obj->tio.entity)
            continue;

        // Only process real geometry entities (skip VERTEX_* which belong to
        // polyline chains, and SEQEND/BLOCK/ENDBLK markers).
        switch (obj->fixedtype) {
            case DWG_TYPE_VERTEX_2D:
            case DWG_TYPE_VERTEX_3D:
            case DWG_TYPE_VERTEX_MESH:
            case DWG_TYPE_VERTEX_PFACE:
            case DWG_TYPE_VERTEX_PFACE_FACE:
            case DWG_TYPE_SEQEND:
            case DWG_TYPE_BLOCK:
            case DWG_TYPE_ENDBLK:
                continue;
            default:
                break;
        }

        Py::Dict d = entityCommon(obj);
        d["type"] = str(obj->name ? obj->name : "UNKNOWN");

        // owning block (or model space)
        auto it = m_entityBlock.find(obj);
        std::string blk = it != m_entityBlock.end() ? it->second : "*Model_Space";
        d["block"] = str(blk.c_str());

        Py::Object geom = entityGeometry(obj);
        if (geom.isNone())
            continue; // unsupported geometry type, skip
        d["geometry"] = geom;
        entities.append(d);
    }

    m_result["entities"] = entities;
}

void DWGReader::readInventory()
{
    if (!m_dwg)
        return;

    std::map<std::string, std::map<std::string, long>> byLayer;
    std::map<std::string, long> byType;

    for (unsigned long i = 0; i < m_dwg->num_objects; i++) {
        const Dwg_Object* obj = &m_dwg->object[i];
        if (obj->supertype != DWG_SUPERTYPE_ENTITY)
            continue;
        // Same exclusions as readEntities(): polyline sub-entities and the
        // block delimiters are not standalone content.
        switch (obj->fixedtype) {
            case DWG_TYPE_VERTEX_2D:
            case DWG_TYPE_VERTEX_3D:
            case DWG_TYPE_VERTEX_MESH:
            case DWG_TYPE_VERTEX_PFACE:
            case DWG_TYPE_VERTEX_PFACE_FACE:
            case DWG_TYPE_SEQEND:
            case DWG_TYPE_BLOCK:
            case DWG_TYPE_ENDBLK:
                continue;
            default:
                break;
        }

        const std::string type = obj->name ? obj->name : "UNKNOWN";
        std::string layer = "0";
        if (obj->tio.entity && obj->tio.entity->layer) {
            int e = 0;
            char* n = dwg_obj_table_get_name(obj->tio.entity->layer->obj, &e);
            if (n) {
                layer = n;
            }
        }
        byLayer[layer][type] += 1;
        byType[type] += 1;
    }

    Py::Dict inv;
    for (const auto& lk : byLayer) {
        Py::Dict types;
        for (const auto& tk : lk.second) {
            types[str(tk.first.c_str())] = Py::Long(tk.second);
        }
        inv[str(lk.first.c_str())] = types;
    }
    Py::Dict totals;
    for (const auto& tk : byType) {
        totals[str(tk.first.c_str())] = Py::Long(tk.second);
    }
    m_result["inventory"] = inv;
    m_result["inventory_totals"] = totals;
}
