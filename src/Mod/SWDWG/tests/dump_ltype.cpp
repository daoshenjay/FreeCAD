// Dump every LTYPE in a DWG with its dash element fields.  Especially useful
// to see what libredwg decodes for complex (text/shape) dashes: x_offset,
// y_offset, length, scale, rotation, complex_shapecode / text.
#include <dwg.h>
#include <dwg_api.h>
#include <stdio.h>
#include <string.h>

static const char* shapeFlagStr(unsigned f)
{
    if (f & 4) return "shape";
    if (f & 2) return "text";
    return "plain";
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.dwg [name]\n", argv[0]);
        return 1;
    }
    Dwg_Data dwg;
    int err = dwg_read_file(argv[1], &dwg);
    fprintf(stderr, "dwg_read_file err=%d num_objects=%u\n", err,
            (unsigned)dwg.num_objects);
    if (!dwg.num_objects) {
        return 1;
    }
    const char* only = argc >= 3 ? argv[2] : NULL;
    for (unsigned i = 0; i < dwg.num_objects; ++i) {
        Dwg_Object* obj = &dwg.object[i];
        if (obj->supertype != DWG_SUPERTYPE_OBJECT
            || obj->fixedtype != DWG_TYPE_LTYPE) continue;
        Dwg_Object_LTYPE* lt = obj->tio.object->tio.LTYPE;
        if (!lt || !lt->name) continue;
        const char* nm = lt->name;
        if (only && strstr(nm, only) == NULL) continue;
        printf("\nLTYPE '%s' desc='%s' pattern_len=%.6f numdashes=%d\n",
               nm, lt->description ? lt->description : "",
               lt->pattern_len, lt->numdashes);
        for (int k = 0; k < lt->numdashes; ++k) {
            const Dwg_LTYPE_dash& d = lt->dashes[k];
            printf("  dash[%d] length=%.6f shape_flag=0x%x(%s) "
                   "shapecode=%d text='%s' scale=%.6f rot=%.6f "
                   "x_off=%.6f y_off=%.6f style=%p\n",
                   k, d.length, d.shape_flag, shapeFlagStr(d.shape_flag),
                   d.complex_shapecode,
                   d.text ? d.text : "",
                   d.scale, d.rotation,
                   d.x_offset, d.y_offset, (void*)d.style);
        }
    }
    dwg_free(&dwg);
    return 0;
}
