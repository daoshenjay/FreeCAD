//! Dump every segment of the BAT shape (ltypeshp.shx, code 134) so we can
//! see whether the parser is producing a tiny "eye" outline that the
//! user is seeing on screen as the green artefact.
#include "DWGShapeFile.h"
#include "DWGTypes.h"

#include <cstdio>
#include <vector>

using namespace SWDWG;

int main()
{
    const char* shx = "C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/"
                      "Support/ltypeshp.shx";
    ShapeFile sf = parseShapeFile(shx);
    if (sf.empty()) {
        std::printf("FAIL: cannot parse %s\n", shx);
        return 1;
    }
    auto it = sf.find(134);  // BAT shapecode
    if (it == sf.end()) {
        std::printf("FAIL: BAT (code 134) not in %s\n", shx);
        return 1;
    }
    const auto& segs = it->second.segs;
    std::printf("BAT has %zu segs:\n", segs.size());
    double xMin = 1e300, xMax = -1e300, yMin = 1e300, yMax = -1e300;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const auto& s = segs[i];
        std::printf("  [%2zu] (%.4f, %.4f) -> (%.4f, %.4f)\n",
                    i, s.x0, s.y0, s.x1, s.y1);
        xMin = std::min(xMin, std::min(s.x0, s.x1));
        xMax = std::max(xMax, std::max(s.x0, s.x1));
        yMin = std::min(yMin, std::min(s.y0, s.y1));
        yMax = std::max(yMax, std::max(s.y0, s.y1));
    }
    std::printf("  bbox: x[%.4f, %.4f]  y[%.4f, %.4f]\n",
                xMin, xMax, yMin, yMax);
    return 0;
}
