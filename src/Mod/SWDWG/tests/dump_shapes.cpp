#include "../App/DWGShapeFile.h"
#include <cstdio>

static void dump(const char* path)
{
    using namespace SWDWG;
    ShapeFile sf = parseShapeFile(path);
    std::printf("%s: %zu shapes\n", path, sf.size());
    for (const auto& kv : sf) {
        std::printf("  code=%d name='%s' segs=%zu\n", kv.first,
                    kv.second.name.c_str(), kv.second.segs.size());
    }
}

int main()
{
    dump("C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/Support/"
         "ltypeshp.shx");
    dump("C:/Program Files/Autodesk/AutoCAD 2016/Fonts/ltypeshp.shx");
    return 0;
}