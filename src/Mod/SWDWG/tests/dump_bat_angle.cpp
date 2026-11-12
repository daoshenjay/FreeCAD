//! Dump the BAT shape segments and the angle of each line from +X, from
//! several ltypeshp.shx copies (UserDataCache, user's Support folder).
#include "DWGShapeFile.h"
#include "DWGTypes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace SWDWG;

static void dumpFile(const char* path)
{
    ShapeFile sf = parseShapeFile(path);
    if (sf.empty()) {
        std::printf("cannot parse %s\n", path);
        return;
    }
    auto it = sf.find(134);
    if (it == sf.end()) {
        std::printf("%s: BAT(134) not found\n", path);
        return;
    }
    std::printf("== %s\n", path);
    const auto& segs = it->second.segs;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (i != 0 && i != 37 && i != 1 && i != 36) continue;
        const auto& s = segs[i];
        const double ang = std::atan2(s.y1 - s.y0, s.x1 - s.x0) * 180.0 / 3.141592653589793;
        const double deg = std::fmod(ang + 360.0, 360.0);
        std::printf("  seg[%2zu] (%.4f, %.4f)->(%.4f, %.4f)  angle=%7.2f deg\n",
                    i, s.x0, s.y0, s.x1, s.y1, deg);
    }
}

int main()
{
    dumpFile("C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/Support/ltypeshp.shx");
    dumpFile("C:/Program Files/Autodesk/AutoCAD 2016/Fonts/ltypeshp.shx");
    dumpFile("C:/Users/angan/AppData/Roaming/Autodesk/AutoCAD 2016/R20.1/chs/Support/ltypeshp.shx");
    return 0;
}
